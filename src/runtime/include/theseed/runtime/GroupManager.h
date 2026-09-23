#pragma once

#include "theseed/runtime/EntityRef.h"
#include "theseed/runtime/RuntimeTypes.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace theseed::runtime {

class Entity;

class EntityGroup final {
public:
    using GroupId = std::uint64_t;

    EntityGroup(GroupId id, EntityId leader);

    GroupId id() const;
    EntityId leader() const;
    void setLeader(EntityId leader);

    bool addMember(Entity& entity);
    bool removeMember(EntityId entityId);
    bool isMember(EntityId entityId) const;
    std::size_t memberCount() const;
    std::size_t activeMemberCount() const;

    std::vector<Entity*> activeMembers() const;
    void forEachMember(const std::function<void(Entity&)>& callback) const;

    void broadcast(const std::string& method, std::span<const std::byte> payload = {});

    void cleanup();

    using MemberCallback = std::function<void(EntityGroup& group, EntityId memberId)>;
    void setOnMemberAdded(MemberCallback cb);
    void setOnMemberRemoved(MemberCallback cb);

private:
    GroupId id_;
    EntityId leader_;
    std::unordered_map<EntityId, EntityRef> members_;
    MemberCallback onMemberAdded_;
    MemberCallback onMemberRemoved_;
};

class GroupManager final {
public:
    GroupManager() = default;

    EntityGroup* createGroup(EntityId leader);
    EntityGroup* findGroup(EntityGroup::GroupId id) const;
    bool destroyGroup(EntityGroup::GroupId id);

    void cleanupAll();
    std::size_t groupCount() const;

    EntityGroup* findGroupByMember(EntityId memberId) const;
    std::vector<EntityGroup::GroupId> groupsForMember(EntityId memberId) const;

private:
    EntityGroup::GroupId nextId_ = 1;
    std::unordered_map<EntityGroup::GroupId, std::unique_ptr<EntityGroup>> groups_;
};

}  // namespace theseed::runtime
