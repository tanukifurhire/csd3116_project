/* DDS game server -- C++ port of server.c.
 *
 * Behaviour is unchanged from the C version: max 5 players, each with a
 * unique (case-insensitive) name up to 8 characters, collision detection
 * against other players and the window edges, and application-layer
 * authentication independent of the DDS Security cert a client connects
 * with (see auth.c/auth.h).
 */

#include "server.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#include <strings.h> /* strcasecmp */

#include "messages.h"
#include "auth.h"

namespace
{
// "file:" + path, matching the URI scheme DDS Security wants for its
// certificate/governance/permissions properties.
std::string FileUri(const char *path)
{
    return std::string("file:") + path;
}
} // namespace

Server::Server() = default;

// ------------------------------------------------------------------
// DDS Security: creates a participant configured with the Authentication,
// Access Control, and Cryptographic plugins.
// ------------------------------------------------------------------
dds_entity_t Server::CreateSecureParticipant(
    const char *identity_ca_path,
    const char *identity_cert_path,
    const char *private_key_path,
    const char *governance_path,
    const char *permissions_path)
{
    dds_qos_t *qos = dds_create_qos();

    const std::string identity_ca = FileUri(identity_ca_path);
    const std::string identity_cert = FileUri(identity_cert_path);
    const std::string private_key = FileUri(private_key_path);
    const std::string permissions_ca = FileUri(identity_ca_path); /* same CA reused */
    const std::string governance = FileUri(governance_path);
    const std::string permissions = FileUri(permissions_path);

    /* --- Authentication plugin --- */
    dds_qset_prop(qos, "dds.sec.auth.identity_ca", identity_ca.c_str());
    dds_qset_prop(qos, "dds.sec.auth.identity_certificate", identity_cert.c_str());
    dds_qset_prop(qos, "dds.sec.auth.private_key", private_key.c_str());
    dds_qset_prop(qos, "dds.sec.auth.library.path", "dds_security_auth");
    dds_qset_prop(qos, "dds.sec.auth.library.init", "init_authentication");
    dds_qset_prop(qos, "dds.sec.auth.library.finalize", "finalize_authentication");

    /* --- Access Control plugin --- */
    dds_qset_prop(qos, "dds.sec.access.permissions_ca", permissions_ca.c_str());
    dds_qset_prop(qos, "dds.sec.access.governance", governance.c_str());
    dds_qset_prop(qos, "dds.sec.access.permissions", permissions.c_str());
    dds_qset_prop(qos, "dds.sec.access.library.path", "dds_security_ac");
    dds_qset_prop(qos, "dds.sec.access.library.init", "init_access_control");
    dds_qset_prop(qos, "dds.sec.access.library.finalize", "finalize_access_control");

    /* --- Cryptographic plugin --- */
    dds_qset_prop(qos, "dds.sec.crypto.library.path", "dds_security_crypto");
    dds_qset_prop(qos, "dds.sec.crypto.library.init", "init_crypto");
    dds_qset_prop(qos, "dds.sec.crypto.library.finalize", "finalize_crypto");

    const dds_entity_t participant = dds_create_participant(DDS_DOMAIN_DEFAULT, qos, NULL);

    dds_delete_qos(qos);

    return participant;
}

// Caller must hold m_mutex.
bool Server::DetectCollision(int player_id, int x, int y) const
{
    for (int i = 0; i < MAX_PLAYERS; i++)
    {
        // Index + 1 == player ID. Skip the player itself.
        if (i != player_id - 1 && m_players[i].active)
        {
            // Overlap horizontally...
            if (x <= m_players[i].x + SQ_WIDTH && m_players[i].x <= x + SQ_WIDTH)
            {
                // ...and vertically.
                if (y <= m_players[i].y + SQ_WIDTH && m_players[i].y <= y + SQ_WIDTH)
                {
                    return true;
                }
            }
        }
    }
    return false;
}

