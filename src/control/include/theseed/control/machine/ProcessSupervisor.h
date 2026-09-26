#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace theseed::control::machine {

struct ProcessSummary {
    std::string name;
    std::uint32_t pid = 0;
    std::uint32_t port = 0;
    std::string version;
    bool healthy = false;
    bool managed = false;
};

class IProcessSupervisor {
public:
    virtual ~IProcessSupervisor() = default;  // LCOV_EXCL_LINE C++ ABI：trivial 虚析构是空体，gcc 不为其生成计数指令，D0/D1/D2 三符号变体恒 0（结构不可测）

    virtual std::vector<ProcessSummary> listProcesses() const = 0;
    virtual bool start(const std::string& target) = 0;
    virtual bool stop(std::uint32_t pid) = 0;
    virtual bool restart(std::uint32_t pid) = 0;

    // 主机级非受控进程处置（SIGTERM）：受管进程不走此路径——它们的唯一
    // 停止入口是 stop/restart，绕开会脱离 reap/记账（单一控制路径纪律）。
    // 返回 false 表示拒绝（目标是受管进程）或处置未生效（进程已不存在、
    // 平台未开放治理处置）。
    virtual bool terminateUnmanaged(std::uint32_t pid) = 0;
};

// 本进程 id（治理侧“不杀自己”守卫的判定输入；跨平台取值口径与
// listProcesses 枚举的 pid 一致）。
std::uint32_t currentProcessId();

class LocalProcessSupervisor final : public IProcessSupervisor {
public:
    LocalProcessSupervisor();
    ~LocalProcessSupervisor() override;

    LocalProcessSupervisor(const LocalProcessSupervisor&) = delete;
    LocalProcessSupervisor& operator=(const LocalProcessSupervisor&) = delete;

    std::vector<ProcessSummary> listProcesses() const override;
    bool start(const std::string& target) override;
    bool stop(std::uint32_t pid) override;
    bool restart(std::uint32_t pid) override;
    bool terminateUnmanaged(std::uint32_t pid) override;

    // 命令行解析工具（无状态，start 内部也走这两个）：对外暴露以便
    // CLI/测试直接复用，不必 fork 一个进程才能验证解析行为。
    static std::vector<std::string> splitCommandLine(const std::string& commandLine);
    static std::string basenameOf(const std::string& commandLine);

private:
    struct ChildProcess;

    void reapManagedProcesses() const;
    bool terminateManagedProcess(std::uint32_t pid);

    mutable std::mutex mutex_;
    mutable std::unordered_map<std::uint32_t, std::unique_ptr<ChildProcess>> managedProcesses_;
};

}  // namespace theseed::control::machine
