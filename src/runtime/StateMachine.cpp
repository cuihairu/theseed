#include "theseed/runtime/StateMachine.h"
#include "theseed/runtime/Entity.h"

#include <utility>

namespace theseed::runtime {

StateMachine::StateMachine(Entity& owner) : owner_(owner) {}

bool StateMachine::addState(std::string state) {
    if (state.empty()) return false;
    return states_.insert(std::move(state)).second;
}

bool StateMachine::addTransition(std::string from, std::string to) {
    if (from.empty() || to.empty()) return false;
    if (!states_.contains(from) || !states_.contains(to)) return false;
    return transitions_[std::move(from)].insert(std::move(to)).second;
}

bool StateMachine::addTransitionFromAny(std::string to) {
    if (to.empty()) return false;
    if (!states_.contains(to)) return false;
    return wildcardTargets_.insert(std::move(to)).second;
}

bool StateMachine::setState(const std::string& state) {
    if (state.empty() || !states_.contains(state)) return false;

    if (!currentState_.empty() && currentState_ == state) return true;

    if (!canTransitionTo(state)) return false;

    auto oldState = std::move(currentState_);
    currentState_ = state;

    if (onExit_ && !oldState.empty()) {
        onExit_(owner_, oldState, currentState_);
    }
    if (onEnter_) {
        onEnter_(owner_, oldState, currentState_);
    }
    return true;
}

const std::string& StateMachine::state() const {
    return currentState_;
}

bool StateMachine::isInState(std::string_view state) const {
    return currentState_ == state;
}

bool StateMachine::canTransitionTo(const std::string& target) const {
    if (currentState_.empty()) return true;  // initial state

    auto it = transitions_.find(currentState_);
    if (it != transitions_.end() && it->second.contains(target)) return true;

    if (wildcardTargets_.contains(target)) return true;

    return false;
}

bool StateMachine::hasState(const std::string& state) const {
    return states_.contains(state);
}

std::vector<std::string> StateMachine::states() const {
    return {states_.begin(), states_.end()};
}

std::vector<std::string> StateMachine::availableTransitions() const {
    std::vector<std::string> result;
    if (currentState_.empty()) {
        result.assign(states_.begin(), states_.end());
        return result;
    }

    auto it = transitions_.find(currentState_);
    if (it != transitions_.end()) {
        result.assign(it->second.begin(), it->second.end());
    }

    for (const auto& target : wildcardTargets_) {
        if (target != currentState_ && std::find(result.begin(), result.end(), target) == result.end()) {
            result.push_back(target);
        }
    }
    return result;
}

void StateMachine::setOnStateEnter(StateCallback cb) {
    onEnter_ = std::move(cb);
}

void StateMachine::setOnStateExit(StateCallback cb) {
    onExit_ = std::move(cb);
}

void StateMachine::reset() {
    currentState_.clear();
}

}  // namespace theseed::runtime
