#include "theseed/runtime/Entity.h"

#include <string_view>

namespace theseed::runtime {

namespace {

bool supportsMethodSide(EntitySide entitySide, MethodSide methodSide) {
    switch (entitySide) {
    case EntitySide::Base:
        return methodSide == MethodSide::Base;
    case EntitySide::Cell:
        return methodSide == MethodSide::Cell;
    }

    return false;
}

}  // namespace

Entity::Entity(EntityId id, EntitySide side, const EntityDef& def)
    : id_(id), side_(side), def_(&def) {
    properties_.init(def);
}

EntityId Entity::id() const {
    return id_;
}

EntitySide Entity::side() const {
    return side_;
}

EntityState Entity::state() const {
    return state_.load(std::memory_order_acquire);
}

const std::string& Entity::entityType() const {
    return def_->entityType();
}

const EntityCall* Entity::baseEntityCall() const {
    return baseCall_ ? &*baseCall_ : nullptr;
}

EntityCall* Entity::baseEntityCall() {
    return baseCall_ ? &*baseCall_ : nullptr;
}

const EntityCall* Entity::cellEntityCall() const {
    return cellCall_ ? &*cellCall_ : nullptr;
}

EntityCall* Entity::cellEntityCall() {
    return cellCall_ ? &*cellCall_ : nullptr;
}

void Entity::bindBaseEntityCall(ComponentId targetComponent, DeliveryClass deliveryClass) {
    baseCall_.emplace(id_, targetComponent, entityType(), deliveryClass);
}

void Entity::bindCellEntityCall(ComponentId targetComponent, DeliveryClass deliveryClass) {
    cellCall_.emplace(id_, targetComponent, entityType(), deliveryClass);
}

void Entity::clearBaseEntityCall() {
    baseCall_.reset();
}

void Entity::clearCellEntityCall() {
    cellCall_.reset();
}

bool Entity::bindMethodHandler(std::string method, MethodHandler handler) {
    if (method.empty() || !handler) {
        return false;
    }

    const auto* descriptor = def_->findMethod(method);
    if (descriptor == nullptr || !supportsMethodSide(side_, descriptor->side)) {
        return false;
    }

    methodHandlers_.insert_or_assign(std::move(method), std::move(handler));
    streamHandlers_.erase(method);
    return true;
}

bool Entity::bindStreamMethodHandler(std::string method, StreamMethodHandler handler) {
    if (method.empty() || !handler) {
        return false;
    }

    const auto* descriptor = def_->findMethod(method);
    if (descriptor == nullptr || !supportsMethodSide(side_, descriptor->side)) {
        return false;
    }

    streamHandlers_.insert_or_assign(std::move(method), std::move(handler));
    methodHandlers_.erase(method);
    return true;
}

bool Entity::hasMethodHandler(std::string_view method) const {
    auto key = std::string(method);
    return methodHandlers_.contains(key) || streamHandlers_.contains(key);
}

bool Entity::dispatchMethod(std::string_view method, std::span<const std::byte> payload) {
    const auto* descriptor = def_->findMethod(method);
    if (descriptor == nullptr || !supportsMethodSide(side_, descriptor->side)) {
        return false;
    }

    auto key = std::string(method);

    auto streamIter = streamHandlers_.find(key);
    if (streamIter != streamHandlers_.end()) {
        foundation::MemoryStream ms(payload.size());
        ms.writeBytes(payload.data(), payload.size());
        ms.resetRead();
        streamIter->second(*this, ms);
        return true;
    }

    const auto iter = methodHandlers_.find(key);
    if (iter == methodHandlers_.end()) {
        return false;
    }

    iter->second(*this, payload);
    return true;
}

bool Entity::dispatchInvocation(const RuntimeInvocation& invocation) {
    if (invocation.entityId != id_ || invocation.entityType != entityType()) {
        return false;
    }

    return dispatchMethod(invocation.method, invocation.payload);
}

void Entity::clearMethodHandlers() {
    methodHandlers_.clear();
    streamHandlers_.clear();
}

void Entity::subscribe(std::string event, EventCallback callback) {
    if (!event.empty() && callback) {
        eventSubscriptions_.emplace(std::move(event), std::move(callback));
    }
}

void Entity::unsubscribe(const std::string& event) {
    eventSubscriptions_.erase(event);
}

void Entity::emit(std::string_view event, std::span<const std::byte> data) {
    auto range = eventSubscriptions_.equal_range(std::string(event));
    for (auto it = range.first; it != range.second; ++it) {
        it->second(*this, event, data);
    }
}

void Entity::pushInput(InputAction action) {
    inputQueue_.push_back(std::move(action));
}

void Entity::processInput() {
    if (!actionHandler_ || inputQueue_.empty()) {
        inputQueue_.clear();
        return;
    }

    auto queue = std::move(inputQueue_);
    inputQueue_.clear();

    for (auto& action : queue) {
        actionHandler_(*this, action.name, action.payload);
    }
}

void Entity::clearInput() {
    inputQueue_.clear();
}

std::size_t Entity::pendingInputCount() const {
    return inputQueue_.size();
}

void Entity::setActionHandler(ActionHandler handler) {
    actionHandler_ = std::move(handler);
}

void Entity::setOnCreate(LifecycleCallback cb) {
    onCreate_ = std::move(cb);
}

void Entity::setOnRestore(LifecycleCallback cb) {
    onRestore_ = std::move(cb);
}

void Entity::setOnDestroy(LifecycleCallback cb) {
    onDestroy_ = std::move(cb);
}

void Entity::setOnCellReady(LifecycleCallback cb) {
    onCellReady_ = std::move(cb);
}

void Entity::setOnEnterSpace(SpaceCallback cb) {
    onEnterSpace_ = std::move(cb);
}

void Entity::setOnLeaveSpace(SpaceCallback cb) {
    onLeaveSpace_ = std::move(cb);
}

void Entity::setOnEnterAoI(AoICallback cb) {
    onEnterAoI_ = std::move(cb);
}

void Entity::setOnLeaveAoI(AoICallback cb) {
    onLeaveAoI_ = std::move(cb);
}

void Entity::setOnPositionChanged(PositionCallback cb) {
    onPositionChanged_ = std::move(cb);
}

void Entity::notifyCreate() {
    if (onCreate_) {
        onCreate_(*this);
    }
}

void Entity::notifyRestore() {
    if (onRestore_) {
        onRestore_(*this);
    }
}

void Entity::notifyDestroy() {
    if (onDestroy_) {
        onDestroy_(*this);
    }
}

void Entity::notifyCellReady() {
    if (onCellReady_) {
        onCellReady_(*this);
    }
}

void Entity::notifyEnterSpace(SpaceId spaceId) {
    if (onEnterSpace_) {
        onEnterSpace_(*this, spaceId);
    }
}

void Entity::notifyLeaveSpace(SpaceId spaceId) {
    if (onLeaveSpace_) {
        onLeaveSpace_(*this, spaceId);
    }
}

void Entity::notifyEnterAoI(EntityId other) {
    if (onEnterAoI_) {
        onEnterAoI_(*this, other);
    }
}

void Entity::notifyLeaveAoI(EntityId other) {
    if (onLeaveAoI_) {
        onLeaveAoI_(*this, other);
    }
}

void Entity::notifyPositionChanged(Vector3 oldPos, Vector3 newPos) {
    if (onPositionChanged_) {
        onPositionChanged_(*this, oldPos, newPos);
    }
}

void Entity::addTag(std::string tag) {
    tags_.insert(std::move(tag));
}

void Entity::removeTag(const std::string& tag) {
    tags_.erase(tag);
}

bool Entity::hasTag(const std::string& tag) const {
    return tags_.contains(tag);
}

void Entity::setVelocity(Vector3 velocity) {
    velocity_ = velocity;
    hasVelocity_ = true;
}

Vector3 Entity::velocity() const {
    return velocity_;
}

void Entity::clearVelocity() {
    velocity_ = {};
    hasVelocity_ = false;
}

bool Entity::hasVelocity() const {
    return hasVelocity_;
}

const std::unordered_set<std::string>& Entity::tags() const {
    return tags_;
}

void Entity::setTransport(IRuntimeTransport* transport) {
    transport_ = transport;
}

IRuntimeTransport* Entity::transport() const {
    return transport_;
}

SendResult Entity::callCell(std::string method, std::span<const std::byte> payload) {
    if (!transport_ || !cellCall_ || !cellCall_->isValid()) {
        return SendResult::NotConnected;
    }
    return cellCall_->call(*transport_, std::move(method), payload);
}

SendResult Entity::callBase(std::string method, std::span<const std::byte> payload) {
    if (!transport_ || !baseCall_ || !baseCall_->isValid()) {
        return SendResult::NotConnected;
    }
    return baseCall_->call(*transport_, std::move(method), payload);
}

void Entity::setTimerScheduleFns(TimerScheduleFn oneShot, TimerScheduleFn periodic) {
    addOneShotTimer_ = std::move(oneShot);
    addPeriodicTimerFn_ = std::move(periodic);
}

foundation::TimerHandle Entity::addTimer(Duration delay, EntityTimerCallback callback) {
    if (!addOneShotTimer_ || !callback) {
        return {};
    }
    return addOneShotTimer_(delay, std::move(callback));
}

foundation::TimerHandle Entity::addPeriodicTimer(Duration interval, EntityTimerCallback callback) {
    if (!addPeriodicTimerFn_ || !callback) {
        return {};
    }
    return addPeriodicTimerFn_(interval, std::move(callback));
}

void Entity::activate() {
    auto expected = EntityState::Creating;
    state_.compare_exchange_strong(expected, EntityState::Active,
                                    std::memory_order_release, std::memory_order_relaxed);
}

void Entity::beginMigration() {
    auto expected = EntityState::Active;
    state_.compare_exchange_strong(expected, EntityState::Migrating,
                                    std::memory_order_release, std::memory_order_relaxed);
}

void Entity::beginDestroy() {
    auto s = state_.load(std::memory_order_acquire);
    if (s == EntityState::Active || s == EntityState::Migrating) {
        state_.store(EntityState::Destroying, std::memory_order_release);
    }
}

void Entity::destroy() {
    auto s = state_.load(std::memory_order_acquire);
    if (s == EntityState::Destroying || s == EntityState::Active || s == EntityState::Migrating) {
        state_.store(EntityState::Destroyed, std::memory_order_release);
        clearBaseEntityCall();
        clearCellEntityCall();
        clearMethodHandlers();
        clearPropertyChangedCallbacks();
        clearInput();
        parentId_ = 0;
        children_.clear();
    }
}

bool Entity::isActive() const {
    return state_.load(std::memory_order_acquire) == EntityState::Active;
}

void Entity::setParent(EntityId parentId) {
    parentId_ = parentId;
}

EntityId Entity::parent() const {
    return parentId_;
}

bool Entity::hasParent() const {
    return parentId_ != 0;
}

void Entity::addChild(EntityId childId) {
    if (childId != 0 && childId != id_) {
        children_.insert(childId);
    }
}

void Entity::removeChild(EntityId childId) {
    children_.erase(childId);
}

const std::unordered_set<EntityId>& Entity::children() const {
    return children_;
}

bool Entity::hasChildren() const {
    return !children_.empty();
}

std::size_t Entity::childCount() const {
    return children_.size();
}

std::string_view Entity::getString(PropertyId id) const {
    return properties_.getString(id);
}

void Entity::setString(PropertyId id, std::string_view value) {
    properties_.setString(id, value);
}

std::string_view Entity::findString(const std::string& name) const {
    auto* desc = def_->findProperty(name);
    if (!desc) return {};
    return properties_.getString(desc->id);
}

bool Entity::setString(const std::string& name, std::string_view value) {
    auto* desc = def_->findProperty(name);
    if (!desc) return false;
    properties_.setString(desc->id, value);
    return true;
}

std::span<const std::byte> Entity::getBlob(PropertyId id) const {
    return properties_.getBlob(id);
}

void Entity::setBlob(PropertyId id, std::span<const std::byte> value) {
    properties_.setBlob(id, value);
}

bool Entity::isPropertyDirty(PropertyId id) const {
    return properties_.isDirty(id);
}

void Entity::clearDirtyFlags() {
    properties_.clearDirty();
}

std::vector<PropertyDelta> Entity::buildDirtyPropertyDelta(PropertyFlag excludeFlags) const {
    return properties_.buildDirtyDelta(excludeFlags);
}

std::vector<PropertyDelta> Entity::buildFullPropertySnapshot(PropertyFlag excludeFlags) const {
    return properties_.buildFullSnapshot(excludeFlags);
}

void Entity::applyPropertyDelta(std::span<const PropertyDelta> deltas, bool markDirty) {
    properties_.applyDelta(deltas, markDirty);
}

void Entity::setPropertyChangedCallback(PropertyId id, PropertyChangeCallback callback) {
    if (!callback) {
        properties_.setChangeCallback(id, nullptr);
        return;
    }

    auto* self = this;
    properties_.setChangeCallback(id,
        [self, cb = std::move(callback)](PropertyId pid, const std::byte* oldVal,
                                          const std::byte* newVal, std::size_t size) {
            cb(*self, pid, oldVal, newVal, size);
        });
}

void Entity::clearPropertyChangedCallbacks() {
    properties_.clearChangeCallbacks();
}

const PropertyBlock& Entity::propertyBlock() const {
    return properties_;
}

PropertyBlock& Entity::propertyBlock() {
    return properties_;
}

}  // namespace theseed::runtime
