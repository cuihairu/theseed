#include "theseed/control/machine/MachineAgent.h"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <memory>
#include <utility>

namespace theseed::control::machine {

MachineAgent::MachineAgent(std::unique_ptr<IHostProbe> hostProbe,
                           std::unique_ptr<IProcessSupervisor> processSupervisor)
    : MachineAgent(std::move(hostProbe), std::move(processSupervisor), nullptr) {}

MachineAgent::MachineAgent(std::unique_ptr<IHostProbe> hostProbe,
                           std::unique_ptr<IProcessSupervisor> processSupervisor,
                           INodeReportSink* reportSink)
    : hostProbe_(std::move(hostProbe)),
      processSupervisor_(std::move(processSupervisor)),
      reportSink_(reportSink) {}

NodeSummary MachineAgent::snapshot() {
    NodeSummary summary;
    summary.host = hostProbe_->sample();
    summary.processes = processSupervisor_->listProcesses();
    return summary;
}

std::vector<ProcessSummary> MachineAgent::enumerateHostProcesses() {
    // 与 snapshot 同源（supervisor 的全主机枚举），只省掉资源采样——
    // 治理动作只需要进程身份（pid/name/managed），不需要主机水位。
    return processSupervisor_->listProcesses();
}

bool MachineAgent::terminateHostProcess(std::uint32_t pid) {
    // 策略判定（来源白名单/目标名单/保护类）在 daemon 侧完成，这里纯转发。
    return processSupervisor_->terminateUnmanaged(pid);
}

void MachineAgent::setReportSink(INodeReportSink* reportSink) {
    reportSink_ = reportSink;
}

void MachineAgent::report() {
    if (reportSink_ == nullptr) {
        return;  // 未注册出口：上报是能力而非义务
    }

    NodeReport report;
    report.summary = snapshot();
    report.nodeId = report.summary.host.hostname;
    report.timestamp = std::chrono::system_clock::now();
    if (profileMetaSource_ != nullptr) {
        report.profiles = profileMetaSource_->profileMetas();
    }
    reportSink_->publish(report);
}

void MachineAgent::setProfileMetaSource(
    IProfileMetaSource* profileMetaSource) {
    profileMetaSource_ = profileMetaSource;
}

bool MachineAgent::execute(const std::string& command, const std::string& args) {
    if (command == "start") {
        return processSupervisor_->start(args);
    }

    if (command == "stop") {
        std::uint32_t pid = 0;
        const auto* begin = args.data();
        const auto* end = begin + args.size();
        const auto [ptr, ec] = std::from_chars(begin, end, pid);
        if (ec != std::errc{} || ptr != end) {
            return false;
        }
        return processSupervisor_->stop(pid);
    }

    if (command == "restart") {
        std::uint32_t pid = 0;
        const auto* begin = args.data();
        const auto* end = begin + args.size();
        const auto [ptr, ec] = std::from_chars(begin, end, pid);
        if (ec != std::errc{} || ptr != end) {
            return false;
        }
        return processSupervisor_->restart(pid);
    }

    return false;
}

}  // namespace theseed::control::machine