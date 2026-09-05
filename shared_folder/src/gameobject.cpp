#include "gameobject.h"

GameObjectPool::GameObjectPool(size_t capacity)
    : m_objects(capacity)
{
    /* Filled back to front so that acquire() hands out slot 0 first, which
     * keeps the pool readable while debugging. */
    m_free_slots.reserve(capacity);
    for (size_t i = capacity; i > 0; --i)
    {
        m_free_slots.push_back(static_cast<int32_t>(i - 1));
    }
}

GameObjectHandle GameObjectPool::acquire()
{
    GameObjectHandle handle;

    if (m_free_slots.empty())
    {
        return handle; /* pool exhausted; handle.valid() is false */
    }

    const int32_t index = m_free_slots.back();
    m_free_slots.pop_back();

    GameObject &object = m_objects[static_cast<size_t>(index)];

    /* Reset the slot, but keep the generation the previous user left behind --
     * that counter is what invalidates their handles. */
    const uint32_t generation = object.generation;
    object = GameObject();
    object.generation = generation;
    object.active = true;

    ++m_active_count;

    handle.index = index;
    handle.generation = generation;
    return handle;
}

void GameObjectPool::release(GameObjectHandle handle)
{
    GameObject *object = get(handle);
    if (object == nullptr)
    {
        return; /* invalid, out of range, or already released */
    }

    object->active = false;

    /* Any handle still pointing at this slot no longer matches. Wrapping after
     * 2^32 reuses is theoretically possible and harmless in practice. */
    ++object->generation;

    m_free_slots.push_back(handle.index);
    --m_active_count;
}

GameObject *GameObjectPool::get(GameObjectHandle handle)
{
    /* const_cast on the const overload keeps the two lookups identical. */
    return const_cast<GameObject *>(
        static_cast<const GameObjectPool *>(this)->get(handle));
}

const GameObject *GameObjectPool::get(GameObjectHandle handle) const
{
    if (!handle.valid() ||
        static_cast<size_t>(handle.index) >= m_objects.size())
    {
        return nullptr;
    }

    const GameObject &object = m_objects[static_cast<size_t>(handle.index)];
    if (!object.active || object.generation != handle.generation)
    {
        return nullptr; /* slot was released, and possibly reused since */
    }

    return &object;
}

GameObjectHandle GameObjectPool::find_by_owner(int32_t owner_id) const
{
    GameObjectHandle handle;

    if (owner_id == GAMEOBJECT_NO_OWNER)
    {
        return handle; /* not an identity; every unowned object would match */
    }

    for (size_t i = 0; i < m_objects.size(); ++i)
    {
        const GameObject &object = m_objects[i];
        if (object.active && object.owner_id == owner_id)
        {
            handle.index = static_cast<int32_t>(i);
            handle.generation = object.generation;
            return handle;
        }
    }

    return handle;
}

GameObjectHandle GameObjectPool::handle_at(size_t index) const
{
    GameObjectHandle handle;

    if (index >= m_objects.size() || !m_objects[index].active)
    {
        return handle;
    }

    handle.index = static_cast<int32_t>(index);
    handle.generation = m_objects[index].generation;
    return handle;
}

void GameObjectPool::clear()
{
    m_free_slots.clear();
    m_free_slots.reserve(m_objects.size());

    for (size_t i = m_objects.size(); i > 0; --i)
    {
        GameObject &object = m_objects[i - 1];
        if (object.active)
        {
            object.active = false;
            ++object.generation;
        }
        m_free_slots.push_back(static_cast<int32_t>(i - 1));
    }

    m_active_count = 0;
}
