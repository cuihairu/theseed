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
