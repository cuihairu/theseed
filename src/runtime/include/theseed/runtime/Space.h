#pragma once

#include "theseed/runtime/Entity.h"
#include "theseed/runtime/RuntimeTypes.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace theseed::runtime {

class CoordinateSystem;
class CoordinateNode;

class ISpaceTopology {
public:
    virtual ~ISpaceTopology() = default;

    virtual CellId locateCell(const Vector3& position) const = 0;
    virtual std::vector<CellId> getAdjacentCells(const Vector3& position, float radius) const = 0;
    virtual void onTopologyChanged(std::function<void()> callback) = 0;
    virtual void reportLoad(CellId cell, float load) = 0;
    virtual void rebalance() = 0;
};

class SingleCellTopology final : public ISpaceTopology {
public:
    explicit SingleCellTopology(CellId cellId = 1);

    CellId locateCell(const Vector3& position) const override;
    std::vector<CellId> getAdjacentCells(const Vector3& position, float radius) const override;
    void onTopologyChanged(std::function<void()> callback) override;
    void reportLoad(CellId cell, float load) override;
    void rebalance() override;

    CellId cellId() const;
    float lastReportedLoad() const;

private:
    CellId cellId_ = 1;
    float lastReportedLoad_ = 0.0F;
    std::function<void()> callback_{};
};

struct SpaceConfig final {
    std::string name;
};

enum class SpaceState : std::uint8_t {
    Created = 0,
    Running,
    Draining,
    Shutdown,
};

class Space final {
public:
    Space(SpaceId id, std::string name, std::unique_ptr<ISpaceTopology> topology);

    void initialize(const SpaceConfig& config = {});
    void beginDrain();
    void shutdown();

    SpaceId id() const;
    const std::string& name() const;
    SpaceState state() const;

    void addEntity(Entity& entity, const Vector3& position);
    void removeEntity(EntityId entityId);
    Entity* findEntity(EntityId entityId) const;

    void updateEntityPosition(EntityId entityId, const Vector3& position);
    std::optional<Vector3> entityPosition(EntityId entityId) const;
    std::vector<Entity*> entities() const;
    std::vector<Entity*> queryRange(const Vector3& center, float radius) const;
    std::size_t entityCount() const;

    CoordinateSystem& coordinateSystem();
    const CoordinateSystem& coordinateSystem() const;
    ISpaceTopology& topology();
    const ISpaceTopology& topology() const;

private:
    struct Member final {
        Entity* entity = nullptr;
        std::unique_ptr<CoordinateNode> node;
    };

    SpaceId id_ = 0;
    std::string name_;
    SpaceState state_ = SpaceState::Created;
    std::unique_ptr<ISpaceTopology> topology_;
    std::unique_ptr<CoordinateSystem> coordinateSystem_;
    std::unordered_map<EntityId, Member> entities_;
};

}  // namespace theseed::runtime
