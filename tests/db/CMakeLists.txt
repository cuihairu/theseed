add_executable(theseed_db_app_test DBAppTest.cpp)

target_link_libraries(
    theseed_db_app_test
    PRIVATE theseed_project_options
    PRIVATE theseed_db)

add_test(NAME DBAppTest COMMAND theseed_db_app_test)

# MySQL 集成测试：需要运行中的 MySQL 实例，未配置 THESEED_MYSQL_HOST 时测试自身跳过。
# 仅在 theseed_db 启用了 MySQL 后端（THESEED_MYSQL_ENABLED 全局属性为 TRUE）时构建。
get_property(THESEED_MYSQL_ENABLED GLOBAL PROPERTY theseed_mysql_enabled)
if(THESEED_MYSQL_ENABLED)
    add_executable(theseed_mysql_store_test MySqlEntityStoreTest.cpp)

    target_link_libraries(
        theseed_mysql_store_test
        PRIVATE theseed_project_options
        PRIVATE theseed_db)

    add_test(NAME MySQLEntityStoreTest COMMAND theseed_mysql_store_test)
endif()
