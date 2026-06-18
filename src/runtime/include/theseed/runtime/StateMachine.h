#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace theseed::runtime {

class Entity;

// Lightweight finite state machine for entity game states.
// Orthogonal to EntityState (lifecycle) — this tracks gameplay states like Idle, Combat, Dead.
class StateMachine final {
public:
    explicit StateMachine(Entity& owner);

    bool addState(std::string state);
    bool addTransition(std::string from, std::string to);

    // Wildcard: transition from any state
    bool addTransitionFromAny(std::string to);

    bool setState(const std::string& state);
    const std::string& state() const;
    bool isInState(std::string_view state) const;
    bool canTransitionTo(const std::string& target) const;

    bool hasState(const std::string& state) const;
    std::vector<std::string> states() const;
    std::vector<std::string> availableTransitions() const;

    using StateCallback = std::function<void(Entity&, const std::string& oldState,
                                             const std::string& newState)>;

    void setOnStateEnter(StateCallback cb);
    void setOnStateExit(StateCallback cb);

    void reset();

    static constexpr std::string_view kAnyState = "*";

private:
    Entity& owner_;
    std::string currentState_;
    std::unordered_set<std::string> states_;
    // from -> {to1, to2, ...}
    std::unordered_map<std::string, std::unordered_set<std::string>> transitions_;
    std::unordered_set<std::string> wildcardTargets_;
    StateCallback onEnter_;
    StateCallback onExit_;
};

}  // namespace theseed::runtime
