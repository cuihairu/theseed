#include "theseed/control/machine/MachineSnapshotCodec.h"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <iomanip>
#include <sstream>

namespace theseed::control::machine {

namespace {

std::uint32_t currentProcessId() {
#ifdef _WIN32
    return static_cast<std::uint32_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint32_t>(getpid());
#endif
}

const ProcessSummary* selectDisplayProcess(const std::vector<ProcessSummary>& processes) {
    const auto currentPid = currentProcessId();
    for (const auto& process : processes) {
        if (process.pid == currentPid) {
            return &process;
        }
    }

    return processes.empty() ? nullptr : &processes.front();
}

std::string escapeJson(const std::string& input) {
    std::string output;
    output.reserve(input.size() + 8);

    for (const char ch : input) {
        switch (ch) {
            case '\\':
                output += "\\\\";
                break;
            case '"':
                output += "\\\"";
                break;
            case '\n':
                output += "\\n";
                break;
            case '\r':
                output += "\\r";
                break;
            case '\t':
                output += "\\t";
                break;
            default:
                output += ch;
                break;
        }
    }

    return output;
}

void appendProcessJson(std::ostringstream& out, const ProcessSummary& process) {
    out << "{"
        << "\"name\":\"" << escapeJson(process.name) << "\","
        << "\"pid\":" << process.pid << ","
        << "\"port\":" << process.port << ","
        << "\"version\":\"" << escapeJson(process.version) << "\","
        << "\"healthy\":" << (process.healthy ? "true" : "false") << ","
        << "\"managed\":" << (process.managed ? "true" : "false")
        << "}";
}

}  // namespace

std::string formatSnapshotText(const NodeSummary& summary) {
    std::ostringstream out;
    out << "host=" << summary.host.hostname << '\n'
        << "platform=" << summary.host.platform << '\n'
        << std::fixed << std::setprecision(2)
        << "cpu_usage=" << summary.host.cpuUsage << '\n'
        << "memory_usage=" << summary.host.memoryUsage << '\n'
        << "disk_usage=" << summary.host.diskUsage << '\n'
        << "load_average=" << summary.host.loadAverage << '\n'
        << "process_count=" << summary.processes.size() << '\n';

    if (const auto* process = selectDisplayProcess(summary.processes)) {
        out << "process_name=" << process->name << '\n'
            << "process_pid=" << process->pid << '\n'
            << "process_healthy=" << (process->healthy ? "true" : "false") << '\n';
    }

    return out.str();
}

std::string formatSnapshotJson(const NodeSummary& summary) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(2);
    out << "{"
        << "\"host\":{"
        << "\"hostname\":\"" << escapeJson(summary.host.hostname) << "\","
        << "\"platform\":\"" << escapeJson(summary.host.platform) << "\","
        << "\"cpuUsage\":" << summary.host.cpuUsage << ","
        << "\"memoryUsage\":" << summary.host.memoryUsage << ","
        << "\"diskUsage\":" << summary.host.diskUsage << ","
        << "\"loadAverage\":" << summary.host.loadAverage << ","
        << "\"networkRxBytes\":" << summary.host.networkRxBytes << ","
        << "\"networkTxBytes\":" << summary.host.networkTxBytes
        << "},"
        << "\"processes\":[";

    for (std::size_t index = 0; index < summary.processes.size(); ++index) {
        if (index != 0) {
            out << ',';
        }
        appendProcessJson(out, summary.processes[index]);
    }

    out << "],"
        << "\"draining\":" << (summary.draining ? "true" : "false") << ","
        << "\"overloaded\":" << (summary.overloaded ? "true" : "false")
        << "}";

    return out.str();
}

}  // namespace theseed::control::machine
