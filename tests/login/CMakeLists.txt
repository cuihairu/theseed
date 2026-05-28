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
