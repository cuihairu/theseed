#pragma once

#include "theseed/runtime/Entity.h"

#include <optional>
#include <string>

namespace theseed::runtime {

struct GhostRoute final {
    ComponentId targetComponent = 0;
    TimePoint expiry{};
};

class GhostManager final {
public:
    GhostManager() = default;

    void attach(Entity& owner);
    void detach();
    Entity* owner() const;

    void setReal(ComponentId localComponent);
    void setGhost(ComponentId realComponent);
    bool isReal() const;
    bool isGhost() const;

    void createGhost(ComponentId targetCellApp);
    void destroyGhost();
    bool hasGhost() const;
    ComponentId ghostTarget() const;

    void setRoute(ComponentId targetComponent, Duration ttl, TimePoint now = Clock::now());
    void clearRoute();
    std::optional<ComponentId> routeTarget(TimePoint now = Clock::now()) const;

    std::optional<RuntimeInvocation> forwardToReal(std::string method,
                                                   std::span<const std::byte> payload = {}) const;

private:
    Entity* owner_ = nullptr;
    ComponentId localComponent_ = 0;
    std::optional<ComponentId> realComponent_{};
    std::optional<ComponentId> ghostComponent_{};
    std::optional<GhostRoute> route_{};
};

}  // namespace theseed::runtime
