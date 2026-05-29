#pragma once

#include "theseed/runtime/Entity.h"
#include "theseed/runtime/RuntimeTypes.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace theseed::runtime {

class CoordinateSystem;

// Sorted cross-linked list node. Each node participates in two independent
// doubly-linked lists (X-axis and Z-axis), enabling O(K) range queries.

class CoordinateNode {
public:
    CoordinateNode(Entity& entity, Vector3 position = {});
    virtual ~CoordinateNode() = default;

    Entity& entity() const;
    EntityId entityId() const;
    const Vector3& position() const;

    float x() const;
    float z() const;

    CoordinateNode* prevX() const;
    CoordinateNode* nextX() const;
    CoordinateNode* prevZ() const;
    CoordinateNode* nextZ() const;

protected:
    friend class CoordinateSystem;

    Entity* entity_ = nullptr;
    Vector3 position_{};

    CoordinateNode* prevX_ = nullptr;
    CoordinateNode* nextX_ = nullptr;
    CoordinateNode* prevZ_ = nullptr;
    CoordinateNode* nextZ_ = nullptr;
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
    CoordinateSystem() = default;
    ~CoordinateSystem() = default;

    CoordinateSystem(const CoordinateSystem&) = delete;
    CoordinateSystem& operator=(const CoordinateSystem&) = delete;

    void insert(CoordinateNode& node);
    void remove(EntityId entityId);
    void update(EntityId entityId, const Vector3& position);

    CoordinateNode* find(EntityId entityId) const;
    std::vector<Entity*> entitiesInRange(const Vector3& origin, float radius) const;

    CoordinateNode* headX() const;
    CoordinateNode* headZ() const;

private:
    void insertSortedX(CoordinateNode& node);
    void insertSortedZ(CoordinateNode& node);
    void unlinkX(CoordinateNode& node);
    void unlinkZ(CoordinateNode& node);
    void reposition(CoordinateNode& node);

    CoordinateNode* headX_ = nullptr;
    CoordinateNode* headZ_ = nullptr;
    std::unordered_map<EntityId, CoordinateNode*> nodes_;
};

}  // namespace theseed::runtime
