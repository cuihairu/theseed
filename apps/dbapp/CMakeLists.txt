add_executable(theseed_dbapp main.cpp)

target_link_libraries(
    theseed_dbapp
    PRIVATE theseed_project_options
    PRIVATE theseed_db)
