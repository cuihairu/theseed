add_executable(theseed_app_main_util_test AppMainUtilTest.cpp)
target_include_directories(
    theseed_app_main_util_test
    PRIVATE ${CMAKE_SOURCE_DIR}/apps)
target_link_libraries(
    theseed_app_main_util_test
    PRIVATE theseed_project_options theseed_test_options)
add_test(NAME theseed_app_main_util_test COMMAND theseed_app_main_util_test)
