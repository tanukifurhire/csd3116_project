#include "client.h"

#include <cstdio>
#include <cstring>
#include <iostream>

#include <strings.h>  /* strcasecmp */
#include <termios.h>
#include <unistd.h>

#include "messages.h"

Client::Client()
    : m_objects(MAX_PLAYERS)
{
}

std::atomic<bool> Client::s_stop_requested{false};

void Client::RequestStop()
{
    // Only ever called from a signal handler (see main.cpp). Signal
    // handlers may only safely call a small set of async-signal-safe
    // functions; a lock-free atomic store is one of the few library
    // operations that qualifies. std::atomic<bool> is lock-free on every
    // platform this project targets (m_running already relies on the same
    // property), so this is safe despite not being a plain sig_atomic_t.
    s_stop_requested.store(true);
}

// ------------------------------------------------------------------
// DDS Security: creates a participant configured with the Authentication,
// Access Control, and Cryptographic plugins. Identical setup to the
// server's, except each client presents its own certs/clientN.* identity.
// ------------------------------------------------------------------
dds_entity_t Client::CreateSecureParticipant(
    const char *identity_ca_path,
    const char *identity_cert_path,
    const char *private_key_path,
    const char *governance_path,
    const char *permissions_path)
{
    dds_qos_t *qos = dds_create_qos();

    const std::string identity_ca = std::string("file:") + identity_ca_path;
    const std::string identity_cert = std::string("file:") + identity_cert_path;
    const std::string private_key = std::string("file:") + private_key_path;
    const std::string permissions_ca = std::string("file:") + identity_ca_path; /* same CA reused */
    const std::string governance = std::string("file:") + governance_path;
    const std::string permissions = std::string("file:") + permissions_path;

    dds_qset_prop(qos, "dds.sec.auth.identity_ca", identity_ca.c_str());
    dds_qset_prop(qos, "dds.sec.auth.identity_certificate", identity_cert.c_str());
    dds_qset_prop(qos, "dds.sec.auth.private_key", private_key.c_str());
    dds_qset_prop(qos, "dds.sec.auth.library.path", "dds_security_auth");
    dds_qset_prop(qos, "dds.sec.auth.library.init", "init_authentication");
    dds_qset_prop(qos, "dds.sec.auth.library.finalize", "finalize_authentication");

    dds_qset_prop(qos, "dds.sec.access.permissions_ca", permissions_ca.c_str());
    dds_qset_prop(qos, "dds.sec.access.governance", governance.c_str());
    dds_qset_prop(qos, "dds.sec.access.permissions", permissions.c_str());
    dds_qset_prop(qos, "dds.sec.access.library.path", "dds_security_ac");
    dds_qset_prop(qos, "dds.sec.access.library.init", "init_access_control");
    dds_qset_prop(qos, "dds.sec.access.library.finalize", "finalize_access_control");

    dds_qset_prop(qos, "dds.sec.crypto.library.path", "dds_security_crypto");
    dds_qset_prop(qos, "dds.sec.crypto.library.init", "init_crypto");
    dds_qset_prop(qos, "dds.sec.crypto.library.finalize", "finalize_crypto");

    const dds_entity_t participant = dds_create_participant(DDS_DOMAIN_DEFAULT, qos, NULL);

    dds_delete_qos(qos);

    return participant;
}

void Client::PromptForName(std::string &out_name)
{
    std::string input;
    if (!std::getline(std::cin, input))
    {
        input.clear();
    }
    if (input.size() > static_cast<size_t>(MAX_NAME_LEN))
    {
        input.resize(MAX_NAME_LEN);
    }
    out_name = input.empty() ? "Player" : input;
}

void Client::PromptForPassword(std::string &out_password)
{
    termios old_term{};
    termios new_term{};
    const bool have_term = (tcgetattr(STDIN_FILENO, &old_term) == 0);

    if (have_term)
    {
        new_term = old_term;
        new_term.c_lflag &= ~ECHO;
        new_term.c_lflag |= ECHONL;
        tcsetattr(STDIN_FILENO, TCSANOW, &new_term);
    }

    std::string input;
    std::getline(std::cin, input);

    if (have_term)
    {
        tcsetattr(STDIN_FILENO, TCSANOW, &old_term);
    }

    if (input.size() > 64)
    {
        input.resize(64);
    }
    out_password = input;
}

