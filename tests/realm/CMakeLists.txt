add_executable(theseed_realm_app_test RealmAppTest.cpp)

target_link_libraries(
    theseed_realm_app_test
    PRIVATE theseed_project_options
    PRIVATE theseed_realm
    PRIVATE theseed_runtime)

add_test(NAME RealmAppTest COMMAND theseed_realm_app_test)
