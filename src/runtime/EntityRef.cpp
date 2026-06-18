#include "theseed/runtime/EntityRef.h"
#include "theseed/runtime/Entity.h"

namespace theseed::runtime {

EntityRef::EntityRef(Entity& entity)
    : id_(entity.id()),
      cached_(&entity),
      alive_(entity.aliveFlag()) {}

Entity* EntityRef::get() const {
    if (!alive_ || !*alive_) return nullptr;
    return cached_;
}

Entity& EntityRef::operator*() const {
    return *get();
}

Entity* EntityRef::operator->() const {
    return get();
}

bool EntityRef::isValid() const {
    return alive_ && *alive_ && cached_ != nullptr;
}

EntityRef::operator bool() const {
    return isValid();
}

EntityId EntityRef::entityId() const {
    return id_;
}

void EntityRef::reset() {
    id_ = 0;
    cached_ = nullptr;
    alive_.reset();
}

EntityRef EntityRef::fromEntity(Entity& entity) {
    return EntityRef(entity);
}

EntityRef EntityRef::invalid() {
    return EntityRef();
}

}  // namespace theseed::runtime
