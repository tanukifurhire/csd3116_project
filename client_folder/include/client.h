#ifndef CLIENT_H
#define CLIENT_H

#include "renderer.h"

class Client
{
public:
    bool Init();

    /* Runs the render/input loop until the window is closed. */
    void Run();

    void Shutdown();

private:
    Renderer m_renderer;

    /* Local player position, in pixels from the top-left of the window.
     * Networking (DDS) is not wired into the C++ port yet -- see
     * docs/NETWORKING.md -- so for now only this square exists. */
    float m_x = 320.0f;
    float m_y = 240.0f;
};

#endif /* CLIENT_H */
