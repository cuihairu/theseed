add_executable(theseed_db_app_test DBAppTest.cpp)

target_link_libraries(
    theseed_db_app_test
    PRIVATE theseed_project_options
    PRIVATE theseed_db)

add_test(NAME DBAppTest COMMAND theseed_db_app_test)