bool Client::Init(const std::string &client_id)
{
    m_client_id = client_id;
    m_identity = "client" + client_id;

    std::cout << "Using certs/client" << client_id << ".pem for authentication\n";

    // Ask for the player's name BEFORE anything else happens -- this is
    // what gets sent in the join request, so it must be known first.
    std::cout << "Enter your player name (max " << MAX_NAME_LEN << " characters): ";
    std::cout.flush();
    PromptForName(m_name);

    // Password proves WHO is playing at the application layer (a new name
    // registers it, an existing name must match). Sent over the JoinRequest
    // topic, which governance.xml already marks ENCRYPT under DDS Security.
    std::cout << "Enter password for '" << m_name << "': ";
    std::cout.flush();
    PromptForPassword(m_password);

    std::cout << "Welcome, " << m_name << "! DDS identity = " << m_identity << "\n";

    if (s_stop_requested.load())
    {
        std::cout << "Interrupted before startup completed -- exiting.\n";
        return false;
    }

    if (!m_renderer.init(WINDOW_WIDTH, WINDOW_HEIGHT, "DDS Client (C++)"))
    {
        std::cerr << "Renderer failed to initialise" << std::endl;
        return false;
    }

    m_running = true;
    m_worker = std::thread(&Client::WorkerThread, this);

    return true;
}

