// LocalProcessSupervisor 单元测试：
// - 命令行切分（引号内空格、连续空白、空串）
// - basenameOf（引号剥离、空命令行回退 "unknown"）
// - start/stop/restart/listProcesses 用真实子进程驱动（fork /bin/sleep），
//   覆盖 supervise 的 reap 与 restart 路径；析构兜底终止用 waitpid(ECHILD) 验证。
// Windows 下只测纯字符串接口（进程 API 分支由 SmokeTest 覆盖）。
#include "theseed/control/machine/ProcessSupervisor.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <system_error>
#ifndef _WIN32
#include <sys/resource.h>
#endif
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

using theseed::control::machine::LocalProcessSupervisor;
using theseed::control::machine::ProcessSummary;

static int testsPassed = 0;
static int testsFailed = 0;

#define TEST(name)                                              \
    do {                                                        \
        std::cout << "  " << (name) << "... " << std::flush;    \
    } while (0)

#define PASS()                                                  \
    do {                                                        \
        std::cout << "OK\n";                                    \
        ++testsPassed;                                          \
    } while (0)

#define FAIL(msg)                                               \
    do {                                                        \
        std::cout << "FAIL: " << (msg) << "\n";                 \
        ++testsFailed;                                          \
    } while (0)

#define EXPECT(cond, msg)                                       \
    do {                                                        \
        if (!(cond)) FAIL(msg);                                 \
    } while (0)

namespace {

// 在 managed 列表里找名字匹配的受管进程。
const ProcessSummary* findManaged(const std::vector<ProcessSummary>& processes,
                                  const std::string& name) {
    for (const auto& p : processes) {
        if (p.managed && p.name == name) return &p;
    }
    return nullptr;
}

}  // namespace

static void test_split_command_line() {
    TEST("splitCommandLine: quotes/whitespace/empty");
    LocalProcessSupervisor supervisor;

    EXPECT(supervisor.splitCommandLine("\"a b\" c --flag").size() == 3,
           "quoted space should stay one token");

    EXPECT(supervisor.splitCommandLine("  a   b  ").size() == 2,
           "repeated whitespace should collapse");

    EXPECT(supervisor.splitCommandLine("").empty(), "empty line yields no tokens");

    EXPECT(supervisor.splitCommandLine("\"\"").empty(),
           "quote-only line yields no tokens");
    PASS();
}

static void test_basename_of() {
    TEST("basenameOf: quotes/paths/empty fallback");
    LocalProcessSupervisor supervisor;

    EXPECT(supervisor.basenameOf("\"/opt/theseed/node\"") == "node",
           "quoted path should strip quotes then basename");

    EXPECT(supervisor.basenameOf("/usr/bin/sleep") == "sleep",
           "absolute path should reduce to filename");

    // basenameOf 不负责切 token（调用方先 splitCommandLine 再传首 token），
    // 因此对含空格输入按整段路径名处理。
    EXPECT(supervisor.basenameOf("sleep --quiet") == "sleep --quiet",
           "no token splitting inside basenameOf");

    EXPECT(supervisor.basenameOf("") == "unknown", "empty command line fallback");
    PASS();
}

#ifndef _WIN32
static void test_start_stop_round_trip() {
    TEST("start+stop: fork /bin/sleep and terminate");
    LocalProcessSupervisor supervisor;

    EXPECT(!supervisor.start(""), "empty target must be rejected");

    if (!supervisor.start("/bin/sleep 30")) {
        FAIL("start /bin/sleep failed");
        return;
    }

    const auto processes = supervisor.listProcesses();
    const auto* managed = findManaged(processes, "sleep");
    if (managed == nullptr) {
        FAIL("managed sleep child not listed");
        return;
    }
    const std::uint32_t pid = managed->pid;

    if (!supervisor.stop(pid)) {
        FAIL("stop on managed pid failed");
        return;
    }
    EXPECT(!supervisor.stop(pid), "second stop on same pid must be false");
    EXPECT(findManaged(supervisor.listProcesses(), "sleep") == nullptr,
           "stopped child must disappear from managed list");
    PASS();
}

