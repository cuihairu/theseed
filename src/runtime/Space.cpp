#include "theseed/runtime/Space.h"

#include "theseed/runtime/AOI.h"

#include <stdexcept>
#include <utility>

namespace theseed::runtime {

SingleCellTopology::SingleCellTopology(CellId cellId) : cellId_(cellId) {}

CellId SingleCellTopology::locateCell(const Vector3& position) const {
    static_cast<void>(position);
    return cellId_;
}

std::vector<CellId> SingleCellTopology::getAdjacentCells(const Vector3& position,
                                                         float radius) const {
    static_cast<void>(position);
    static_cast<void>(radius);
    return {cellId_};
}

void SingleCellTopology::onTopologyChanged(std::function<void()> callback) {
    callback_ = std::move(callback);
}

void SingleCellTopology::reportLoad(CellId cell, float load) {
    if (cell != cellId_) {
        throw std::invalid_argument("single cell topology received unexpected cell id");
    }

    lastReportedLoad_ = load;
}

void SingleCellTopology::rebalance() {
    if (callback_) {
        callback_();
    }
}

CellId SingleCellTopology::cellId() const {
    return cellId_;
}

float SingleCellTopology::lastReportedLoad() const {
    return lastReportedLoad_;
}

Space::Space(SpaceId id, std::string name, std::unique_ptr<ISpaceTopology> topology)
    : id_(id),
      name_(std::move(name)),
      topology_(std::move(topology)),
      coordinateSystem_(std::make_unique<CoordinateSystem>()) {
    if (!topology_) {
        throw std::invalid_argument("space topology must not be null");
    }
}

Space::~Space() = default;

void Space::initialize(const SpaceConfig& config) {
    if (!config.name.empty()) {
        name_ = config.name;
    }

    state_ = SpaceState::Running;
}

void Space::beginDrain() {
    state_ = SpaceState::Draining;
}

void Space::shutdown() {
    entities_.clear();
    state_ = SpaceState::Shutdown;
}

SpaceId Space::id() const {
    return id_;
}

const std::string& Space::name() const {
    return name_;
}

SpaceState Space::state() const {
    return state_;
}

void Space::addEntity(Entity& entity, const Vector3& position) {
    if (entities_.contains(entity.id())) {
        throw std::invalid_argument("entity already exists in space");
    }

    entity.setPosition(position);
    auto node = std::make_unique<CoordinateNode>(entity, position);
    coordinateSystem_->insert(*node);
    entities_.emplace(entity.id(), Member{
                                      .entity = &entity,
                                      .node = std::move(node),
                                  });
}

void Space::removeEntity(EntityId entityId) {
    coordinateSystem_->remove(entityId);
    entities_.erase(entityId);
}

Entity* Space::findEntity(EntityId entityId) const {
    const auto iter = entities_.find(entityId);
    if (iter == entities_.end()) {
        return nullptr;
    }

    return iter->second.entity;
}

void Space::updateEntityPosition(EntityId entityId, const Vector3& position) {
    coordinateSystem_->update(entityId, position);
}

std::optional<Vector3> Space::entityPosition(EntityId entityId) const {
    const auto* node = coordinateSystem_->find(entityId);
    if (node == nullptr) {
        return std::nullopt;
    }

    return node->position();
}

std::vector<Entity*> Space::entities() const {
    std::vector<Entity*> result;
    result.reserve(entities_.size());
    for (const auto& [entityId, member] : entities_) {
        static_cast<void>(entityId);
        result.push_back(member.entity);
    }

    return result;
}

std::vector<Entity*> Space::queryRange(const Vector3& center, float radius) const {
    return coordinateSystem_->entitiesInRange(center, radius);
}

std::vector<Entity*> Space::findEntitiesByType(const std::string& entityType) const {
    std::vector<Entity*> result;
    for (const auto& [entityId, member] : entities_) {
        static_cast<void>(entityId);
        if (member.entity != nullptr && member.entity->entityType() == entityType) {
            result.push_back(member.entity);
        }
    }
    return result;
}

std::vector<Entity*> Space::findEntitiesByTag(const std::string& tag) const {
    std::vector<Entity*> result;
    for (const auto& [entityId, member] : entities_) {
        static_cast<void>(entityId);
        if (member.entity != nullptr && member.entity->hasTag(tag)) {
            result.push_back(member.entity);
        }
    }
    return result;
}

std::size_t Space::entityCount() const {
    return entities_.size();
}

CoordinateSystem& Space::coordinateSystem() {
    return *coordinateSystem_;
}

const CoordinateSystem& Space::coordinateSystem() const {
    return *coordinateSystem_;
}

ISpaceTopology& Space::topology() {
    return *topology_;
}

const ISpaceTopology& Space::topology() const {
    return *topology_;
}

}  // namespace theseed::runtime