void Client::WorkerThread()
{
    const std::string cert_path = "certs/client" + m_client_id + ".pem";
    const std::string key_path = "certs/client" + m_client_id + ".key";

    m_participant = CreateSecureParticipant(
        "certs/ca.pem",
        cert_path.c_str(),
        key_path.c_str(),
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
    const dds_entity_t input_topic = dds_create_topic(
        m_participant, &Input_desc, "Input", NULL, NULL);
    const dds_entity_t position_topic = dds_create_topic(
        m_participant, &Position_desc, "Position", NULL, NULL);
    const dds_entity_t leave_topic = dds_create_topic(
        m_participant, &LeaveRequest_desc, "LeaveRequest", NULL, NULL);

    /* ----------------------------------------------------- */
    /* Create writers                                         */
    /* ----------------------------------------------------- */

    m_join_writer = dds_create_writer(m_participant, join_topic, NULL, NULL);
    m_input_writer = dds_create_writer(m_participant, input_topic, NULL, NULL);
    m_leave_writer = dds_create_writer(m_participant, leave_topic, NULL, NULL);

    /* ----------------------------------------------------- */
    /* Create readers                                         */
    /* ----------------------------------------------------- */

    const dds_entity_t response_reader = dds_create_reader(m_participant, response_topic, NULL, NULL);
    const dds_entity_t position_reader = dds_create_reader(m_participant, position_topic, NULL, NULL);

    JoinRequest request;
    request.b_dummy = true;
    strncpy(request.str_identity, m_identity.c_str(), sizeof(request.str_identity) - 1);
    request.str_identity[sizeof(request.str_identity) - 1] = '\0';
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        strncpy(request.str_name, m_name.c_str(), MAX_NAME_LEN);
        request.str_name[MAX_NAME_LEN] = '\0';
        strncpy(request.str_password, m_password.c_str(), sizeof(request.str_password) - 1);
        request.str_password[sizeof(request.str_password) - 1] = '\0';
    }

    void *samples_response[1] = {JoinResponse__alloc()};
    dds_sample_info_t infos_response[1];
    void *samples_position[1] = {Position__alloc()};
    dds_sample_info_t infos_position[1];
    dds_return_t rc;

    uint64_t last_join_req_time = dds_time();

    while (m_running.load())
    {
        // Reflects whether the server's Position writer currently exists,
        // not whether we've received data recently -- a quiet server (no
        // one moving) is not a disconnection, but the writer going away
        // (server process exiting, crashing, or a lease-duration liveliness
        // timeout after the network drops) is. Cheap local status query,
        // no network round-trip, safe to call every iteration.
        dds_subscription_matched_status_t matched_status;
        if (dds_get_subscription_matched_status(position_reader, &matched_status) == DDS_RETCODE_OK)
        {
            const bool now_connected = matched_status.current_count > 0;
            if (now_connected != m_connected.load())
            {
                m_connected.store(now_connected);
                printf(now_connected ? "Connected to server\n"
                                      : "Lost connection to server\n");
            }
        }

        int player_id_temp;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            player_id_temp = m_player_id;
        }

        // Not in a game yet.
        if (player_id_temp == -1)
        {
            // Listen for the server's join response.
            rc = dds_take(response_reader, samples_response, infos_response, 1, 1);
            if (rc < 0)
            {
                DDS_FATAL("dds_take: %s\n", dds_strretcode(-rc));
            }

            if ((rc > 0) && infos_response[0].valid_data)
            {
                JoinResponse *response = static_cast<JoinResponse *>(samples_response[0]);

                std::string my_name;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    my_name = m_name;
                }

                // Multiple clients share this reader/topic, so only react
                // to a response addressed to the name we requested.
                if (strcasecmp(response->str_name, my_name.c_str()) == 0)
                {
                    printf("Join response received\n");
                    const int resp_player_id = response->int_player_id;

                    if (resp_player_id == 0)
                    {
                        // Server is full.
                        printf("Unable to join: server is full\n");
                    }
                    else if (resp_player_id == -1)
                    {
                        // Name already taken -- ask for another one and retry.
                        printf("Name '%s' is already taken. Enter a different name (max %d chars): ",
                               my_name.c_str(), MAX_NAME_LEN);
                        fflush(stdout);

                        std::string new_name;
                        PromptForName(new_name);
                        {
                            std::lock_guard<std::mutex> lock(m_mutex);
                            m_name = new_name;
                        }
                        strncpy(request.str_name, new_name.c_str(), MAX_NAME_LEN);
                        request.str_name[MAX_NAME_LEN] = '\0';

                        printf("Trying to join as '%s'...\n", new_name.c_str());

                        // Force an immediate re-send of the join request.
                        last_join_req_time = 0;
                    }
                    else if (resp_player_id == -2)
                    {
                        printf("This DDS certificate is already connected.\n");
                    }
                    else if (resp_player_id == -3)
                    {
                        // Wrong password for this (existing) username -- retry.
                        printf("Wrong password for '%s'. Enter password again: ", my_name.c_str());
                        fflush(stdout);

                        std::string new_password;
                        PromptForPassword(new_password);
                        {
                            std::lock_guard<std::mutex> lock(m_mutex);
                            m_password = new_password;
                        }
                        strncpy(request.str_password, new_password.c_str(), sizeof(request.str_password) - 1);
                        request.str_password[sizeof(request.str_password) - 1] = '\0';

                        printf("Trying to join as '%s'...\n", my_name.c_str());
                        last_join_req_time = 0;
                    }
                    else
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        m_player_id = resp_player_id;
                        printf("Joined as Player %d (%s)\n", resp_player_id, my_name.c_str());
                    }
                }
            }
            else
            {
                /* Polling sleep. */
                dds_sleepfor(DDS_MSECS(20));
            }

            // Send the join request every 1s until admitted.
            const uint64_t curr_time = dds_time();
            if (curr_time - last_join_req_time > DDS_SECS(1))
            {
                last_join_req_time = curr_time;

                rc = dds_write(m_join_writer, &request);
                printf("Join request sent (name='%s', identity='%s')\n",
                       request.str_name, request.str_identity);

                if (rc != DDS_RETCODE_OK)
                {
                    DDS_FATAL("dds_write: %s\n", dds_strretcode(-rc));
                }
            }
        }
        // Client is an active player.
        else
        {
            char dir_temp;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                dir_temp = m_direction;
            }

            if (dir_temp != 'n')
            {
                Input input;
                input.int_player_id = player_id_temp;
                input.ch_direction = dir_temp;

                rc = dds_write(m_input_writer, &input);
                if (rc != DDS_RETCODE_OK)
                {
                    DDS_FATAL("dds_write: %s\n", dds_strretcode(-rc));
                }
                printf("Input sent\n");
                // Slow input down, or the player would reach the window
                // limits in a single frame.
                dds_sleepfor(DDS_MSECS(10));
            }
        }

        // Listen for position updates -- every player's, including ours;
        // the server is authoritative for movement and collision. Drain
        // everything currently available so a burst of updates can't build
        // up a backlog behind a single sample-per-iteration read.
        do
        {
            rc = dds_take(position_reader, samples_position, infos_position, 1, 1);
            if (rc < 0)
            {
                DDS_FATAL("dds_take: %s\n", dds_strretcode(-rc));
            }

            if ((rc > 0) && infos_position[0].valid_data)
            {
                printf("Position received\n");
                Position *position = static_cast<Position *>(samples_position[0]);
                const int position_player_id = position->int_player_id;

                if (position_player_id >= 1 && position_player_id <= MAX_PLAYERS)
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    PlayerSnapshot &slot = m_snapshot[position_player_id - 1];
                    slot.active = position->b_active;
                    if (position->b_active)
                    {
                        slot.x = position->int_x;
                        slot.y = position->int_y;
                        // str_name is a fixed 9-byte buffer; bound the read
                        // in case it's ever not null-terminated.
                        slot.name.assign(position->str_name,
                                         strnlen(position->str_name, sizeof(position->str_name)));
                    }
                }
            }
        } while (rc > 0);
    }
}

