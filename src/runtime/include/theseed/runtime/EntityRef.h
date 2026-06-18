#pragma once

#include "theseed/runtime/RuntimeTypes.h"

#include <memory>

namespace theseed::runtime {

class Entity;

// Safe weak reference to an Entity. Automatically invalidates when the entity is destroyed.
class EntityRef final {
public:
    EntityRef() = default;
    explicit EntityRef(Entity& entity);
    EntityRef(const EntityRef& other) = default;
    EntityRef(EntityRef&& other) noexcept = default;
    EntityRef& operator=(const EntityRef& other) = default;
    EntityRef& operator=(EntityRef&& other) noexcept = default;
    ~EntityRef() = default;

    Entity* get() const;
    Entity& operator*() const;
    Entity* operator->() const;

    bool isValid() const;
    explicit operator bool() const;
    EntityId entityId() const;

    void reset();

    static EntityRef fromEntity(Entity& entity);
    static EntityRef invalid();

private:
    EntityId id_ = 0;
    Entity* cached_ = nullptr;
    std::shared_ptr<bool> alive_;
};

}  // namespace theseed::runtime
