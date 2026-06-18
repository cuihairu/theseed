#include "theseed/runtime/GroupManager.h"
#include "theseed/runtime/Entity.h"

#include <utility>

namespace theseed::runtime {

EntityGroup::EntityGroup(GroupId id, EntityId leader)
    : id_(id), leader_(leader) {}

EntityGroup::GroupId EntityGroup::id() const {
    return id_;
}

EntityId EntityGroup::leader() const {
    return leader_;
}

void EntityGroup::setLeader(EntityId leader) {
    leader_ = leader;
}

bool EntityGroup::addMember(Entity& entity) {
    auto id = entity.id();
    if (members_.contains(id)) return false;

    members_.emplace(id, entity.ref());
    if (onMemberAdded_) {
        onMemberAdded_(*this, id);
    }
    return true;
}

bool EntityGroup::removeMember(EntityId entityId) {
    auto it = members_.find(entityId);
    if (it == members_.end()) return false;

    members_.erase(it);
    if (onMemberRemoved_) {
        onMemberRemoved_(*this, entityId);
    }
    return true;
}

bool EntityGroup::isMember(EntityId entityId) const {
    return members_.contains(entityId);
}

std::size_t EntityGroup::memberCount() const {
    return members_.size();
}

std::size_t EntityGroup::activeMemberCount() const {
    std::size_t count = 0;
    for (auto& [id, ref] : members_) {
        if (ref.isValid()) ++count;
    }
    return count;
}

std::vector<Entity*> EntityGroup::activeMembers() const {
    std::vector<Entity*> result;
    result.reserve(members_.size());
    for (auto& [id, ref] : members_) {
        if (auto* e = ref.get()) {
            result.push_back(e);
        }
    }
    return result;
}

void EntityGroup::forEachMember(const std::function<void(Entity&)>& callback) const {
    for (auto& [id, ref] : members_) {
        if (auto* e = ref.get()) {
            callback(*e);
        }
    }
}

void EntityGroup::broadcast(const std::string& method, std::span<const std::byte> payload) {
    for (auto& [id, ref] : members_) {
        if (auto* e = ref.get()) {
            e->dispatchMethod(method, payload);
        }
    }
}

void EntityGroup::cleanup() {
    std::erase_if(members_, [](const auto& pair) {
        return !pair.second.isValid();
    });
}

void EntityGroup::setOnMemberAdded(MemberCallback cb) {
    onMemberAdded_ = std::move(cb);
}

void EntityGroup::setOnMemberRemoved(MemberCallback cb) {
    onMemberRemoved_ = std::move(cb);
}

// GroupManager

EntityGroup* GroupManager::createGroup(EntityId leader) {
    auto id = nextId_++;
    auto group = std::make_unique<EntityGroup>(id, leader);
    auto* ptr = group.get();
    groups_.emplace(id, std::move(group));
    return ptr;
}

EntityGroup* GroupManager::findGroup(EntityGroup::GroupId id) const {
    auto it = groups_.find(id);
    return it != groups_.end() ? it->second.get() : nullptr;
}

bool GroupManager::destroyGroup(EntityGroup::GroupId id) {
    return groups_.erase(id) > 0;
}

void GroupManager::cleanupAll() {
    for (auto& [id, group] : groups_) {
        group->cleanup();
    }
}

std::size_t GroupManager::groupCount() const {
    return groups_.size();
}

EntityGroup* GroupManager::findGroupByMember(EntityId memberId) const {
    for (auto& [id, group] : groups_) {
        if (group->isMember(memberId)) {
            return group.get();
        }
    }
    return nullptr;
}

std::vector<EntityGroup::GroupId> GroupManager::groupsForMember(EntityId memberId) const {
    std::vector<EntityGroup::GroupId> result;
    for (auto& [id, group] : groups_) {
        if (group->isMember(memberId)) {
            result.push_back(id);
        }
    }
    return result;
}

}  // namespace theseed::runtime
