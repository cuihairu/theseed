# machine/control 的进程治理与套接字探针测试依赖 POSIX（fork/waitpid、
# ::getpid、POSIX socket），Windows 暂不纳入；HostProbe 与 OpsControlCenter
# 的测试平台中立、全平台跑（macos job 依赖 HostProbe 测试首验 Apple 胶合，
# 不可门控掉）。MachineAgent/MachineDaemon/ProcessPortScanner 依赖 Linux 专用
# procfs / ss 接口，macOS 上全红，门控至 Linux。
if(NOT WIN32)
add_executable(theseed_machine_snapshot_codec_test MachineSnapshotCodecTest.cpp)

target_link_libraries(
    theseed_machine_snapshot_codec_test
    PRIVATE theseed_project_options theseed_test_options
    PRIVATE theseed_control)

add_test(NAME theseed_machine_snapshot_codec_test COMMAND theseed_machine_snapshot_codec_test)

add_executable(theseed_process_supervisor_test ProcessSupervisorTest.cpp)

target_link_libraries(
    theseed_process_supervisor_test
    PRIVATE theseed_project_options theseed_test_options
    PRIVATE theseed_control)

add_test(NAME theseed_process_supervisor_test COMMAND theseed_process_supervisor_test)
endif()

if(UNIX AND NOT APPLE)
add_executable(theseed_machine_agent_test MachineAgentTest.cpp)

target_link_libraries(
    theseed_machine_agent_test
    PRIVATE theseed_project_options theseed_test_options
    PRIVATE theseed_control)

add_test(NAME theseed_machine_agent_test COMMAND theseed_machine_agent_test)
endif()

add_executable(theseed_host_probe_test HostProbeTest.cpp)

target_link_libraries(
    theseed_host_probe_test
    PRIVATE theseed_project_options theseed_test_options
    PRIVATE theseed_control)

add_test(NAME theseed_host_probe_test COMMAND theseed_host_probe_test)

if(UNIX AND NOT APPLE)
add_executable(theseed_process_port_scanner_test ProcessPortScannerTest.cpp)

target_link_libraries(
    theseed_process_port_scanner_test
    PRIVATE theseed_project_options theseed_test_options
    PRIVATE theseed_control theseed_ops theseed_runtime)

add_test(NAME theseed_process_port_scanner_test COMMAND theseed_process_port_scanner_test)

add_executable(theseed_machine_daemon_test MachineDaemonTest.cpp)

target_link_libraries(
    theseed_machine_daemon_test
    PRIVATE theseed_project_options theseed_test_options
    PRIVATE theseed_control theseed_ops theseed_runtime)

add_test(NAME theseed_machine_daemon_test COMMAND theseed_machine_daemon_test)
endif()

add_executable(theseed_ops_control_center_test OpsControlCenterTest.cpp)

target_link_libraries(
    theseed_ops_control_center_test
    PRIVATE theseed_project_options theseed_test_options
    PRIVATE theseed_control theseed_runtime)

add_test(NAME theseed_ops_control_center_test COMMAND theseed_ops_control_center_test)

# OTLP trace 导出器的回环 e2e 用例走真实 TCP（TcpListener/TcpConnection），
# 同仓内 TCP 测试惯例门控 Linux。
if(UNIX AND NOT APPLE)
add_executable(theseed_otlp_trace_exporter_test OtlpTraceExporterTest.cpp)

target_link_libraries(
    theseed_otlp_trace_exporter_test
    PRIVATE theseed_project_options theseed_test_options
    PRIVATE theseed_control)

add_test(NAME theseed_otlp_trace_exporter_test COMMAND theseed_otlp_trace_exporter_test)
endif()