// Caller must hold m_mutex.
bool Server::IsIdentityTaken(const char *identity) const
{
    for (int i = 0; i < MAX_PLAYERS; i++)
    {
        if (m_players[i].active && strcmp(m_players[i].identity, identity) == 0)
        {
            return true;
        }
    }
    return false;
}

// Case-insensitive check for whether 'name' is already in use by an active
// player. Caller must hold m_mutex.
bool Server::IsNameTaken(const char *name) const
{
    for (int i = 0; i < MAX_PLAYERS; i++)
    {
        if (m_players[i].active && strcasecmp(m_players[i].name, name) == 0)
        {
            return true;
        }
    }
    return false;
}

// Returns the 1-based player ID of the currently-active player using
// 'identity', or -1 if none. Caller must hold m_mutex.
int Server::FindActivePlayerIdByIdentity(const char *identity) const
{
    for (int i = 0; i < MAX_PLAYERS; i++)
    {
        if (m_players[i].active && strcmp(m_players[i].identity, identity) == 0)
        {
            return i + 1;
        }
    }
    return -1;
}

void Server::WorkerThread()
{
    m_participant = CreateSecureParticipant(
        "certs/ca.pem",
        "certs/server.pem",
        "certs/server.key",
        "certs/governance.p7s",
        "certs/permissions.p7s");

    if (m_participant < 0)
    {
        printf("Failed to create participant: %s\n", dds_strretcode(-m_participant));
        return;
    }

    /* ----------------------------------------------------- */
    /* Create topics                                          */
    /* ----------------------------------------------------- */

    const dds_entity_t join_topic = dds_create_topic(
        m_participant, &JoinRequest_desc, "JoinRequest", NULL, NULL);
    const dds_entity_t response_topic = dds_create_topic(
        m_participant, &JoinResponse_desc, "JoinResponse", NULL, NULL);
    const dds_entity_t position_topic = dds_create_topic(
        m_participant, &Position_desc, "Position", NULL, NULL);
    const dds_entity_t input_topic = dds_create_topic(
        m_participant, &Input_desc, "Input", NULL, NULL);
    const dds_entity_t leave_topic = dds_create_topic(
        m_participant, &LeaveRequest_desc, "LeaveRequest", NULL, NULL);

    /* ----------------------------------------------------- */
    /* Create readers                                         */
    /* ----------------------------------------------------- */

    const dds_entity_t join_reader = dds_create_reader(m_participant, join_topic, NULL, NULL);
    const dds_entity_t input_reader = dds_create_reader(m_participant, input_topic, NULL, NULL);
    const dds_entity_t leave_reader = dds_create_reader(m_participant, leave_topic, NULL, NULL);

    /* ----------------------------------------------------- */
    /* Create writers                                         */
    /* ----------------------------------------------------- */

    m_response_writer = dds_create_writer(m_participant, response_topic, NULL, NULL);
    m_position_writer = dds_create_writer(m_participant, position_topic, NULL, NULL);

    /* Pointer to unknown type. */
    void *samples_join[1] = {JoinRequest__alloc()};
    dds_sample_info_t infos_join[1];
    void *samples_input[1] = {Input__alloc()};
    dds_sample_info_t infos_input[1];
    void *samples_leave[1] = {LeaveRequest__alloc()};
    dds_sample_info_t infos_leave[1];

    dds_return_t rc;

    JoinResponse response;
    Position position;

    uint64_t last_time = dds_time();

    /* Poll until data has been read. */
    while (m_running.load())
    {
        // ---- Process a join request ----
        rc = dds_take(join_reader, samples_join, infos_join, 1, 1);
        if (rc < 0)
        {
            DDS_FATAL("dds_take: %s\n", dds_strretcode(-rc));
        }

        if ((rc > 0) && infos_join[0].valid_data)
        {
            printf("Join request received\n");

            JoinRequest *request = static_cast<JoinRequest *>(samples_join[0]);
            printf("Authenticated DDS client: %s\n", request->str_identity);

            char req_name[MAX_NAME_LEN + 1];
            strncpy(req_name, request->str_name, MAX_NAME_LEN);
            req_name[MAX_NAME_LEN] = '\0';

            // JoinResponse is best-effort DDS (dropped samples are not
            // retransmitted), so a client that never saw its own success
            // response keeps resending the same JoinRequest. If that request
            // now looks like "identity already connected" only because OUR
            // OWN earlier accept went unseen, just re-send the original
            // confirmation instead of rejecting a client that, from its own
            // point of view, never joined.
            int existing_player_id;
            bool is_resend_of_own_join;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                existing_player_id = FindActivePlayerIdByIdentity(request->str_identity);
                is_resend_of_own_join =
                    existing_player_id > 0 &&
                    strcasecmp(m_players[existing_player_id - 1].name, req_name) == 0;
            }

            if (is_resend_of_own_join)
            {
                response.int_player_id = existing_player_id;
                strncpy(response.str_name, req_name, MAX_NAME_LEN);
                response.str_name[MAX_NAME_LEN] = '\0';

                rc = dds_write(m_response_writer, &response);
                if (rc != DDS_RETCODE_OK)
                {
                    DDS_FATAL("dds_write: %s\n", dds_strretcode(-rc));
                }

                printf("Re-sent join confirmation to '%s' (identity '%s', player %d) - "
                       "original response was likely lost\n",
                       req_name, request->str_identity, existing_player_id);
            }
            else
            {
                char req_password[65];
                strncpy(req_password, request->str_password, sizeof(req_password) - 1);
                req_password[sizeof(req_password) - 1] = '\0';

                bool name_taken;
                bool identity_taken;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    name_taken = IsNameTaken(req_name);
                    identity_taken = IsIdentityTaken(request->str_identity);
                }

                // Only spend time hashing/verifying credentials if the join
                // would otherwise be accepted (real auth check, independent
                // of which DDS client cert the connection presented).
                AuthResult auth_result = AUTH_OK;
                if (!name_taken && !identity_taken)
                {
                    auth_result = auth_authenticate(req_name, req_password);
                }
                memset(req_password, 0, sizeof(req_password));

                if (!name_taken && !identity_taken && auth_result == AUTH_OK)
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    strncpy(m_pending_name, req_name, MAX_NAME_LEN);
                    m_pending_name[MAX_NAME_LEN] = '\0';
                    strncpy(m_pending_identity, request->str_identity, sizeof(m_pending_identity) - 1);
                    m_pending_identity[sizeof(m_pending_identity) - 1] = '\0';
                }

                if (identity_taken)
                {
                    response.int_player_id = -2;
                    strncpy(response.str_name, req_name, MAX_NAME_LEN);
                    response.str_name[MAX_NAME_LEN] = '\0';

                    rc = dds_write(m_response_writer, &response);
                    if (rc != DDS_RETCODE_OK)
                    {
                        DDS_FATAL("dds_write: %s\n", dds_strretcode(-rc));
                    }

                    printf("Identity '%s' already connected\n", request->str_identity);
                }
                else if (name_taken)
                {
                    response.int_player_id = -1;
                    strncpy(response.str_name, req_name, MAX_NAME_LEN);
                    response.str_name[MAX_NAME_LEN] = '\0';

                    rc = dds_write(m_response_writer, &response);
                    if (rc != DDS_RETCODE_OK)
                    {
                        DDS_FATAL("dds_write: %s\n", dds_strretcode(-rc));
                    }

                    printf("Name '%s' already taken - join rejected\n", req_name);
                }
                else if (auth_result != AUTH_OK)
                {
                    response.int_player_id = -3;
                    strncpy(response.str_name, req_name, MAX_NAME_LEN);
                    response.str_name[MAX_NAME_LEN] = '\0';

                    rc = dds_write(m_response_writer, &response);
                    if (rc != DDS_RETCODE_OK)
                    {
                        DDS_FATAL("dds_write: %s\n", dds_strretcode(-rc));
                    }

                    printf("Wrong password for '%s' - join rejected\n", req_name);
                }
                else
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_player_join = true;
                }
            }
        }

        // ---- If Run() has assigned a new player ID, send the response ----
        int new_player_id_temp;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            new_player_id_temp = m_new_player_id;
        }
        if (new_player_id_temp != -1)
        {
            char resp_name[MAX_NAME_LEN + 1];
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                // Reset the player ID.
                m_new_player_id = -1;
                // Grab the name this join was for, to echo it in the
                // response. Must be m_admitted_name, not m_pending_name:
                // the latter may already have been overwritten by a newer
                // join accepted by this same worker thread before this
                // response went out.
                strncpy(resp_name, m_admitted_name, MAX_NAME_LEN);
                resp_name[MAX_NAME_LEN] = '\0';
            }

            // The player ID can be 0, i.e. invalid (server full).
            response.int_player_id = new_player_id_temp;
            strncpy(response.str_name, resp_name, MAX_NAME_LEN);
            response.str_name[MAX_NAME_LEN] = '\0';

            rc = dds_write(m_response_writer, &response);
            if (rc != DDS_RETCODE_OK)
            {
                DDS_FATAL("dds_write: %s\n", dds_strretcode(-rc));
            }
            printf("Join response sent\n");
            /* Polling sleep. */
            dds_sleepfor(DDS_MSECS(20));
        }

        // ---- Broadcast every active player's position, at most 10x/sec ----
        const uint64_t curr_time = dds_time();
        if (curr_time - last_time > DDS_SECS(0.1))
        {
            last_time = curr_time;

            for (int i = 0; i < MAX_PLAYERS; i++)
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_players[i].active)
                {
                    position.int_player_id = i + 1;
                    position.int_x = m_players[i].x;
                    position.int_y = m_players[i].y;
                    position.b_active = true;
                    strncpy(position.str_name, m_players[i].name, MAX_NAME_LEN);
                    position.str_name[MAX_NAME_LEN] = '\0';

                    rc = dds_write(m_position_writer, &position);
                    if (rc != DDS_RETCODE_OK)
                    {
                        DDS_FATAL("dds_write: %s\n", dds_strretcode(-rc));
                    }
                    printf("Position sent\n");
                    /* Polling sleep. */
                    dds_sleepfor(DDS_MSECS(20));
                }
            }
        }

        // ---- Listen for input ----
        rc = dds_take(input_reader, samples_input, infos_input, 1, 1);
        if (rc < 0)
        {
            DDS_FATAL("dds_take: %s\n", dds_strretcode(-rc));
        }

        if ((rc > 0) && infos_input[0].valid_data)
        {
            printf("Input received\n");
            Input *input = static_cast<Input *>(samples_input[0]);
            const int input_player_id = input->int_player_id;
            const char input_dir = input->ch_direction;

            int x;
            int y;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                x = m_players[input_player_id - 1].x;
                y = m_players[input_player_id - 1].y;
            }

            switch (input_dir)
            {
            case 'u': y -= STEP; break;
            case 'd': y += STEP; break;
            case 'l': x -= STEP; break;
            case 'r': x += STEP; break;
            default: break;
            }

            // Check collision with the window...
            if (x >= 0 && x <= WIDTH - SQ_WIDTH && y >= 0 && y <= HEIGHT - SQ_WIDTH)
            {
                // ...then with other players.
                std::lock_guard<std::mutex> lock(m_mutex);
                if (!DetectCollision(input_player_id, x, y))
                {
                    m_players[input_player_id - 1].x = x;
                    m_players[input_player_id - 1].y = y;
                }
            }
        }

        // ---- Process a leave request ----
        rc = dds_take(leave_reader, samples_leave, infos_leave, 1, 1);
        if (rc < 0)
        {
            DDS_FATAL("dds_take: %s\n", dds_strretcode(-rc));
        }

        if ((rc > 0) && infos_leave[0].valid_data)
        {
            LeaveRequest *leave = static_cast<LeaveRequest *>(samples_leave[0]);
            const int id = leave->int_player_id;

            std::lock_guard<std::mutex> lock(m_mutex);
            if (id > 0 && id <= MAX_PLAYERS)
            {
                position.int_player_id = id;
                position.int_x = 0;
                position.int_y = 0;
                position.b_active = false;
                position.str_name[0] = '\0';

                dds_write(m_position_writer, &position);

                m_players[id - 1].active = false;
                m_players[id - 1].name[0] = '\0';
                m_players[id - 1].identity[0] = '\0';

                m_num_active_players--;

                printf("Player %d left (%s)\n", id, leave->str_identity);
            }
        }
    }
}

