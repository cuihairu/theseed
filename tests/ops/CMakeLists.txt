add_executable(theseed_ops_inspector_test OpsInspectorTest.cpp)

target_link_libraries(
    theseed_ops_inspector_test
    PRIVATE theseed_project_options
    PRIVATE theseed_ops)

add_test(NAME theseed_ops_inspector_test COMMAND theseed_ops_inspector_test)
