#ifndef CLIENT_H
#define CLIENT_H

#include "gameobject.h"
#include "renderer.h"

class Client
{
public:
    Client();

    bool Init();

    /* Runs the render/input loop until the window is closed. */
    void Run();

    void Shutdown();

private:
    void HandleInput(float dt);
    void Update(float dt);
    void Draw();

    Renderer m_renderer;

    /* Every drawable lives in this pool. Networking (DDS) is not wired into
     * the C++ port yet -- see docs/NETWORKING.md -- so for now the local
     * player is the only owned object; remote players will acquire their own
     * slots as they join, keyed by owner_id. */
    GameObjectPool m_objects;
    GameObjectHandle m_player;

    /* Rate limit on spawning, so holding space does not drain the pool in a
     * single frame. Seconds until the next spawn is allowed. */
    float m_spawn_cooldown = 0.0f;
};

#endif /* CLIENT_H */
