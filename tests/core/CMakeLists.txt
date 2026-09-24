add_executable(theseed_entity_data_test EntityDataTest.cpp)

target_link_libraries(
    theseed_entity_data_test
    PRIVATE theseed_project_options
    PRIVATE theseed_core)

add_test(NAME theseed_entity_data_test COMMAND theseed_entity_data_test)

add_executable(theseed_entity_def_loader_test EntityDefLoaderTest.cpp)

target_link_libraries(
    theseed_entity_def_loader_test
    PRIVATE theseed_project_options
    PRIVATE theseed_core)

add_test(NAME theseed_entity_def_loader_test COMMAND theseed_entity_def_loader_test)

add_executable(theseed_base_runtime_test BaseRuntimeTest.cpp)

target_link_libraries(
    theseed_base_runtime_test
    PRIVATE theseed_project_options
    PRIVATE theseed_core)

add_test(NAME theseed_base_runtime_test COMMAND theseed_base_runtime_test)

add_executable(theseed_entity_def_registry_test EntityDefRegistryTest.cpp)

target_link_libraries(
    theseed_entity_def_registry_test
    PRIVATE theseed_project_options
    PRIVATE theseed_core)

add_test(NAME theseed_entity_def_registry_test COMMAND theseed_entity_def_registry_test)

add_executable(theseed_base_app_test BaseAppTest.cpp)

target_link_libraries(
    theseed_base_app_test
    PRIVATE theseed_project_options
    PRIVATE theseed_core)

add_test(NAME theseed_base_app_test COMMAND theseed_base_app_test)

add_executable(theseed_cell_app_test CellAppTest.cpp)

target_link_libraries(
    theseed_cell_app_test
    PRIVATE theseed_project_options
    PRIVATE theseed_core)

add_test(NAME theseed_cell_app_test COMMAND theseed_cell_app_test)

# Phase B：CellApp 可观测性整合测试（entity_count / transport stats）。
add_executable(theseed_cell_app_observability_test CellAppObservabilityTest.cpp)

target_link_libraries(
    theseed_cell_app_observability_test
    PRIVATE theseed_project_options
    PRIVATE theseed_core)

add_test(NAME theseed_cell_app_observability_test COMMAND theseed_cell_app_observability_test)

add_executable(theseed_integration_test IntegrationTest.cpp)

target_link_libraries(
    theseed_integration_test
    PRIVATE theseed_project_options
    PRIVATE theseed_core)

add_test(NAME theseed_integration_test COMMAND theseed_integration_test)

add_executable(theseed_entity_timer_test EntityTimerTest.cpp)

target_link_libraries(
    theseed_entity_timer_test
    PRIVATE theseed_project_options
    PRIVATE theseed_core)

add_test(NAME theseed_entity_timer_test COMMAND theseed_entity_timer_test)

add_executable(theseed_property_metadata_test PropertyMetadataTest.cpp)

target_link_libraries(
    theseed_property_metadata_test
    PRIVATE theseed_project_options
    PRIVATE theseed_core)

add_test(NAME theseed_property_metadata_test COMMAND theseed_property_metadata_test)

add_executable(theseed_tcp_integration_test TcpIntegrationTest.cpp)

target_link_libraries(
    theseed_tcp_integration_test
    PRIVATE theseed_project_options
    PRIVATE theseed_core)

add_test(NAME theseed_tcp_integration_test COMMAND theseed_tcp_integration_test)

add_executable(theseed_multi_cellapp_tcp_test MultiCellAppTcpTest.cpp)

target_link_libraries(
    theseed_multi_cellapp_tcp_test
    PRIVATE theseed_project_options
    PRIVATE theseed_core)

add_test(NAME theseed_multi_cellapp_tcp_test COMMAND theseed_multi_cellapp_tcp_test)

add_executable(theseed_tick_driven_tcp_test TickDrivenTcpTest.cpp)

target_link_libraries(
    theseed_tick_driven_tcp_test
    PRIVATE theseed_project_options
    PRIVATE theseed_core)

