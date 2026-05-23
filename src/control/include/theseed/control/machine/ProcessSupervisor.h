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
    virtual ~IProcessSupervisor() = default;

    virtual std::vector<ProcessSummary> listProcesses() const = 0;
    virtual bool start(const std::string& target) = 0;
    virtual bool stop(std::uint32_t pid) = 0;
    virtual bool restart(std::uint32_t pid) = 0;
};

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

private:
    struct ChildProcess;

    static std::vector<std::string> splitCommandLine(const std::string& commandLine);
    static std::string basenameOf(const std::string& commandLine);
    void reapManagedProcesses() const;
    bool terminateManagedProcess(std::uint32_t pid);

    mutable std::mutex mutex_;
    mutable std::unordered_map<std::uint32_t, std::unique_ptr<ChildProcess>> managedProcesses_;
};

}  // namespace theseed::control::machine
