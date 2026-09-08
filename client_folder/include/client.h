#ifndef CLIENT_H
#define CLIENT_H

#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "dds/dds.h"

#include "gameobject.h"
#include "renderer.h"

class Client
{
public:
    Client();

    /* 'client_id' selects which certs/clientN.{pem,key} this instance
     * authenticates as (see gen_certs.sh) and becomes its DDS identity,
     * "clientN". Prompts on stdin for a player name and password (same
     * console dance as client.c), brings up the renderer, and starts the
     * secure DDS participant on a network worker thread. */
    bool Init(const std::string &client_id);

    /* Runs the render/input loop until the window is closed, then sends a
     * LeaveRequest if this client had been admitted. */
    void Run();

    void Shutdown();

private:
    static const int MAX_PLAYERS = 5;
    static const int MAX_NAME_LEN = 8;
    static const int WINDOW_WIDTH = 800;
    static const int WINDOW_HEIGHT = 600;
    static const int SQ_WIDTH = 50;

    void HandleInput(float dt);
    void Update(float dt);
    void Draw();

    // --- Networking, ported from client.c ---
    void WorkerThread();
    void SendLeaveRequest();

    static dds_entity_t CreateSecureParticipant(const char *identity_ca_path,
                                                 const char *identity_cert_path,
                                                 const char *private_key_path,
                                                 const char *governance_path,
                                                 const char *permissions_path);

    // Prompt on stdin for a name, truncated to MAX_NAME_LEN chars. Falls
    // back to "Player" if the user just hits enter.
    static void PromptForName(std::string &out_name);

    // Prompt on stdin for a password with terminal echo disabled, truncated
    // to 64 chars. Restores echo before returning even if reading fails.
    static void PromptForPassword(std::string &out_password);

    // One entry per player slot (index == player_id - 1), filled in by
    // WorkerThread() from incoming Position samples and consumed by
    // Update(). Deliberately separate from m_objects: the pool is only
    // ever touched from the main thread (Update()/Draw()), so this plain,
    // mutex-guarded snapshot is how position updates cross from the
    // network thread to the main one.
    struct PlayerSnapshot
    {
        bool active = false;
        int x = 0;
        int y = 0;
    };

    Renderer m_renderer;

    /* One pool slot per player. A slot's GameObject::owner_id is the
     * 1-based player ID the server assigned it, per the scheme described
     * in gameobject.h. Every object here -- including this client's own
     * square -- is driven purely by Position samples from the server; the
     * server, not this client, is authoritative for movement and
     * collision. */
    GameObjectPool m_objects;

    std::mutex m_mutex;

    // --- Guarded by m_mutex ---
    std::array<PlayerSnapshot, MAX_PLAYERS> m_snapshot;
    int m_player_id = -1;    // -1 until the server admits us
    char m_direction = 'n';  // 'u'/'d'/'l'/'r'/'n', set by HandleInput()
    std::string m_name;
    std::string m_password;
    // --- end guarded members ---

    std::string m_identity;   // "clientN", sent as this client's app-layer identity
    std::string m_client_id;  // "N", selects certs/clientN.{pem,key}
    std::string m_last_title; // last title passed to glfwSetWindowTitle, so we only call it on change

    dds_entity_t m_participant = 0;
    dds_entity_t m_join_writer = 0;
    dds_entity_t m_input_writer = 0;
    dds_entity_t m_leave_writer = 0;

    std::thread m_worker;
    std::atomic<bool> m_running{false};
};

#endif /* CLIENT_H */
