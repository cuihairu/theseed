add_executable(theseed_baseapp main.cpp)

target_link_libraries(
    theseed_baseapp
    PRIVATE theseed_project_options
    PRIVATE theseed_core)
