add_executable(theseed_login_protocol_test LoginProtocolTest.cpp)

target_link_libraries(
    theseed_login_protocol_test
    PRIVATE theseed_project_options
    PRIVATE theseed_login)

add_test(NAME theseed_login_protocol_test COMMAND theseed_login_protocol_test)

add_executable(theseed_login_app_test LoginAppTest.cpp)

target_link_libraries(
    theseed_login_app_test
    PRIVATE theseed_project_options
    PRIVATE theseed_login)

add_test(NAME theseed_login_app_test COMMAND theseed_login_app_test)

# LoginApp 端到端测试：真实 TCP 回环（null/password/db 三种鉴权路径），
# db 场景由测试内后台线程驱动 file 后端 DBApp。
add_executable(theseed_login_app_e2e_test LoginAppE2ETest.cpp)

target_link_libraries(
    theseed_login_app_e2e_test
    PRIVATE theseed_project_options
    PRIVATE theseed_login
    PRIVATE theseed_db)

add_test(NAME theseed_login_app_e2e_test COMMAND theseed_login_app_e2e_test)

add_executable(theseed_client_protocol_test ClientProtocolTest.cpp)

target_link_libraries(
    theseed_client_protocol_test
    PRIVATE theseed_project_options
    PRIVATE theseed_login)

add_test(NAME theseed_client_protocol_test COMMAND theseed_client_protocol_test)

# Phase B：LoginApp ↔ Redis（SessionStore/RateLimiter）集成测试。
# 纯内存实现（InMemoryRedisProvider），无需真实 Redis 服务。
add_executable(theseed_login_redis_integration_test LoginRedisIntegrationTest.cpp)

target_link_libraries(
    theseed_login_redis_integration_test
    PRIVATE theseed_project_options
    PRIVATE theseed_login)

add_test(NAME theseed_login_redis_integration_test COMMAND theseed_login_redis_integration_test)
