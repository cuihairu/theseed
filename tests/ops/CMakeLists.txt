add_executable(theseed_ops_inspector_test OpsInspectorTest.cpp)

target_link_libraries(
    theseed_ops_inspector_test
    PRIVATE theseed_project_options theseed_test_options
    PRIVATE theseed_ops)

add_test(NAME theseed_ops_inspector_test COMMAND theseed_ops_inspector_test)

# OpsServer 依赖 TCP 服务器，macOS 上通信异常（CI macos job 证实），门控至 Linux。
if(UNIX AND NOT APPLE)
add_executable(theseed_ops_server_test OpsServerTest.cpp)

target_link_libraries(
    theseed_ops_server_test
    PRIVATE theseed_project_options theseed_test_options
    PRIVATE theseed_ops
    PRIVATE theseed_runtime)

add_test(NAME theseed_ops_server_test COMMAND theseed_ops_server_test)
endif()