static void test_restart_replaces_child() {
    TEST("restart: old pid replaced by a fresh child");
    LocalProcessSupervisor supervisor;

    if (!supervisor.start("/bin/sleep 30")) {
        FAIL("start for restart failed");
        return;
    }
    const auto first = findManaged(supervisor.listProcesses(), "sleep");
    if (first == nullptr) {
        FAIL("first child not listed");
        return;
    }
    const std::uint32_t oldPid = first->pid;

    EXPECT(!supervisor.restart(4'000'000), "restart of unknown pid must be false");

    if (!supervisor.restart(oldPid)) {
        FAIL("restart of managed pid failed");
        return;
    }

    const auto after = findManaged(supervisor.listProcesses(), "sleep");
    if (after == nullptr) {
        FAIL("restarted child not listed");
        return;
    }
    EXPECT(after->pid != oldPid, "restart must spawn a new pid");
    EXPECT(supervisor.stop(after->pid), "stop restarted child");

    // 旧 pid 已被 terminate 收尸：waitpid 应报 ECHILD 而不是悬留僵尸。
    int status = 0;
    const pid_t waited = waitpid(static_cast<pid_t>(oldPid), &status, 0);
    EXPECT(waited == -1 && errno == ECHILD,
           "old pid should already be reaped after restart");
    PASS();
}

static void test_reap_removes_exited_children() {
    TEST("reap: exited child disappears after listProcesses");
    LocalProcessSupervisor supervisor;

    // /bin/true 立即退出——reap 路径应把它从 managed 清掉。
    if (!supervisor.start("/bin/true")) {
        FAIL("start /bin/true failed");
        return;
    }

    bool reaped = false;
    for (int i = 0; i < 200 && !reaped; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        reaped = findManaged(supervisor.listProcesses(), "true") == nullptr;
    }
    EXPECT(reaped, "exited child should be reaped from managed list");
    PASS();
}

static void test_destructor_terminates_remaining_children() {
    pid_t childPid = -1;
    {
        TEST("destructor terminates remaining children");
        LocalProcessSupervisor supervisor;
        if (!supervisor.start("/bin/sleep 30")) {
            FAIL("start for destructor test failed");
            return;
        }
        const auto* managed = findManaged(supervisor.listProcesses(), "sleep");
        if (managed == nullptr) {
            FAIL("destructor-test child not listed");
            return;
        }
        childPid = static_cast<pid_t>(managed->pid);
        PASS();  // 断言在作用域外做：析构后 waitpid 应为 ECHILD
    }

    int status = 0;
    const pid_t waited = waitpid(childPid, &status, 0);
    EXPECT(waited == -1 && errno == ECHILD,
           "destructor must terminate and reap remaining children");
    if (waited == -1 && errno == ECHILD) PASS();
}
#endif  // !_WIN32

#ifndef _WIN32
// fork 失败（RLIMIT_NPROC soft 压到 1 → EAGAIN）时 start 必须返回 false。
// 恢复 limit 必须放在断言之前：EXPECT 失败不会提前返回，但保持环境干净。
static void test_start_fork_failure() {
    TEST("start fails when fork cannot create a child");
    struct rlimit oldLimit {};
    if (getrlimit(RLIMIT_NPROC, &oldLimit) != 0) {
        PASS();  // 环境不支持，跳过
        return;
    }

    struct rlimit tight = oldLimit;
    tight.rlim_cur = 1;  // 当前 uid 已有进程数必然 >= 1 → fork EAGAIN
    if (setrlimit(RLIMIT_NPROC, &tight) != 0) {
        PASS();  // 权限不允许收紧，跳过
        return;
    }

    LocalProcessSupervisor supervisor;
    const bool started = supervisor.start("/bin/sleep 30");
    setrlimit(RLIMIT_NPROC, &oldLimit);

    EXPECT(!started, "start must be rejected when fork fails");
    PASS();
}
#endif

int main() {
    std::cout << "ProcessSupervisorTest:" << std::endl;
    test_split_command_line();
    test_basename_of();
#ifndef _WIN32
    test_start_stop_round_trip();
    test_restart_replaces_child();
    test_reap_removes_exited_children();
    test_destructor_terminates_remaining_children();
    test_start_fork_failure();
#endif

    std::cout << "  passed=" << testsPassed << " failed=" << testsFailed << "\n";
    return testsFailed == 0 ? 0 : 1;
}
