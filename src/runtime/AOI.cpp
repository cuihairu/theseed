#include "theseed/runtime/AOI.h"

#include <stdexcept>

namespace theseed::runtime {

CoordinateNode::CoordinateNode(Entity& entity, Vector3 position)
    : entity_(&entity), position_(position) {}

Entity& CoordinateNode::entity() const {
    return *entity_;
}

EntityId CoordinateNode::entityId() const {
    return entity_->id();
}

const Vector3& CoordinateNode::position() const {
    return position_;
}

void CoordinateNode::setPosition(const Vector3& position) {
    position_ = position;
}

RangeTrigger::RangeTrigger(Entity& owner, float range) : owner_(&owner), range_(range) {}

void RangeTrigger::install(CoordinateSystem& coordinateSystem) {
    coordinateSystem_ = &coordinateSystem;
    inside_.clear();
    refresh();
}

void RangeTrigger::uninstall() {
    coordinateSystem_ = nullptr;
    inside_.clear();
}

void RangeTrigger::updateRange(float newRange) {
    range_ = newRange;
    if (coordinateSystem_ != nullptr) {
        refresh();
    }
}

float RangeTrigger::range() const {
    return range_;
}

Entity& RangeTrigger::owner() const {
    return *owner_;
}

bool RangeTrigger::installed() const {
    return coordinateSystem_ != nullptr;
}

void RangeTrigger::refresh() {
    if (coordinateSystem_ == nullptr) {
        return;
    }

    const auto* ownerNode = coordinateSystem_->find(owner_->id());
    if (ownerNode == nullptr) {
        throw std::logic_error("trigger owner is not in coordinate system");
    }

    std::unordered_set<EntityId> nextInside;
    const auto entities = coordinateSystem_->entitiesInRange(ownerNode->position(), range_);
    for (Entity* entity : entities) {
        if (entity == nullptr || entity->id() == owner_->id()) {
            continue;
        }

        nextInside.insert(entity->id());
        if (!inside_.contains(entity->id())) {
            auto* node = coordinateSystem_->find(entity->id());
            if (node != nullptr) {
                onEnter(*node, std::sqrt(distanceSquared(ownerNode->position(), node->position())));
            }
        }
    }

    for (const auto entityId : inside_) {
        if (nextInside.contains(entityId)) {
            continue;
        }

        auto* node = coordinateSystem_->find(entityId);
        if (node != nullptr) {
            onLeave(*node);
        }
    }

    inside_ = std::move(nextInside);
}

void CoordinateSystem::insert(CoordinateNode& node) {
    nodes_[node.entityId()] = &node;
}

void CoordinateSystem::remove(EntityId entityId) {
    nodes_.erase(entityId);
}

void CoordinateSystem::update(EntityId entityId, const Vector3& position) {
    auto* node = find(entityId);
    if (node == nullptr) {
        throw std::out_of_range("coordinate node not found");
    }

    node->setPosition(position);
}

CoordinateNode* CoordinateSystem::find(EntityId entityId) const {
    const auto iter = nodes_.find(entityId);
    if (iter == nodes_.end()) {
        return nullptr;
    }

    return iter->second;
}

std::vector<Entity*> CoordinateSystem::entitiesInRange(const Vector3& origin, float radius) const {
    const auto radiusSq = radius * radius;
    std::vector<Entity*> result;
    result.reserve(nodes_.size());
    for (const auto& [entityId, node] : nodes_) {
        static_cast<void>(entityId);
        if (distanceSquared(origin, node->position()) <= radiusSq) {
            result.push_back(&node->entity());
        }
    }

    return result;
}

}  // namespace theseed::runtime
