#include "../include/client.h"

#include <iostream>

/* Same window and square dimensions as the C client, so the two look alike. */
static const int   WINDOW_WIDTH  = 800;
static const int   WINDOW_HEIGHT = 600;
static const float SQ_WIDTH      = 50.0f;
static const float MOVE_SPEED    = 300.0f; /* pixels per second */

bool Client::Init()
{
    if (!m_renderer.init(WINDOW_WIDTH, WINDOW_HEIGHT, "DDS Client (C++)"))
    {
        std::cerr << "Renderer failed to initialise" << std::endl;
        return false;
    }
    return true;
}

void Client::Run()
{
    GLFWwindow *window = m_renderer.window();
    double last_time = glfwGetTime();

    while (!m_renderer.should_close())
    {
        /* Frame-rate independent movement: scale by how long the last frame
         * took instead of moving a fixed number of pixels per iteration. */
        const double now = glfwGetTime();
        const float dt = static_cast<float>(now - last_time);
        last_time = now;

        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
        {
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }
        if (glfwGetKey(window, GLFW_KEY_UP) == GLFW_PRESS)
        {
            m_y -= MOVE_SPEED * dt;
        }
        if (glfwGetKey(window, GLFW_KEY_DOWN) == GLFW_PRESS)
        {
            m_y += MOVE_SPEED * dt;
        }
        if (glfwGetKey(window, GLFW_KEY_LEFT) == GLFW_PRESS)
        {
            m_x -= MOVE_SPEED * dt;
        }
        if (glfwGetKey(window, GLFW_KEY_RIGHT) == GLFW_PRESS)
        {
            m_x += MOVE_SPEED * dt;
        }

        m_renderer.begin_frame(1.0f, 1.0f, 1.0f);
        m_renderer.draw_quad_outline(m_x, m_y, SQ_WIDTH, SQ_WIDTH, 2.0f,
                                     1.0f, 0.0f, 0.0f);
        m_renderer.end_frame();
    }
}

void Client::Shutdown()
{
    m_renderer.shutdown();
}
