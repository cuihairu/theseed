#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace theseed::runtime {

class Entity;

enum class BehaviorStatus : std::uint8_t {
    Success = 0,
    Failure,
    Running,
};

class BehaviorNode {
public:
    virtual ~BehaviorNode() = default;
    virtual BehaviorStatus execute(Entity& entity) = 0;
    virtual void reset();
};

// Sequence: executes children in order. Fails if any child fails.
// Succeeds when all children succeed. Running if current child is running.
class SequenceNode final : public BehaviorNode {
public:
    void addChild(std::unique_ptr<BehaviorNode> child);
    BehaviorStatus execute(Entity& entity) override;
    void reset() override;
    std::size_t childCount() const;

private:
    std::vector<std::unique_ptr<BehaviorNode>> children_;
    std::size_t current_ = 0;
};

// Selector: executes children in order. Succeeds if any child succeeds.
// Fails when all children fail. Running if current child is running.
class SelectorNode final : public BehaviorNode {
public:
    void addChild(std::unique_ptr<BehaviorNode> child);
    BehaviorStatus execute(Entity& entity) override;
    void reset() override;
    std::size_t childCount() const;

private:
    std::vector<std::unique_ptr<BehaviorNode>> children_;
    std::size_t current_ = 0;
};

// Action: leaf node wrapping a lambda that returns BehaviorStatus.
class ActionNode final : public BehaviorNode {
public:
    using Action = std::function<BehaviorStatus(Entity&)>;
    explicit ActionNode(Action action);
    BehaviorStatus execute(Entity& entity) override;

private:
    Action action_;
};

// Condition: guard decorator. Executes child only if condition returns true.
// Returns Failure if condition fails.
class ConditionNode final : public BehaviorNode {
public:
    using Condition = std::function<bool(Entity&)>;
    ConditionNode(Condition condition, std::unique_ptr<BehaviorNode> child);
    BehaviorStatus execute(Entity& entity) override;
    void reset() override;

private:
    Condition condition_;
    std::unique_ptr<BehaviorNode> child_;
};

// Inverter: inverts Success↔Failure of its child. Running passes through.
class InverterNode final : public BehaviorNode {
public:
    explicit InverterNode(std::unique_ptr<BehaviorNode> child);
    BehaviorStatus execute(Entity& entity) override;
    void reset() override;

private:
    std::unique_ptr<BehaviorNode> child_;
};

// Repeat: re-executes child until it returns Failure (or max count reached).
// Returns Success when done, Running while child is Running or repeating.
class RepeatNode final : public BehaviorNode {
public:
    explicit RepeatNode(std::unique_ptr<BehaviorNode> child, std::size_t maxCount = 0);
    BehaviorStatus execute(Entity& entity) override;
    void reset() override;

private:
    std::unique_ptr<BehaviorNode> child_;
    std::size_t maxCount_;  // 0 = unlimited
    std::size_t count_ = 0;
};

// Succeeder: always returns Success regardless of child result.
class SucceederNode final : public BehaviorNode {
public:
    explicit SucceederNode(std::unique_ptr<BehaviorNode> child);
    BehaviorStatus execute(Entity& entity) override;
    void reset() override;

private:
    std::unique_ptr<BehaviorNode> child_;
};

// BehaviorTree: top-level wrapper.
class BehaviorTree final {
public:
    explicit BehaviorTree(std::unique_ptr<BehaviorNode> root);
    BehaviorStatus tick(Entity& entity);
    void reset();
    bool isRunning() const;

private:
    std::unique_ptr<BehaviorNode> root_;
    bool running_ = false;
};

// Builder helpers for fluent construction
namespace bt {

std::unique_ptr<SequenceNode> sequence();
std::unique_ptr<SelectorNode> selector();
std::unique_ptr<ActionNode> action(ActionNode::Action fn);
std::unique_ptr<ConditionNode> condition(ConditionNode::Condition cond, std::unique_ptr<BehaviorNode> child);
std::unique_ptr<InverterNode> inverter(std::unique_ptr<BehaviorNode> child);
std::unique_ptr<RepeatNode> repeat(std::unique_ptr<BehaviorNode> child, std::size_t maxCount = 0);
std::unique_ptr<SucceederNode> succeeder(std::unique_ptr<BehaviorNode> child);

}  // namespace bt

}  // namespace theseed::runtime
