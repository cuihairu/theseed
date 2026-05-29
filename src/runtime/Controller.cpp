#include "theseed/runtime/Controller.h"
#include "theseed/runtime/Entity.h"

#include <cmath>
#include <stdexcept>

namespace theseed::runtime {

// --- Controller ---

Controller::Controller(Entity& owner, ControllerType type, std::int32_t userArg)
    : owner_(&owner), type_(type), userArg_(userArg) {}

ControllerId Controller::id() const { return id_; }
ControllerType Controller::type() const { return type_; }
Entity& Controller::owner() const { return *owner_; }
std::int32_t Controller::userArg() const { return userArg_; }
bool Controller::active() const { return active_; }

// --- ControllerManager ---

ControllerId ControllerManager::add(std::unique_ptr<Controller> controller) {
    auto id = nextId_++;
    controller->id_ = id;
    controller->active_ = true;
    controller->start();
    if (controller->active_) {
        controllers_.emplace(id, std::move(controller));
    }
    return id;
}

void ControllerManager::remove(ControllerId id) {
    auto it = controllers_.find(id);
    if (it == controllers_.end()) return;

    auto& ctrl = *it->second;
    if (ctrl.active_) {
        ctrl.active_ = false;
        ctrl.stop();
    }
    controllers_.erase(it);
}

void ControllerManager::clear() {
    for (auto& [id, ctrl] : controllers_) {
        static_cast<void>(id);
        if (ctrl->active_) {
            ctrl->active_ = false;
            ctrl->stop();
        }
    }
    controllers_.clear();
}

void ControllerManager::tick(float deltaTime) {
    // Collect IDs of completed controllers to avoid invalidating iterators
    std::vector<ControllerId> completed;

    for (auto& [id, ctrl] : controllers_) {
        if (!ctrl->active_) continue;
        ctrl->tick(deltaTime);
        if (!ctrl->active_) {
            completed.push_back(id);
        }
    }

    for (auto id : completed) {
        controllers_.erase(id);
    }
}

Controller* ControllerManager::find(ControllerId id) const {
    auto it = controllers_.find(id);
    if (it == controllers_.end()) return nullptr;
    return it->second.get();
}

std::size_t ControllerManager::count() const { return controllers_.size(); }

// --- MoveToPointController ---

MoveToPointController::MoveToPointController(Entity& owner, Vector3 target,
                                             float speed, float arrivalThreshold,
                                             std::int32_t userArg)
    : Controller(owner, ControllerType::MoveToPoint, userArg),
      target_(target),
      speed_(speed),
      arrivalThreshold_(arrivalThreshold) {}

void MoveToPointController::start() {
    auto pos = owner_->position();
    auto dx = target_.x - pos.x;
    auto dz = target_.z - pos.z;
    auto dist = std::sqrt(dx * dx + dz * dz);

    if (dist <= arrivalThreshold_) {
        active_ = false;
        owner_->notifyControllerComplete(id_, userArg_, true);
        return;
    }

    Vector3 dir{dx / dist, 0.0F, dz / dist};
    owner_->setVelocity(Vector3{dir.x * speed_, 0.0F, dir.z * speed_});
}

void MoveToPointController::stop() {
    owner_->clearVelocity();
}

void MoveToPointController::tick(float deltaTime) {
    static_cast<void>(deltaTime);

    auto pos = owner_->position();
    auto dx = target_.x - pos.x;
    auto dz = target_.z - pos.z;
    auto dist = std::sqrt(dx * dx + dz * dz);

    if (dist <= arrivalThreshold_) {
        active_ = false;
        owner_->clearVelocity();
        owner_->notifyControllerComplete(id_, userArg_, true);
        return;
    }

    // Update direction (entity may have been displaced externally)
    Vector3 dir{dx / dist, 0.0F, dz / dist};
    owner_->setVelocity(Vector3{dir.x * speed_, 0.0F, dir.z * speed_});
}

const Vector3& MoveToPointController::target() const { return target_; }
float MoveToPointController::speed() const { return speed_; }

// --- MoveToEntityController ---

MoveToEntityController::MoveToEntityController(Entity& owner, EntityId targetEntityId,
                                               float speed, float range,
                                               std::int32_t userArg)
    : Controller(owner, ControllerType::MoveToEntity, userArg),
      targetEntityId_(targetEntityId),
      speed_(speed),
      range_(range) {}

void MoveToEntityController::start() {
    auto targetPos = owner_->queryEntityPosition(targetEntityId_);
    if (!targetPos.has_value()) {
        active_ = false;
        owner_->notifyControllerComplete(id_, userArg_, false);
        return;
    }

    auto pos = owner_->position();
    auto dx = targetPos->x - pos.x;
    auto dz = targetPos->z - pos.z;
    auto dist = std::sqrt(dx * dx + dz * dz);

    if (dist <= range_) {
        active_ = false;
        owner_->notifyControllerComplete(id_, userArg_, true);
        return;
    }

    Vector3 dir{dx / dist, 0.0F, dz / dist};
    owner_->setVelocity(Vector3{dir.x * speed_, 0.0F, dir.z * speed_});
}

void MoveToEntityController::stop() {
    owner_->clearVelocity();
}

void MoveToEntityController::tick(float deltaTime) {
    static_cast<void>(deltaTime);

    auto targetPos = owner_->queryEntityPosition(targetEntityId_);
    if (!targetPos.has_value()) {
        // Target entity no longer exists
        active_ = false;
        owner_->clearVelocity();
        owner_->notifyControllerComplete(id_, userArg_, false);
        return;
    }

    auto pos = owner_->position();
    auto dx = targetPos->x - pos.x;
    auto dz = targetPos->z - pos.z;
    auto dist = std::sqrt(dx * dx + dz * dz);

    if (dist <= range_) {
        active_ = false;
        owner_->clearVelocity();
        owner_->notifyControllerComplete(id_, userArg_, true);
        return;
    }

    Vector3 dir{dx / dist, 0.0F, dz / dist};
    owner_->setVelocity(Vector3{dir.x * speed_, 0.0F, dir.z * speed_});
}

EntityId MoveToEntityController::targetEntityId() const { return targetEntityId_; }
float MoveToEntityController::speed() const { return speed_; }
float MoveToEntityController::range() const { return range_; }

}  // namespace theseed::runtime
