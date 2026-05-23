add_executable(theseed_machine main.cpp)

target_link_libraries(
    theseed_machine
    PRIVATE theseed_project_options
    PRIVATE theseed_control)
