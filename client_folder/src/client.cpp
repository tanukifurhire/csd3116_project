#include "client.h"

#include <iostream>

/* Same window and square dimensions as the C client, so the two look alike. */
static const int   WINDOW_WIDTH  = 800;
static const int   WINDOW_HEIGHT = 600;
static const float SQ_WIDTH      = 50.0f;
static const float MOVE_SPEED    = 300.0f; /* pixels per second */

/* Five clients are supported (see gen_certs.sh), each with one square, plus
 * headroom for the projectiles/effects the pool is really there for. */
static const size_t MAX_OBJECTS = 64;

static const float SPAWN_INTERVAL = 0.1f;

Client::Client()
    : m_objects(MAX_OBJECTS)
{
}

bool Client::Init()
{
    if (!m_renderer.init(WINDOW_WIDTH, WINDOW_HEIGHT, "DDS Client (C++)"))
    {
        std::cerr << "Renderer failed to initialise" << std::endl;
        return false;
    }

    m_player = m_objects.acquire();
    GameObject *player = m_objects.get(m_player);
    if (player == nullptr)
    {
        std::cerr << "Object pool is empty" << std::endl;
        return false;
    }

    player->x = WINDOW_WIDTH / 2.0f - SQ_WIDTH / 2.0f;
    player->y = WINDOW_HEIGHT / 2.0f - SQ_WIDTH / 2.0f;
    player->w = SQ_WIDTH;
    player->h = SQ_WIDTH;
    player->r = 1.0f;
    player->g = 0.0f;
    player->b = 0.0f;

    return true;
}

void Client::HandleInput(float dt)
{
    GLFWwindow *window = m_renderer.window();

    if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
    {
        glfwSetWindowShouldClose(window, GLFW_TRUE);
    }

    GameObject *player = m_objects.get(m_player);
    if (player == nullptr)
    {
        return;
    }

    if (glfwGetKey(window, GLFW_KEY_UP) == GLFW_PRESS)
    {
        player->y -= MOVE_SPEED * dt;
    }
    if (glfwGetKey(window, GLFW_KEY_DOWN) == GLFW_PRESS)
    {
        player->y += MOVE_SPEED * dt;
    }
    if (glfwGetKey(window, GLFW_KEY_LEFT) == GLFW_PRESS)
    {
        player->x -= MOVE_SPEED * dt;
    }
    if (glfwGetKey(window, GLFW_KEY_RIGHT) == GLFW_PRESS)
    {
        player->x += MOVE_SPEED * dt;
    }

    /* Space fires a projectile: acquire a slot, and give it back once it
     * leaves the window (see Update). Holding it down keeps the pool churning,
     * which is the behaviour the pool exists for. */
    m_spawn_cooldown -= dt;
    if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS && m_spawn_cooldown <= 0.0f)
    {
        GameObjectHandle handle = m_objects.acquire();
        if (GameObject *bullet = m_objects.get(handle))
        {
            bullet->w = 8.0f;
            bullet->h = 8.0f;
            bullet->x = player->x + SQ_WIDTH / 2.0f - bullet->w / 2.0f;
            bullet->y = player->y;
            bullet->vy = -600.0f;
            bullet->r = 0.1f;
            bullet->g = 0.3f;
            bullet->b = 0.9f;
        }
        /* An invalid handle just means the pool is full this frame -- the shot
         * is dropped rather than growing the pool. */
        m_spawn_cooldown = SPAWN_INTERVAL;
    }
}

void Client::Update(float dt)
{
    int window_width = 0;
    int window_height = 0;
    glfwGetWindowSize(m_renderer.window(), &window_width, &window_height);

    /* Walking slots by index (rather than for_each_active) so each object
     * comes with a handle, which is what release() needs. */
    for (size_t i = 0; i < m_objects.capacity(); ++i)
    {
        GameObjectHandle handle = m_objects.handle_at(i);
        GameObject *object = m_objects.get(handle);
        if (object == nullptr)
        {
            continue; /* free slot */
        }

        object->x += object->vx * dt;
        object->y += object->vy * dt;

        if (handle.index == m_player.index)
        {
            continue; /* the local player is never recycled */
        }

        const bool off_screen = object->x + object->w < 0.0f ||
                                object->y + object->h < 0.0f ||
                                object->x > static_cast<float>(window_width) ||
                                object->y > static_cast<float>(window_height);
        if (off_screen)
        {
            m_objects.release(handle);
        }
    }
}

void Client::Draw()
{
    m_renderer.begin_frame(1.0f, 1.0f, 1.0f);

    m_objects.for_each_active([&](const GameObject &object) {
        m_renderer.draw_quad(object.x, object.y, object.w, object.h,
                             object.r, object.g, object.b, object.a);
    });

    m_renderer.end_frame();
}

void Client::Run()
{
    double last_time = glfwGetTime();

    while (!m_renderer.should_close())
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
}

void Client::Shutdown()
{
    m_objects.clear();
    m_renderer.shutdown();
}
