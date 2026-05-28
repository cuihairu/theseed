add_executable(theseed_realmapp main.cpp)

target_link_libraries(
    theseed_realmapp
    PRIVATE theseed_project_options
    PRIVATE theseed_realm)
