add_executable(theseed_cellapp main.cpp)

target_link_libraries(
    theseed_cellapp
    PRIVATE theseed_project_options
    PRIVATE theseed_core)
