add_executable(theseed_bots_config_test BotsConfigTest.cpp)

target_include_directories(theseed_bots_config_test
    PRIVATE ${CMAKE_SOURCE_DIR}/tools/bots)

target_link_libraries(
    theseed_bots_config_test
    PRIVATE theseed_project_options theseed_test_options
    PRIVATE theseed_bots)

add_test(NAME theseed_bots_config_test COMMAND theseed_bots_config_test)

# bots E2E：真实 TCP 回环全栈（LoginApp + BaseApp/CellApp + BotManager），
# macOS 上链路无法建立（与 LoginApp/OpsServer E2E 同款门控），门控至 Linux。
if(UNIX AND NOT APPLE)
    add_executable(theseed_bots_e2e_test BotsE2ETest.cpp)

    target_include_directories(theseed_bots_e2e_test
        PRIVATE ${CMAKE_SOURCE_DIR}/tools/bots)

    target_link_libraries(
        theseed_bots_e2e_test
        PRIVATE theseed_project_options theseed_test_options
        PRIVATE theseed_bots
        PRIVATE theseed_core
        PRIVATE theseed_login
        PRIVATE theseed_runtime)

    add_test(NAME theseed_bots_e2e_test COMMAND theseed_bots_e2e_test)
endif()
