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