bool Server::Init()
{
    // Seed rand(), so that the random spawn points differ between runs.
    srand(static_cast<unsigned int>(time(NULL)));

    if (!auth_init("players.auth"))
    {
        fprintf(stderr, "Failed to open/create players.auth\n");
        return false;
    }

    m_running = true;
    m_worker = std::thread(&Server::WorkerThread, this);

    return true;
}

void Server::Run()
{
    printf("Waiting...\n");

    while (m_running.load())
    {
        // If a join request has been accepted by WorkerThread()...
        bool player_join_temp;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            player_join_temp = m_player_join;
        }

        if (player_join_temp)
        {
            // Reset the flag.
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_player_join = false;
            }

            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_num_active_players == MAX_PLAYERS)
            {
                // Set an invalid player ID: server is full.
                m_new_player_id = 0;
            }
            else
            {
                for (int i = 0; i < MAX_PLAYERS; i++)
                {
                    if (m_players[i].active)
                    {
                        continue;
                    }

                    m_players[i].active = true;
                    m_num_active_players++;

                    // Record the name/identity this player joined with.
                    strncpy(m_players[i].name, m_pending_name, MAX_NAME_LEN);
                    m_players[i].name[MAX_NAME_LEN] = '\0';
                    strncpy(m_players[i].identity, m_pending_identity, MAX_IDENTITY_LEN - 1);
                    m_players[i].identity[MAX_IDENTITY_LEN - 1] = '\0';

                    // Calculate a spawn point clear of every other player.
                    int rand_x = 0;
                    int rand_y = 0;
                    while (true)
                    {
                        rand_x = rand() % (WIDTH - SQ_WIDTH + 1);
                        rand_y = rand() % (HEIGHT - SQ_WIDTH + 1);
                        // DetectCollision doesn't need its own lock/unlock;
                        // this loop already holds m_mutex.
                        if (!DetectCollision(i + 1, rand_x, rand_y))
                        {
                            break;
                        }
                    }

                    m_players[i].x = rand_x;
                    m_players[i].y = rand_y;

                    printf("Player %d admitted (name=%s identity=%s)\n",
                           i + 1, m_players[i].name, m_players[i].identity);

                    // Snapshot the name alongside the ID, atomically, so
                    // WorkerThread() echoes back the name that actually
                    // matches this player ID (see comment on
                    // m_admitted_name in server.h).
                    strncpy(m_admitted_name, m_players[i].name, MAX_NAME_LEN);
                    m_admitted_name[MAX_NAME_LEN] = '\0';

                    m_new_player_id = i + 1;
                    break;
                }
            }
        }

        // A short, polite sleep -- the original process busy-spun this loop.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void Server::Shutdown()
{
    if (!m_running.exchange(false))
    {
        return; // already shut down (or never started)
    }

    if (m_worker.joinable())
    {
        m_worker.join();
    }

    if (m_participant > 0)
    {
        // Deletes every topic/reader/writer created under it too.
        dds_delete(m_participant);
        m_participant = 0;
    }
}
