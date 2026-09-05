#ifndef GAMEOBJECT_H
#define GAMEOBJECT_H

/* Fixed-size pool of game objects.
 *
 * Players join and leave at runtime, so the obvious approach is to push_back /
 * erase a std::vector of players. That reallocates, invalidates pointers, and
 * shuffles indices around while other code is still holding them. Instead the
 * pool allocates every object once, up front, and hands out slots:
 *
 *   GameObjectPool pool(64);
 *   GameObjectHandle h = pool.acquire();
 *   if (GameObject *obj = pool.get(h)) { obj->x = 100.0f; }
 *   ...
 *   pool.release(h);
 *
 * Handles carry a generation counter, so a handle to a slot that has since
 * been released and reused resolves to nullptr rather than silently reading or
 * writing some other player's object. That matters here because a DDS sample
 * for a player who has already disconnected can still arrive after the fact.
 */

#include <cstddef>
#include <cstdint>
#include <vector>

/* No owner: the object is not tied to a networked player (scenery, a local
 * effect, the not-yet-assigned local player). */
static const int32_t GAMEOBJECT_NO_OWNER = -1;

struct GameObject
{
    /* Position of the top-left corner, in pixels, matching the renderer's
     * coordinate space (origin top-left, +y down). */
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;

    /* Pixels per second. Left at zero for objects moved directly. */
    float vx = 0.0f;
    float vy = 0.0f;

    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;

    /* The DDS player id this object represents, or GAMEOBJECT_NO_OWNER. */
    int32_t owner_id = GAMEOBJECT_NO_OWNER;

    /* Owned by the pool -- do not set these directly. */
    bool     active = false;
    uint32_t generation = 0;
};

struct GameObjectHandle
{
    int32_t  index = -1;
    uint32_t generation = 0;

    bool valid() const { return index >= 0; }
};

class GameObjectPool
{
public:
    /* Allocates every slot immediately; the pool never grows past this. */
    explicit GameObjectPool(size_t capacity);

    /* Takes a free slot and resets it to a default-constructed GameObject.
     * Returns an invalid handle when the pool is full -- check valid(). */
    GameObjectHandle acquire();

    /* Returns the slot to the free list. Bumps the slot's generation, which is
     * what makes every outstanding handle to it stale. Releasing an already
     * released or invalid handle does nothing. */
    void release(GameObjectHandle handle);

    /* nullptr if the handle is invalid, out of range, or stale. Always check
     * the result; do not cache the pointer across a release. */
    GameObject *get(GameObjectHandle handle);
    const GameObject *get(GameObjectHandle handle) const;

    /* Handle of the first active object with this owner id, invalid if there
     * is none. Linear scan -- fine at these sizes (tens of objects). */
    GameObjectHandle find_by_owner(int32_t owner_id) const;

    /* Handle for slot `index`, invalid if that slot is free. Walking slots by
     * index with this is the way to release objects while iterating: unlike
     * for_each_active it hands back a handle, and release() only touches the
     * free list and that one slot, so the walk stays valid. */
    GameObjectHandle handle_at(size_t index) const;

    /* Releases every object without freeing memory. */
    void clear();

    size_t active_count() const { return m_active_count; }
    size_t capacity() const { return m_objects.size(); }

    /* Calls fn(GameObject&) for each active object. Do not acquire or release
     * from inside fn -- finish iterating first. */
    template <typename Fn>
    void for_each_active(Fn fn)
    {
        for (GameObject &object : m_objects)
        {
            if (object.active)
            {
                fn(object);
            }
        }
    }

    template <typename Fn>
    void for_each_active(Fn fn) const
    {
        for (const GameObject &object : m_objects)
        {
            if (object.active)
            {
                fn(object);
            }
        }
    }

private:
    std::vector<GameObject> m_objects;

    /* Indices of free slots, used as a stack: acquire pops, release pushes. */
    std::vector<int32_t> m_free_slots;

    size_t m_active_count = 0;
};

#endif /* GAMEOBJECT_H */
