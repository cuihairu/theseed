add_executable(theseed_loginapp main.cpp)

target_link_libraries(
    theseed_loginapp
    PRIVATE theseed_project_options
    PRIVATE theseed_login)
