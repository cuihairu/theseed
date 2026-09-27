add_executable(theseed_login_protocol_test LoginProtocolTest.cpp)

target_link_libraries(
    theseed_login_protocol_test
    PRIVATE theseed_project_options theseed_test_options
    PRIVATE theseed_login)

add_test(NAME theseed_login_protocol_test COMMAND theseed_login_protocol_test)

add_executable(theseed_login_app_test LoginAppTest.cpp)

target_link_libraries(
    theseed_login_app_test
    PRIVATE theseed_project_options theseed_test_options
    PRIVATE theseed_login
    # 通知腿单元测试直接引用 MachineMethod 常量构造分发面用例
    PRIVATE theseed_control)

add_test(NAME theseed_login_app_test COMMAND theseed_login_app_test)

# LoginApp 端到端测试：真实 TCP 回环驱动完整 tick 循环，依赖
# MachineDaemon/ops 服务器通信，macOS 上链路无法建立，门控至 Linux。
if(UNIX AND NOT APPLE)
    add_executable(theseed_login_app_e2e_test LoginAppE2ETest.cpp)

    target_link_libraries(
        theseed_login_app_e2e_test
        PRIVATE theseed_project_options theseed_test_options
        PRIVATE theseed_login
        PRIVATE theseed_db
        # 场景 F 起真实 MachineDaemon（踢人联动端到端）
        PRIVATE theseed_control)

    add_test(NAME theseed_login_app_e2e_test COMMAND theseed_login_app_e2e_test)
endif()

add_executable(theseed_client_protocol_test ClientProtocolTest.cpp)

target_link_libraries(
    theseed_client_protocol_test
    PRIVATE theseed_project_options theseed_test_options
    PRIVATE theseed_login)

add_test(NAME theseed_client_protocol_test COMMAND theseed_client_protocol_test)

# Phase B：LoginApp ↔ Redis（SessionStore/RateLimiter）集成测试。
# 纯内存实现（InMemoryRedisProvider），无需真实 Redis 服务。
add_executable(theseed_login_redis_integration_test LoginRedisIntegrationTest.cpp)

target_link_libraries(
    theseed_login_redis_integration_test
    PRIVATE theseed_project_options theseed_test_options
    PRIVATE theseed_login)

add_test(NAME theseed_login_redis_integration_test COMMAND theseed_login_redis_integration_test)
