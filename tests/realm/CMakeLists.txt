add_executable(theseed_realm_app_test RealmAppTest.cpp)

target_link_libraries(
    theseed_realm_app_test
    PRIVATE theseed_project_options
    PRIVATE theseed_realm
    PRIVATE theseed_runtime)

add_test(NAME RealmAppTest COMMAND theseed_realm_app_test)

# RealmApp 端到端测试：真实 TCP 回环驱动完整 tick 循环。
add_executable(theseed_realm_app_e2e_test RealmAppE2ETest.cpp)

target_link_libraries(
    theseed_realm_app_e2e_test
    PRIVATE theseed_project_options
    PRIVATE theseed_realm)

add_test(NAME RealmAppE2ETest COMMAND theseed_realm_app_e2e_test)
