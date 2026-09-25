// MachineSnapshotCodec 单元测试：text/JSON 两种快照格式化输出，
// 覆盖转义分支、当前进程优先展示、空进程列表与布尔渲染。
#include "theseed/control/machine/MachineSnapshotCodec.h"


#include <cstdint>
#include <iostream>
#include <string>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

using namespace theseed::control::machine;

#define TEST(name)                            \
    do {                                      \
        std::cout << "  " << name << "... ";  \
    } while (0)
#define PASS() std::cout << "OK" << std::endl
#define FAIL(msg)                                    \
    do {                                             \
        std::cout << "FAILED: " << msg << std::endl; \
        return 1;                                    \
    } while (0)

namespace {

NodeSummary makeSummary() {
    NodeSummary s;
    s.host.hostname = "node-1";
    s.host.platform = "linux";
    s.host.cpuUsage = 12.5;
    s.host.memoryUsage = 48.25;
    s.host.diskUsage = 61.0;
    s.host.loadAverage = 1.5;
    s.host.networkRxBytes = 100;
    s.host.networkTxBytes = 200;

    ProcessSummary self;
    self.name = "db\"app\n";
#ifdef _WIN32
    self.pid = static_cast<std::uint32_t>(GetCurrentProcessId());
#else
    self.pid = static_cast<std::uint32_t>(::getpid());
#endif
    self.port = 20003;
    // 原始字符序列：0.1<TAB>0<CR><反斜杠>r —— 覆盖 \t、\r、\\ 三条转义分支
    //（"\\r" 是反斜杠 + 字母 r 两个字符，真正的回车必须写成 \r）。
    self.version = "0.1\t0\r\\r";
    self.healthy = true;
    self.managed = true;

    ProcessSummary other;
    other.name = "baseapp";
    other.pid = self.pid + 1;
    other.port = 20004;
    other.version = "0.1.0";
    other.healthy = false;
    other.managed = false;

    s.processes = {other, self};  // 当前进程放第二位，验证优先选择
    s.draining = true;
    s.overloaded = false;
    return s;
}

}  // namespace

int main() {
    std::cout << "MachineSnapshotCodecTest:" << std::endl;

    const auto summary = makeSummary();

#ifdef _WIN32
    const auto pid = static_cast<std::uint32_t>(GetCurrentProcessId());
#else
    const auto pid = static_cast<std::uint32_t>(::getpid());
#endif

    TEST("formatSnapshotText renders host metrics and current process");
    const auto text = formatSnapshotText(summary);
    if (text.find("host=node-1\n") == std::string::npos ||
        text.find("platform=linux\n") == std::string::npos ||
        text.find("cpu_usage=12.50\n") == std::string::npos ||
        text.find("memory_usage=48.25\n") == std::string::npos ||
        text.find("disk_usage=61.00\n") == std::string::npos ||
        text.find("load_average=1.50\n") == std::string::npos ||
        text.find("process_count=2\n") == std::string::npos)
        FAIL("host section mismatch:\n" + text);
    // text 格式不转义：名字中的引号/换行按原始字符输出。
    if (text.find("process_name=db\"app\n\n") == std::string::npos ||
        text.find("process_pid=" + std::to_string(pid) + "\n") == std::string::npos ||
        text.find("process_healthy=true\n") == std::string::npos)
        FAIL("current-process section missing or wrong:\n" + text);
    PASS();

    TEST("formatSnapshotText without processes omits process block");
    NodeSummary empty = makeSummary();
    empty.processes.clear();
    const auto emptyText = formatSnapshotText(empty);
    if (emptyText.find("process_count=0\n") == std::string::npos ||
        emptyText.find("process_name=") != std::string::npos)
        FAIL("empty process list should omit process block:\n" + emptyText);
    PASS();

    TEST("formatSnapshotJson escapes strings and renders flags");
    const auto json = formatSnapshotJson(summary);
    if (json.find("\"hostname\":\"node-1\"") == std::string::npos ||
        json.find("\"cpuUsage\":12.50") == std::string::npos ||
        json.find("\"networkRxBytes\":100") == std::string::npos)
        FAIL("host JSON mismatch:\n" + json);
    if (json.find("\"name\":\"db\\\"app\\n\"") == std::string::npos ||
        json.find("\"version\":\"0.1\\t0\\r\\\\r\"") == std::string::npos)
        FAIL("string escaping mismatch:\n" + json);
    if (json.find("\"pid\":" + std::to_string(pid) + ",") == std::string::npos ||
        json.find("\"healthy\":true") == std::string::npos ||
        json.find("\"managed\":false") == std::string::npos)
        FAIL("process JSON mismatch:\n" + json);
    if (json.find("\"draining\":true") == std::string::npos ||
        json.find("\"overloaded\":false") == std::string::npos)
        FAIL("flag JSON mismatch:\n" + json);
    if (json.find("},{") == std::string::npos)
        FAIL("expected two processes separated by comma:\n" + json);
    PASS();

    TEST("formatSnapshotJson with empty process list");
    const auto emptyJson = formatSnapshotJson(empty);
    if (emptyJson.find("\"processes\":[]") == std::string::npos)
        FAIL("empty processes array mismatch:\n" + emptyJson);
    PASS();

    TEST("formatSnapshotText falls back to first process when pid absent");
    {
        NodeSummary foreign = makeSummary();
        for (auto& p : foreign.processes) {
            p.pid += 1'000'000;  // 无一匹配当前 pid：selectDisplayProcess 走 front() 兜底
        }
        const auto foreignText = formatSnapshotText(foreign);
        if (foreignText.find("process_name=baseapp\n") == std::string::npos)
            FAIL("should display first process:\n" + foreignText);
        if (foreignText.find("process_healthy=false\n") == std::string::npos)
            FAIL("unhealthy flag should render false:\n" + foreignText);
        PASS();
    }

    TEST("formatSnapshotJson renders overloaded true and draining false");
    {
        NodeSummary hot = makeSummary();
        hot.overloaded = true;
        hot.draining = false;
        const auto hotJson = formatSnapshotJson(hot);
        if (hotJson.find("\"overloaded\":true") == std::string::npos ||
            hotJson.find("\"draining\":false") == std::string::npos)
            FAIL("flag rendering mismatch:\n" + hotJson);
        PASS();
    }

    std::cout << "\nAll MachineSnapshotCodec tests passed!" << std::endl;
    return 0;
}
