#include "theseed/runtime/GhostManager.h"
#include "theseed/runtime/RuntimeTransport.h"

#include <stdexcept>
#include <utility>

namespace theseed::runtime {

void GhostManager::attach(Entity& owner) {
    owner_ = &owner;
}

void GhostManager::detach() {
    owner_ = nullptr;
    realComponent_.reset();
    ghostComponent_.reset();
    route_.reset();
}

Entity* GhostManager::owner() const {
    return owner_;
}

void GhostManager::setReal(ComponentId localComponent) {
    localComponent_ = localComponent;
    realComponent_.reset();
}

void GhostManager::setGhost(ComponentId realComponent) {
    if (realComponent == 0) {
        throw std::invalid_argument("real component must not be zero");
    }

    realComponent_ = realComponent;
}

bool GhostManager::isReal() const {
    return !realComponent_.has_value();
}

bool GhostManager::isGhost() const {
    return realComponent_.has_value();
}

void GhostManager::createGhost(ComponentId targetCellApp) {
    if (targetCellApp == 0) {
        throw std::invalid_argument("ghost target must not be zero");
    }

    ghostComponent_ = targetCellApp;
}

void GhostManager::destroyGhost() {
    ghostComponent_.reset();
}

bool GhostManager::hasGhost() const {
    return ghostComponent_.has_value();
}

ComponentId GhostManager::ghostTarget() const {
    if (!ghostComponent_) {
        throw std::logic_error("ghost target is not set");
    }

    return *ghostComponent_;
}

void GhostManager::setRoute(ComponentId targetComponent, Duration ttl, TimePoint now) {
    if (targetComponent == 0) {
        throw std::invalid_argument("route target must not be zero");
    }

    route_ = GhostRoute{
        .targetComponent = targetComponent,
        .expiry = now + ttl,
    };
}

void GhostManager::clearRoute() {
    route_.reset();
}

std::optional<ComponentId> GhostManager::routeTarget(TimePoint now) const {
    if (!route_.has_value()) {
        return std::nullopt;
    }

    if (now >= route_->expiry) {
        return std::nullopt;
    }

    return route_->targetComponent;
}

std::optional<RuntimeInvocation> GhostManager::forwardToReal(std::string method,
                                                             std::span<const std::byte> payload) const {
    if (owner_ == nullptr) {
        throw std::logic_error("ghost manager is not attached");
    }

    if (!isGhost()) {
        return std::nullopt;
    }

    const auto targetComponent = routeTarget().value_or(*realComponent_);
    EntityCall call(owner_->id(), targetComponent, owner_->entityType());
    return call.buildInvocation(std::move(method), payload);
}

}  // namespace theseed::runtime
