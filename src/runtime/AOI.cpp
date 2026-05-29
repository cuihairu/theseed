#include "theseed/runtime/AOI.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace theseed::runtime {

// --- CoordinateNode ---

CoordinateNode::CoordinateNode(Entity& entity, Vector3 position)
    : entity_(&entity), position_(position) {}

Entity& CoordinateNode::entity() const { return *entity_; }

EntityId CoordinateNode::entityId() const { return entity_->id(); }

const Vector3& CoordinateNode::position() const { return position_; }

float CoordinateNode::x() const { return position_.x; }

float CoordinateNode::z() const { return position_.z; }

CoordinateNode* CoordinateNode::prevX() const { return prevX_; }
CoordinateNode* CoordinateNode::nextX() const { return nextX_; }
CoordinateNode* CoordinateNode::prevZ() const { return prevZ_; }
CoordinateNode* CoordinateNode::nextZ() const { return nextZ_; }

// --- RangeTrigger ---

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

float RangeTrigger::range() const { return range_; }

Entity& RangeTrigger::owner() const { return *owner_; }

bool RangeTrigger::installed() const { return coordinateSystem_ != nullptr; }

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

// --- CoordinateSystem ---

void CoordinateSystem::insertSortedX(CoordinateNode& node) {
    if (headX_ == nullptr) {
        headX_ = &node;
        return;
    }

    CoordinateNode* prev = nullptr;
    CoordinateNode* curr = headX_;
    while (curr != nullptr && curr->x() < node.x()) {
        prev = curr;
        curr = curr->nextX_;
    }

    node.prevX_ = prev;
    node.nextX_ = curr;
    if (prev != nullptr) {
        prev->nextX_ = &node;
    } else {
        headX_ = &node;
    }
    if (curr != nullptr) {
        curr->prevX_ = &node;
    }
}

void CoordinateSystem::insertSortedZ(CoordinateNode& node) {
    if (headZ_ == nullptr) {
        headZ_ = &node;
        return;
    }

    CoordinateNode* prev = nullptr;
    CoordinateNode* curr = headZ_;
    while (curr != nullptr && curr->z() < node.z()) {
        prev = curr;
        curr = curr->nextZ_;
    }

    node.prevZ_ = prev;
    node.nextZ_ = curr;
    if (prev != nullptr) {
        prev->nextZ_ = &node;
    } else {
        headZ_ = &node;
    }
    if (curr != nullptr) {
        curr->prevZ_ = &node;
    }
}

void CoordinateSystem::unlinkX(CoordinateNode& node) {
    if (node.prevX_ != nullptr) {
        node.prevX_->nextX_ = node.nextX_;
    } else {
        headX_ = node.nextX_;
    }
    if (node.nextX_ != nullptr) {
        node.nextX_->prevX_ = node.prevX_;
    }
    node.prevX_ = nullptr;
    node.nextX_ = nullptr;
}

void CoordinateSystem::unlinkZ(CoordinateNode& node) {
    if (node.prevZ_ != nullptr) {
        node.prevZ_->nextZ_ = node.nextZ_;
    } else {
        headZ_ = node.nextZ_;
    }
    if (node.nextZ_ != nullptr) {
        node.nextZ_->prevZ_ = node.prevZ_;
    }
    node.prevZ_ = nullptr;
    node.nextZ_ = nullptr;
}