add_test(NAME theseed_tick_driven_tcp_test COMMAND theseed_tick_driven_tcp_test)

add_executable(theseed_remote_entity_test RemoteEntityTest.cpp)

target_link_libraries(
    theseed_remote_entity_test
    PRIVATE theseed_project_options
    PRIVATE theseed_core)

add_test(NAME theseed_remote_entity_test COMMAND theseed_remote_entity_test)


add_executable(theseed_tick_node_lifecycle_test TickNodeLifecycleTest.cpp)

target_link_libraries(
    theseed_tick_node_lifecycle_test
    PRIVATE theseed_project_options
    PRIVATE theseed_core)

add_test(NAME theseed_tick_node_lifecycle_test COMMAND theseed_tick_node_lifecycle_test)

add_executable(theseed_property_sync_test PropertySyncTest.cpp)

target_link_libraries(
    theseed_property_sync_test
    PRIVATE theseed_project_options
    PRIVATE theseed_core)

add_test(NAME theseed_property_sync_test COMMAND theseed_property_sync_test)

add_executable(theseed_file_entity_store_test FileEntityStoreTest.cpp)

target_link_libraries(
    theseed_file_entity_store_test
    PRIVATE theseed_project_options
    PRIVATE theseed_core)

add_test(NAME theseed_file_entity_store_test COMMAND theseed_file_entity_store_test)

add_executable(theseed_property_sync_direction_test PropertySyncDirectionTest.cpp)

target_link_libraries(
    theseed_property_sync_direction_test
    PRIVATE theseed_project_options
    PRIVATE theseed_core)

add_test(NAME theseed_property_sync_direction_test COMMAND theseed_property_sync_direction_test)

add_executable(theseed_entity_restore_test EntityRestoreTest.cpp)

target_link_libraries(
    theseed_entity_restore_test
    PRIVATE theseed_project_options
    PRIVATE theseed_core)

add_test(NAME theseed_entity_restore_test COMMAND theseed_entity_restore_test)

add_executable(theseed_client_prop_sync_test ClientPropertySyncTest.cpp)

target_link_libraries(
    theseed_client_prop_sync_test
    PRIVATE theseed_project_options
    PRIVATE theseed_core)

add_test(NAME theseed_client_prop_sync_test COMMAND theseed_client_prop_sync_test)

add_executable(theseed_aoi_client_test AoIClientTest.cpp)

target_link_libraries(
    theseed_aoi_client_test
    PRIVATE theseed_project_options
    PRIVATE theseed_core)

add_test(NAME theseed_aoi_client_test COMMAND theseed_aoi_client_test)

add_executable(theseed_client_action_test ClientActionTest.cpp)

target_link_libraries(
    theseed_client_action_test
    PRIVATE theseed_project_options
    PRIVATE theseed_core)

add_test(NAME theseed_client_action_test COMMAND theseed_client_action_test)

add_executable(theseed_client_disconnect_test ClientDisconnectTest.cpp)

target_link_libraries(
    theseed_client_disconnect_test
    PRIVATE theseed_project_options
    PRIVATE theseed_core)

add_test(NAME theseed_client_disconnect_test COMMAND theseed_client_disconnect_test)

add_executable(theseed_cell_spawn_test CellSpawnTest.cpp)

target_link_libraries(
    theseed_cell_spawn_test
    PRIVATE theseed_project_options
    PRIVATE theseed_core)

add_test(NAME theseed_cell_spawn_test COMMAND theseed_cell_spawn_test)

add_executable(theseed_multi_space_test MultiSpaceTest.cpp)

target_link_libraries(
    theseed_multi_space_test
    PRIVATE theseed_project_options
    PRIVATE theseed_core)

add_test(NAME theseed_multi_space_test COMMAND theseed_multi_space_test)

add_executable(theseed_client_event_test ClientEventTest.cpp)

