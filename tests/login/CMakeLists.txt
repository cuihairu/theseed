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
