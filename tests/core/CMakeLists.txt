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
