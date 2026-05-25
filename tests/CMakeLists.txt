add_executable(theseed_smoke_test smoke/SmokeTest.cpp)

target_link_libraries(
    theseed_smoke_test
    PRIVATE theseed_project_options
    PRIVATE theseed_control)

add_test(NAME theseed_smoke_test COMMAND theseed_smoke_test)

add_subdirectory(core)
add_subdirectory(runtime)
