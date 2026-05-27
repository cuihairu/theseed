add_executable(theseed_runtime_framework_test RuntimeFrameworkTest.cpp)

target_link_libraries(
    theseed_runtime_framework_test
    PRIVATE theseed_project_options
    PRIVATE theseed_runtime
    PRIVATE theseed_core)

add_test(NAME theseed_runtime_framework_test COMMAND theseed_runtime_framework_test)

add_executable(theseed_entity_core_test EntityCoreTest.cpp)

target_link_libraries(
    theseed_entity_core_test
    PRIVATE theseed_project_options
    PRIVATE theseed_runtime
    PRIVATE theseed_core)

add_test(NAME theseed_entity_core_test COMMAND theseed_entity_core_test)

add_executable(theseed_runtime_transport_test RuntimeTransportTest.cpp)

target_link_libraries(
    theseed_runtime_transport_test
    PRIVATE theseed_project_options
    PRIVATE theseed_runtime
    PRIVATE theseed_core)

add_test(NAME theseed_runtime_transport_test COMMAND theseed_runtime_transport_test)

add_executable(theseed_property_replication_test PropertyReplicationTest.cpp)

target_link_libraries(
    theseed_property_replication_test
    PRIVATE theseed_project_options
    PRIVATE theseed_runtime
    PRIVATE theseed_core)

add_test(NAME theseed_property_replication_test COMMAND theseed_property_replication_test)

add_executable(theseed_entity_migration_test EntityMigrationTest.cpp)

target_link_libraries(
    theseed_entity_migration_test
    PRIVATE theseed_project_options
    PRIVATE theseed_runtime
    PRIVATE theseed_core)

add_test(NAME theseed_entity_migration_test COMMAND theseed_entity_migration_test)

add_executable(theseed_witness_ghost_test WitnessGhostTest.cpp)

target_link_libraries(
    theseed_witness_ghost_test
    PRIVATE theseed_project_options
    PRIVATE theseed_runtime
    PRIVATE theseed_core)

add_test(NAME theseed_witness_ghost_test COMMAND theseed_witness_ghost_test)

add_executable(theseed_aoi_test AOITest.cpp)

target_link_libraries(
    theseed_aoi_test
    PRIVATE theseed_project_options
    PRIVATE theseed_runtime
    PRIVATE theseed_core)

add_test(NAME theseed_aoi_test COMMAND theseed_aoi_test)

add_executable(theseed_aoi_witness_integration_test AOIWitnessIntegrationTest.cpp)

target_link_libraries(
    theseed_aoi_witness_integration_test
    PRIVATE theseed_project_options
    PRIVATE theseed_runtime
    PRIVATE theseed_core)

add_test(NAME theseed_aoi_witness_integration_test COMMAND theseed_aoi_witness_integration_test)

add_executable(theseed_space_runtime_test SpaceRuntimeTest.cpp)

target_link_libraries(
    theseed_space_runtime_test
    PRIVATE theseed_project_options
    PRIVATE theseed_runtime
    PRIVATE theseed_core)

add_test(NAME theseed_space_runtime_test COMMAND theseed_space_runtime_test)

add_executable(theseed_cell_runtime_test CellRuntimeTest.cpp)

target_link_libraries(
    theseed_cell_runtime_test
    PRIVATE theseed_project_options
    PRIVATE theseed_runtime
    PRIVATE theseed_core)

add_test(NAME theseed_cell_runtime_test COMMAND theseed_cell_runtime_test)

add_executable(theseed_migration_runtime_test MigrationRuntimeTest.cpp)

target_link_libraries(
    theseed_migration_runtime_test
    PRIVATE theseed_project_options
    PRIVATE theseed_runtime
    PRIVATE theseed_core)

add_test(NAME theseed_migration_runtime_test COMMAND theseed_migration_runtime_test)

add_executable(theseed_cell_entity_timer_test CellEntityTimerTest.cpp)

target_link_libraries(
    theseed_cell_entity_timer_test
    PRIVATE theseed_project_options
    PRIVATE theseed_runtime
    PRIVATE theseed_core)

add_test(NAME theseed_cell_entity_timer_test COMMAND theseed_cell_entity_timer_test)

add_executable(theseed_entity_lifecycle_test EntityLifecycleTest.cpp)

target_link_libraries(
    theseed_entity_lifecycle_test
    PRIVATE theseed_project_options
    PRIVATE theseed_runtime
    PRIVATE theseed_core)

add_test(NAME theseed_entity_lifecycle_test COMMAND theseed_entity_lifecycle_test)

add_executable(theseed_aoi_event_test AoIEventTest.cpp)

target_link_libraries(
    theseed_aoi_event_test
    PRIVATE theseed_project_options
    PRIVATE theseed_runtime
    PRIVATE theseed_core)

add_test(NAME theseed_aoi_event_test COMMAND theseed_aoi_event_test)

add_executable(theseed_property_callback_test PropertyCallbackTest.cpp)

target_link_libraries(
    theseed_property_callback_test
    PRIVATE theseed_project_options
    PRIVATE theseed_runtime)

add_test(NAME theseed_property_callback_test COMMAND theseed_property_callback_test)

add_executable(theseed_invocation_codec_test InvocationCodecTest.cpp)

target_link_libraries(
    theseed_invocation_codec_test
    PRIVATE theseed_project_options
    PRIVATE theseed_runtime
    PRIVATE theseed_core)

add_test(NAME theseed_invocation_codec_test COMMAND theseed_invocation_codec_test)

add_executable(theseed_entity_tag_test EntityTagTest.cpp)

target_link_libraries(
    theseed_entity_tag_test
    PRIVATE theseed_project_options
    PRIVATE theseed_runtime
    PRIVATE theseed_core)

add_test(NAME theseed_entity_tag_test COMMAND theseed_entity_tag_test)

add_executable(theseed_entity_event_test EntityEventTest.cpp)

target_link_libraries(
    theseed_entity_event_test
    PRIVATE theseed_project_options
    PRIVATE theseed_runtime
    PRIVATE theseed_core)

add_test(NAME theseed_entity_event_test COMMAND theseed_entity_event_test)

add_executable(theseed_network_transport_test NetworkTransportTest.cpp)

target_link_libraries(
    theseed_network_transport_test
    PRIVATE theseed_project_options
    PRIVATE theseed_runtime
    PRIVATE theseed_core)

add_test(NAME theseed_network_transport_test COMMAND theseed_network_transport_test)

add_executable(theseed_tcp_transport_test TcpTransportTest.cpp)

target_link_libraries(
    theseed_tcp_transport_test
    PRIVATE theseed_project_options
    PRIVATE theseed_runtime
    PRIVATE theseed_core)

add_test(NAME theseed_tcp_transport_test COMMAND theseed_tcp_transport_test)

add_executable(theseed_transport_hub_test TransportHubTest.cpp)

target_link_libraries(
    theseed_transport_hub_test
    PRIVATE theseed_project_options
    PRIVATE theseed_runtime
    PRIVATE theseed_core)

add_test(NAME theseed_transport_hub_test COMMAND theseed_transport_hub_test)

add_executable(theseed_network_node_test NetworkNodeTest.cpp)

target_link_libraries(
    theseed_network_node_test
    PRIVATE theseed_project_options
    PRIVATE theseed_runtime
    PRIVATE theseed_core)

add_test(NAME theseed_network_node_test COMMAND theseed_network_node_test)
