add_library(
    theseed_login
    LoginApp.cpp
    ClientSession.cpp
    ClientProtocol.cpp
    LoginProtocol.cpp
    SessionToken.cpp)

target_link_libraries(
    theseed_login
    PUBLIC theseed_project_options
    PUBLIC theseed_runtime
    PUBLIC theseed_foundation)

target_include_directories(
    theseed_login
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
