# 基础源文件：不含 MySQL 依赖，始终编译。
set(THESEED_DB_SOURCES
    DBApp.cpp
    DBProtocol.cpp
    RemoteEntityStore.cpp)

add_library(theseed_db ${THESEED_DB_SOURCES})

# MySQL 后端是可选的：libmysql 不可用时回退到 FileEntityStore。
# 通过 THESEED_HAS_MYSQL 宏在编译期裁剪 MySQL 相关代码。
find_package(unofficial-libmysql CONFIG QUIET)
if(unofficial-libmysql_FOUND)
    target_sources(theseed_db PRIVATE
        MySQLConnection.cpp
        MySQLEntityStore.cpp)
    target_compile_definitions(theseed_db PUBLIC THESEED_HAS_MYSQL=1)
    target_link_libraries(theseed_db PRIVATE unofficial::libmysql::libmysql)
    set_property(GLOBAL PROPERTY theseed_mysql_enabled TRUE)
    message(STATUS "theseed_db: MySQL backend enabled (libmysql found)")
else()
    message(STATUS "theseed_db: MySQL backend disabled (libmysql not found, file-only)")
endif()

target_link_libraries(
    theseed_db
    PUBLIC theseed_project_options
    PUBLIC theseed_core
    PUBLIC theseed_runtime
    PUBLIC theseed_foundation)

target_include_directories(
    theseed_db
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
