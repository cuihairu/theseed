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
    PUBLIC theseed_core
    PUBLIC theseed_runtime
    PUBLIC theseed_db
    PUBLIC theseed_foundation
    # 控制面通知腿（machine.session.revoked 分发）共用 control 侧
    # MachineMethod 协议常量——同一常量杜绝方法名漂移。 PRIVATE：LoginApp
    # 的公开头不暴露 control 类型，不向消费者传递。
    PRIVATE theseed_control)

target_include_directories(
    theseed_login
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