void Client::HandleInput(float dt)
{
    (void)dt;

    GLFWwindow *window = m_renderer.window();

    if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
    {
        glfwSetWindowShouldClose(window, GLFW_TRUE);
    }

    char dir = 'n';
    if (glfwGetKey(window, GLFW_KEY_UP) == GLFW_PRESS)
    {
        dir = 'u';
    }
    else if (glfwGetKey(window, GLFW_KEY_DOWN) == GLFW_PRESS)
    {
        dir = 'd';
    }
    else if (glfwGetKey(window, GLFW_KEY_LEFT) == GLFW_PRESS)
    {
        dir = 'l';
    }
    else if (glfwGetKey(window, GLFW_KEY_RIGHT) == GLFW_PRESS)
    {
        dir = 'r';
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_direction = dir;
}

void Client::Update(float dt)
{
    (void)dt;

    std::array<PlayerSnapshot, MAX_PLAYERS> snapshot;
    int player_id;
    std::string name;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        snapshot = m_snapshot;
        player_id = m_player_id;
        name = m_name;
    }

    // Reconcile the pool with the latest snapshot. This is the only place
    // m_objects is touched, which is what keeps the pool safe to use
    // without its own locking -- see the comment on PlayerSnapshot.
    for (int i = 0; i < MAX_PLAYERS; i++)
    {
        const int32_t owner_id = i + 1;
        GameObjectHandle handle = m_objects.find_by_owner(owner_id);

        if (snapshot[i].active)
        {
            GameObject *object = m_objects.get(handle);
            if (object == nullptr)
            {
                handle = m_objects.acquire();
                object = m_objects.get(handle);
                if (object == nullptr)
                {
                    continue; // pool exhausted -- shouldn't happen at MAX_PLAYERS capacity
                }

                object->owner_id = owner_id;
                object->w = static_cast<float>(SQ_WIDTH);
                object->h = static_cast<float>(SQ_WIDTH);

                // Colour still distinguishes you from everyone else at a
                // glance (matches client.c's convention); the name label
                // drawn above the square in Draw() supplements this.
                if (owner_id == player_id)
                {
                    object->r = 1.0f;
                    object->g = 0.0f;
                    object->b = 0.0f;
                }
                else
                {
                    object->r = 0.1f;
                    object->g = 0.3f;
                    object->b = 0.9f;
                }
            }

            object->x = static_cast<float>(snapshot[i].x);
            object->y = static_cast<float>(snapshot[i].y);
            // Refresh in case the player renamed mid-session (re-joined
            // under a different name) or this slot was just (re)claimed.
            m_names[i] = snapshot[i].name;
        }
        else if (handle.valid())
        {
            m_objects.release(handle);
        }
    }

    // The window title stands in for client.c's on-screen banner.
    std::string title;
    if (player_id != -1)
    {
        title = IsConnected()
            ? ("Player " + std::to_string(player_id) + " (" + name + ")")
            : "Disconnected from server!";
    }
    else
    {
        title = "Waiting for server...";
    }

    if (title != m_last_title)
    {
        glfwSetWindowTitle(m_renderer.window(), title.c_str());
        m_last_title = title;
    }
}