target_link_libraries(
    theseed_client_event_test
    PRIVATE theseed_project_options
    PRIVATE theseed_core)

add_test(NAME theseed_client_event_test COMMAND theseed_client_event_test)

add_executable(theseed_migration_test MigrationTest.cpp)

target_link_libraries(
    theseed_migration_test
    PRIVATE theseed_project_options
    PRIVATE theseed_core)

add_test(NAME theseed_migration_test COMMAND theseed_migration_test)

add_executable(theseed_witness_sync_test WitnessSyncTest.cpp)

target_link_libraries(
    theseed_witness_sync_test
    PRIVATE theseed_project_options
    PRIVATE theseed_core)

add_test(NAME theseed_witness_sync_test COMMAND theseed_witness_sync_test)

add_executable(theseed_space_teleport_test SpaceTeleportTest.cpp)

target_link_libraries(
    theseed_space_teleport_test
    PRIVATE theseed_project_options
    PRIVATE theseed_core)

add_test(NAME theseed_space_teleport_test COMMAND theseed_space_teleport_test)

add_executable(theseed_position_sync_test PositionSyncTest.cpp)

target_link_libraries(
    theseed_position_sync_test
    PRIVATE theseed_project_options
    PRIVATE theseed_core)

add_test(NAME theseed_position_sync_test COMMAND theseed_position_sync_test)

add_executable(theseed_destruction_test DestructionTest.cpp)

target_link_libraries(
    theseed_destruction_test
    PRIVATE theseed_project_options
    PRIVATE theseed_core)

add_test(NAME theseed_destruction_test COMMAND theseed_destruction_test)

add_executable(theseed_property_flags_test PropertyFlagsTest.cpp)

target_link_libraries(
    theseed_property_flags_test
    PRIVATE theseed_project_options
    PRIVATE theseed_core)

add_test(NAME theseed_property_flags_test COMMAND theseed_property_flags_test)

add_executable(theseed_method_dispatch_test MethodDispatchTest.cpp)

target_link_libraries(
    theseed_method_dispatch_test
    PRIVATE theseed_project_options
    PRIVATE theseed_core)

add_test(NAME theseed_method_dispatch_test COMMAND theseed_method_dispatch_test)

add_executable(theseed_client_method_test ClientMethodTest.cpp)

target_link_libraries(
    theseed_client_method_test
    PRIVATE theseed_project_options
    PRIVATE theseed_core)

add_test(NAME theseed_client_method_test COMMAND theseed_client_method_test)

add_executable(theseed_entity_query_test EntityQueryTest.cpp)

target_link_libraries(
    theseed_entity_query_test
    PRIVATE theseed_project_options
    PRIVATE theseed_core)

add_test(NAME theseed_entity_query_test COMMAND theseed_entity_query_test)

add_executable(theseed_entity_query_store_test EntityQueryStoreTest.cpp)

target_link_libraries(
    theseed_entity_query_store_test
    PRIVATE theseed_project_options
    PRIVATE theseed_core)

add_test(NAME theseed_entity_query_store_test COMMAND theseed_entity_query_store_test)

# EntityDefLoader 手写 XML 解析器的畸形输入矩阵（分支覆盖第二批）。
add_executable(theseed_entity_def_parser_branch_test EntityDefParserBranchTest.cpp)

target_link_libraries(
    theseed_entity_def_parser_branch_test
    PRIVATE theseed_project_options
    PRIVATE theseed_core)

add_test(NAME theseed_entity_def_parser_branch_test COMMAND theseed_entity_def_parser_branch_test)

# BaseRuntime 分支覆盖（第二批）：payload 边界矩阵 / 回调缺席 / send 失败 / sync 时序。
add_executable(theseed_base_runtime_branch_test BaseRuntimeBranchTest.cpp)

target_link_libraries(
    theseed_base_runtime_branch_test
    PRIVATE theseed_project_options
    PRIVATE theseed_core)

add_test(NAME theseed_base_runtime_branch_test COMMAND theseed_base_runtime_branch_test)
