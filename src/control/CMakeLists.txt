add_library(
    theseed_control
    machine/HostProbe.cpp
    machine/MachineAgent.cpp
    machine/MachineSnapshotCodec.cpp
    machine/ProcessSupervisor.cpp)

target_link_libraries(
    theseed_control
    PUBLIC theseed_project_options
    PUBLIC theseed_core)

target_include_directories(
    theseed_control
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
