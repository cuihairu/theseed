cmake_minimum_required(VERSION 3.28)

project(
    theseed
    VERSION 0.1.0
    DESCRIPTION "theseed server engine"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(THESEED_BUILD_TESTS "Build theseed tests" ON)

add_library(theseed_project_options INTERFACE)
target_compile_features(theseed_project_options INTERFACE cxx_std_23)

if(MSVC)
    target_compile_options(theseed_project_options INTERFACE /W4 /permissive-)
else()
    target_compile_options(
        theseed_project_options
        INTERFACE
        -Wall
        -Wextra
        -Wpedantic)
endif()

add_subdirectory(src)
add_subdirectory(apps)

if(THESEED_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
