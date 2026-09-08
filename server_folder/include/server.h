#ifndef SERVER_H
#define SERVER_H

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>

#include "dds/dds.h"

/* DDS game server, ported from server.c.
 *
 * Two threads split the work, same as the C version:
 *  - WorkerThread() owns every DDS entity: it processes JoinRequest/Input/
 *    LeaveRequest samples and publishes JoinResponse/Position samples.
 *  - Run(), on whichever thread calls it (main(), typically), owns admitting
 *    a newly-validated join into a free player slot and picking its spawn
 *    point. This split exists so slot admission (which needs to scan every
 *    player for a collision-free spawn) never blocks the network thread.
 *
 * All shared state below is guarded by m_mutex; comments call out which
 * members those are.
 */
class Server
{
public:
    Server();

    /* Opens/creates players.auth, brings up the secure DDS participant,
     * topics, readers and writers, and starts the network worker thread.
     * Returns false if any of that fails. */
    bool Init();

    /* Player-admission loop. Blocks until Shutdown() is called (from another
     * thread, e.g. a signal handler) -- this is the counterpart to the
     * original process's `while (1)` in main(). */
    void Run();

    /* Stops Run()'s loop and the network worker thread, and tears down the
     * DDS participant (which takes every topic/reader/writer with it). Safe
     * to call more than once. */
    void Shutdown();

private:
    static const int MAX_PLAYERS = 5;
    static const int MAX_NAME_LEN = 8;
    static const int MAX_IDENTITY_LEN = 32;
    static const int WIDTH = 800;
    static const int HEIGHT = 600;
    static const int SQ_WIDTH = 50;
    static const int STEP = 1;

    // Player coord is defined as the NW corner of the square, same as the C
    // version's arr_Players entries.
    struct Player
    {
        bool active = false;
        int x = 0;
        int y = 0;
        char name[MAX_NAME_LEN + 1] = {0};
        char identity[MAX_IDENTITY_LEN] = {0};
        // GUID of the DDS participant this player joined from, resolved
        // from the JoinRequest's publication handle at admission time. Lets
        // CheckParticipantLiveliness() notice this participant disappearing
        // (crash, kill -9, network loss) even if it never sends a
        // LeaveRequest. All-zero means it couldn't be resolved at join time
        // (see the warning printed there) -- such a player is only ever
        // cleaned up by an explicit LeaveRequest, same as before this
        // feature existed.
        dds_guid_t participant_key = {};
    };

    void WorkerThread();

    static dds_entity_t CreateSecureParticipant(const char *identity_ca_path,
                                                 const char *identity_cert_path,
                                                 const char *private_key_path,
                                                 const char *governance_path,
                                                 const char *permissions_path);

    // Caller must hold m_mutex for all of the following.
    bool DetectCollision(int player_id, int x, int y) const;
    bool IsIdentityTaken(const char *identity) const;
    bool IsNameTaken(const char *name) const;
    int FindActivePlayerIdByIdentity(const char *identity) const;

    // Caller must hold m_mutex. Marks a slot inactive, publishes its "gone"
    // Position sample, and updates the active-player count -- the cleanup
    // shared by an explicit LeaveRequest and CheckParticipantLiveliness()
    // detecting the player's participant disappeared without sending one.
    void EvictPlayer(int slot_index, const char *reason);

    // Takes every currently-available sample from the DCPSParticipant
    // built-in topic reader. Any participant reported not-alive whose key
    // matches an active player's participant_key is evicted via
    // EvictPlayer(); locks m_mutex itself only while actually matching/
    // evicting, so the caller must NOT be holding it already.
    void CheckParticipantLiveliness(dds_entity_t participant_reader);

    mutable std::mutex m_mutex;

    // --- Guarded by m_mutex ---
    std::array<Player, MAX_PLAYERS> m_players;
    int m_num_active_players = 0;

    // Set by WorkerThread() when a join has passed every check and just
    // needs a free slot; cleared by Run() once it has processed it.
    bool m_player_join = false;

    // Player ID assigned by Run() for the join currently being processed.
    // -1 = nothing new, 0 = server full, 1..MAX_PLAYERS = admitted.
    int m_new_player_id = -1;

    // Name/identity for the join WorkerThread() just accepted as unique;
    // read by Run() when it assigns the ID.
    char m_pending_name[MAX_NAME_LEN + 1] = {0};
    char m_pending_identity[MAX_IDENTITY_LEN] = {0};
    // GUID of the joining participant, resolved via dds_get_matched_publication_data()
    // at the same point pending_name/pending_identity are captured. All-zero
    // if resolution failed -- see the comment on Player::participant_key.
    dds_guid_t m_pending_participant_key = {};

    // Name of whichever player m_new_player_id refers to, set by Run() at
    // the exact moment it assigns the ID. Kept separate from
    // m_pending_name because that buffer can be overwritten by
    // WorkerThread() accepting a *different* join before this admission's
    // response has gone out -- using m_pending_name here would risk echoing
    // the wrong player's name back with this player's ID.
    char m_admitted_name[MAX_NAME_LEN + 1] = {0};
    // --- end guarded members ---

    // Owned solely by WorkerThread() once Init() starts it.
    dds_entity_t m_participant = 0;
    dds_entity_t m_response_writer = 0;
    dds_entity_t m_position_writer = 0;

    std::thread m_worker;
    std::atomic<bool> m_running{false};
};

#endif /* SERVER_H */
