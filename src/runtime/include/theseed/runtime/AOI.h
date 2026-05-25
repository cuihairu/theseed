#pragma once

#include "theseed/runtime/Entity.h"
#include "theseed/runtime/RuntimeTypes.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace theseed::runtime {

class CoordinateSystem;

class CoordinateNode final {
public:
    CoordinateNode(Entity& entity, Vector3 position = {});

    Entity& entity() const;
    EntityId entityId() const;
    const Vector3& position() const;
    void setPosition(const Vector3& position);

private:
    Entity* entity_ = nullptr;
    Vector3 position_{};
};

class RangeTrigger {
public:
    RangeTrigger(Entity& owner, float range);
    virtual ~RangeTrigger() = default;

    void install(CoordinateSystem& coordinateSystem);
    void uninstall();
    void updateRange(float newRange);
    float range() const;

    Entity& owner() const;
    bool installed() const;
    void refresh();

protected:
    virtual void onEnter(CoordinateNode& node, float distance) = 0;
    virtual void onLeave(CoordinateNode& node) = 0;

private:
    Entity* owner_ = nullptr;
    float range_ = 0.0F;
    CoordinateSystem* coordinateSystem_ = nullptr;
    std::unordered_set<EntityId> inside_;
};

class CoordinateSystem final {
public:
    void insert(CoordinateNode& node);
    void remove(EntityId entityId);
    void update(EntityId entityId, const Vector3& position);

    CoordinateNode* find(EntityId entityId) const;
    std::vector<Entity*> entitiesInRange(const Vector3& origin, float radius) const;

private:
    std::unordered_map<EntityId, CoordinateNode*> nodes_;
};

}  // namespace theseed::runtime
