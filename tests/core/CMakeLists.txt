add_executable(theseed_memory_stream_test MemoryStreamTest.cpp)

target_link_libraries(
    theseed_memory_stream_test
    PRIVATE theseed_project_options
    PRIVATE theseed_core)

add_test(NAME theseed_memory_stream_test COMMAND theseed_memory_stream_test)

add_executable(theseed_timer_wheel_test TimerWheelTest.cpp)

target_link_libraries(
    theseed_timer_wheel_test
    PRIVATE theseed_project_options
    PRIVATE theseed_core)

add_test(NAME theseed_timer_wheel_test COMMAND theseed_timer_wheel_test)

add_executable(theseed_object_pool_test ObjectPoolTest.cpp)

target_link_libraries(
    theseed_object_pool_test
    PRIVATE theseed_project_options
    PRIVATE theseed_core)

add_test(NAME theseed_object_pool_test COMMAND theseed_object_pool_test)