void Client::Draw()
{
    int player_id;
    std::string name;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        player_id = m_player_id;
        name = m_name;
    }

    m_renderer.begin_frame(1.0f, 1.0f, 1.0f);

    m_objects.for_each_active([&](const GameObject &object) {
        m_renderer.draw_quad_outline(object.x, object.y, object.w, object.h, 2.0f,
                                     object.r, object.g, object.b, object.a);

        // Our own name comes straight from m_name; everyone else's comes
        // from Position::str_name via m_names, refreshed each frame in
        // Update(). Falls back to a per-ID label only if a name hasn't
        // arrived yet (e.g. the very first frame a slot becomes active).
        const std::string &other_name = m_names[object.owner_id - 1];
        const std::string label = (object.owner_id == player_id)
            ? name
            : (other_name.empty() ? ("P" + std::to_string(object.owner_id)) : other_name);

        constexpr float TEXT_SCALE = 2.0f; // pixel size of each font dot
        constexpr float TEXT_GAP = 4.0f;   // gap between square top and text

        const float text_w = m_renderer.text_width(label, TEXT_SCALE);
        const float text_x = object.x + (object.w - text_w) * 0.5f; // centered
        const float text_y = object.y - TEXT_GAP -
                             static_cast<float>(Renderer::GLYPH_HEIGHT) * TEXT_SCALE;

        m_renderer.draw_text(text_x, text_y, label, TEXT_SCALE,
                             object.r, object.g, object.b, object.a);
    });

    // A title-bar message alone is easy to miss while actually looking at
    // the game, so also flag it on-screen once we've joined and then lost
    // the server (not before joining -- "Waiting for server..." already
    // covers that case in the title).
    if (player_id != -1 && !IsConnected())
    {
        const std::string banner = "DISCONNECTED FROM SERVER";
        constexpr float BANNER_SCALE = 3.0f;
        constexpr float BANNER_Y = 20.0f;

        const float banner_w = m_renderer.text_width(banner, BANNER_SCALE);
        const float banner_x = (static_cast<float>(WINDOW_WIDTH) - banner_w) * 0.5f;

        m_renderer.draw_text(banner_x, BANNER_Y, banner, BANNER_SCALE, 0.8f, 0.0f, 0.0f, 1.0f);
    }

    m_renderer.end_frame();
}

void Client::Run()
{
    double last_time = glfwGetTime();

    while (!m_renderer.should_close() && !s_stop_requested.load())
    {
        /* Frame-rate independent movement: scale by how long the last frame
         * took instead of moving a fixed number of pixels per iteration. */
        const double now = glfwGetTime();
        const float dt = static_cast<float>(now - last_time);
        last_time = now;

        HandleInput(dt);
        Update(dt);
        Draw();
    }

    SendLeaveRequest();
}

void Client::SendLeaveRequest()
{
    int player_id;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        player_id = m_player_id;
    }

    if (player_id == -1)
    {
        return; // never got admitted -- nothing to tell the server
    }

    LeaveRequest leave;
    leave.int_player_id = player_id;
    strncpy(leave.str_identity, m_identity.c_str(), sizeof(leave.str_identity) - 1);
    leave.str_identity[sizeof(leave.str_identity) - 1] = '\0';

    dds_write(m_leave_writer, &leave);
    printf("Leave request sent\n");
}

void Client::Shutdown()
{
    if (m_running.exchange(false))
    {
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

    m_objects.clear();
    m_renderer.shutdown();
}
