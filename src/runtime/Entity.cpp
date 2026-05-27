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
    return true;
}

bool Entity::hasMethodHandler(std::string_view method) const {
    return methodHandlers_.contains(std::string(method));
}

bool Entity::dispatchMethod(std::string_view method, std::span<const std::byte> payload) {
    const auto* descriptor = def_->findMethod(method);
    if (descriptor == nullptr || !supportsMethodSide(side_, descriptor->side)) {
        return false;
    }

    const auto iter = methodHandlers_.find(std::string(method));
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

void Entity::setOnCreate(LifecycleCallback cb) {
    onCreate_ = std::move(cb);
}

void Entity::setOnDestroy(LifecycleCallback cb) {
    onDestroy_ = std::move(cb);
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

void Entity::notifyCreate() {
    if (onCreate_) {
        onCreate_(*this);
    }
}

void Entity::notifyDestroy() {
    if (onDestroy_) {
        onDestroy_(*this);
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

void Entity::addTag(std::string tag) {
    tags_.insert(std::move(tag));
}

void Entity::removeTag(const std::string& tag) {
    tags_.erase(tag);
}

bool Entity::hasTag(const std::string& tag) const {
    return tags_.contains(tag);
}

const std::unordered_set<std::string>& Entity::tags() const {
    return tags_;
}

void Entity::activate() {
    state_.store(EntityState::Active, std::memory_order_release);
}

void Entity::beginMigration() {
    state_.store(EntityState::Migrating, std::memory_order_release);
}

void Entity::beginDestroy() {
    state_.store(EntityState::Destroying, std::memory_order_release);
}

void Entity::destroy() {
    state_.store(EntityState::Destroyed, std::memory_order_release);
    clearBaseEntityCall();
    clearCellEntityCall();
    clearMethodHandlers();
    clearPropertyChangedCallbacks();
}

bool Entity::isPropertyDirty(PropertyId id) const {
    return properties_.isDirty(id);
}

void Entity::clearDirtyFlags() {
    properties_.clearDirty();
}

std::vector<PropertyDelta> Entity::buildDirtyPropertyDelta() const {
    return properties_.buildDirtyDelta();
}

std::vector<PropertyDelta> Entity::buildFullPropertySnapshot() const {
    return properties_.buildFullSnapshot();
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
