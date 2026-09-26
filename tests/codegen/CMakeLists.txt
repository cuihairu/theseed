add_executable(theseed_csharp_emitter_test CSharpEmitterTest.cpp)

target_link_libraries(
    theseed_csharp_emitter_test
    PRIVATE theseed_project_options theseed_test_options
    PRIVATE theseed_codegen)

add_test(NAME theseed_csharp_emitter_test COMMAND theseed_csharp_emitter_test)
