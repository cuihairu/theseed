#pragma once

#include "theseed/runtime/RuntimeTypes.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>

namespace theseed::runtime {

class Entity;
class ControllerManager;

enum class ControllerType : std::uint8_t {
    MoveToPoint = 0,
    MoveToEntity = 1,
};

using ControllerId = std::uint32_t;

// Base class for entity controllers. Controllers are per-entity behavior
// modifiers that tick each frame and auto-remove on completion.

class Controller {
public:
    virtual ~Controller() = default;

    ControllerId id() const;
    ControllerType type() const;
    Entity& owner() const;
    std::int32_t userArg() const;
    bool active() const;

    virtual void start() = 0;
    virtual void stop() = 0;
    virtual void tick(float deltaTime) = 0;

protected:
    Controller(Entity& owner, ControllerType type, std::int32_t userArg);

    Entity* owner_;
    ControllerId id_ = 0;
    ControllerType type_;
    std::int32_t userArg_ = 0;
    bool active_ = false;

    friend class ControllerManager;
};

// Per-entity controller collection. Ticked by SpaceRuntime each frame.

class ControllerManager {
public:
    ControllerManager() = default;

    ControllerId add(std::unique_ptr<Controller> controller);
    void remove(ControllerId id);
    void clear();
    void tick(float deltaTime);

    Controller* find(ControllerId id) const;
    std::size_t count() const;

private:
    ControllerId nextId_ = 1;
    std::unordered_map<ControllerId, std::unique_ptr<Controller>> controllers_;
};

// Moves owner entity toward a fixed world position at constant speed.
// Auto-removes on arrival or cancellation.

class MoveToPointController final : public Controller {
public:
    static constexpr float kDefaultArrivalThreshold = 0.5F;

    MoveToPointController(Entity& owner, Vector3 target, float speed,
                          float arrivalThreshold = kDefaultArrivalThreshold,
                          std::int32_t userArg = 0);

    void start() override;
    void stop() override;
    void tick(float deltaTime) override;

    const Vector3& target() const;
    float speed() const;

private:
    Vector3 target_;
    float speed_;
    float arrivalThreshold_;
};

// Moves owner entity toward a target entity at constant speed.
// Updates direction each tick to track a moving target.

class MoveToEntityController final : public Controller {
public:
    MoveToEntityController(Entity& owner, EntityId targetEntityId, float speed,
                           float range = 1.0F, std::int32_t userArg = 0);

    void start() override;
    void stop() override;
    void tick(float deltaTime) override;

    EntityId targetEntityId() const;
    float speed() const;
    float range() const;

private:
    EntityId targetEntityId_;
    float speed_;
    float range_;
};

}  // namespace theseed::runtime
