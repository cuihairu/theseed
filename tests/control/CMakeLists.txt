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

add_executable(theseed_machine_agent_test MachineAgentTest.cpp)

target_link_libraries(
    theseed_machine_agent_test
    PRIVATE theseed_project_options theseed_test_options
    PRIVATE theseed_control)

add_test(NAME theseed_machine_agent_test COMMAND theseed_machine_agent_test)

add_executable(theseed_host_probe_test HostProbeTest.cpp)

target_link_libraries(
    theseed_host_probe_test
    PRIVATE theseed_project_options theseed_test_options
    PRIVATE theseed_control)

add_test(NAME theseed_host_probe_test COMMAND theseed_host_probe_test)

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
    PRIVATE theseed_control theseed_runtime)

add_test(NAME theseed_machine_daemon_test COMMAND theseed_machine_daemon_test)