void CoordinateSystem::reposition(CoordinateNode& node) {
    // Bubble toward head (lower x) while predecessor has larger x
    while (node.prevX_ != nullptr && node.prevX_->x() > node.x()) {
        auto* prev = node.prevX_;
        prev->nextX_ = node.nextX_;
        if (node.nextX_ != nullptr) {
            node.nextX_->prevX_ = prev;
        }
        node.prevX_ = prev->prevX_;
        prev->prevX_ = &node;
        node.nextX_ = prev;
        if (node.prevX_ != nullptr) {
            node.prevX_->nextX_ = &node;
        } else {
            headX_ = &node;
        }
    }
    // Bubble toward tail (higher x) while successor has smaller x
    while (node.nextX_ != nullptr && node.nextX_->x() < node.x()) {
        auto* next = node.nextX_;
        next->prevX_ = node.prevX_;
        if (node.prevX_ != nullptr) {
            node.prevX_->nextX_ = next;
        } else {
            headX_ = next;
        }
        node.nextX_ = next->nextX_;
        next->nextX_ = &node;
        node.prevX_ = next;
        if (node.nextX_ != nullptr) {
            node.nextX_->prevX_ = &node;
        }
    }

    // Same for Z axis
    while (node.prevZ_ != nullptr && node.prevZ_->z() > node.z()) {
        auto* prev = node.prevZ_;
        prev->nextZ_ = node.nextZ_;
        if (node.nextZ_ != nullptr) {
            node.nextZ_->prevZ_ = prev;
        }
        node.prevZ_ = prev->prevZ_;
        prev->prevZ_ = &node;
        node.nextZ_ = prev;
        if (node.prevZ_ != nullptr) {
            node.prevZ_->nextZ_ = &node;
        } else {
            headZ_ = &node;
        }
    }
    while (node.nextZ_ != nullptr && node.nextZ_->z() < node.z()) {
        auto* next = node.nextZ_;
        next->prevZ_ = node.prevZ_;
        if (node.prevZ_ != nullptr) {
            node.prevZ_->nextZ_ = next;
        } else {
            headZ_ = next;
        }
        node.nextZ_ = next->nextZ_;
        next->nextZ_ = &node;
        node.prevZ_ = next;
        if (node.nextZ_ != nullptr) {
            node.nextZ_->prevZ_ = &node;
        }
    }
}

void CoordinateSystem::insert(CoordinateNode& node) {
    nodes_[node.entityId()] = &node;
    insertSortedX(node);
    insertSortedZ(node);
}

void CoordinateSystem::remove(EntityId entityId) {
    auto it = nodes_.find(entityId);
    if (it == nodes_.end()) {
        return;
    }

    auto* node = it->second;
    unlinkX(*node);
    unlinkZ(*node);
    nodes_.erase(it);
}

void CoordinateSystem::update(EntityId entityId, const Vector3& position) {
    auto* node = find(entityId);
    if (node == nullptr) {
        throw std::out_of_range("coordinate node not found");
    }

    node->position_ = position;
    reposition(*node);
}

CoordinateNode* CoordinateSystem::find(EntityId entityId) const {
    auto it = nodes_.find(entityId);
    if (it == nodes_.end()) {
        return nullptr;
    }
    return it->second;
}

std::vector<Entity*> CoordinateSystem::entitiesInRange(const Vector3& origin,
                                                        float radius) const {
    std::vector<Entity*> result;

    const float minX = origin.x - radius;
    const float maxX = origin.x + radius;
    const float minZ = origin.z - radius;
    const float maxZ = origin.z + radius;
    const float radiusSq = radius * radius;

    // Walk X-sorted list from minX to maxX, filter by Z and actual distance
    auto* curr = headX_;
    while (curr != nullptr && curr->x() < minX) {
        curr = curr->nextX_;
    }

    while (curr != nullptr && curr->x() <= maxX) {
        float z = curr->z();
        if (z >= minZ && z <= maxZ) {
            auto dy = curr->position().y - origin.y;
            auto dx = curr->x() - origin.x;
            auto dz = z - origin.z;
            if (dx * dx + dy * dy + dz * dz <= radiusSq) {
                result.push_back(&curr->entity());
            }
        }
        curr = curr->nextX_;
    }

    return result;
}

CoordinateNode* CoordinateSystem::headX() const { return headX_; }
CoordinateNode* CoordinateSystem::headZ() const { return headZ_; }

}  // namespace theseed::runtime
