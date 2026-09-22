add_executable(theseed_scripting_hotupdate_test HotUpdateTest.cpp)

target_link_libraries(
    theseed_scripting_hotupdate_test
    PRIVATE theseed_project_options
    PRIVATE theseed_scripting)

# Windows Defender 在某些构建产物上会触发临时锁定（permission denied）。
# 经 cmake -P 把 exe 复制到 %TEMP% 后执行，避开锁定；$<TARGET_FILE:...>
# 由 ctest 解析为真实产物路径，天然兼容多配置生成器的 per-config 子目录
# （旧 batch 写死 bin\ 根目录，在 Visual Studio 生成器下找不到 exe）。
if(WIN32)
    add_test(NAME theseed_scripting_hotupdate_test
        COMMAND "${CMAKE_COMMAND}"
            -DEXE=$<TARGET_FILE:theseed_scripting_hotupdate_test>
            -P "${CMAKE_CURRENT_SOURCE_DIR}/CopyAndRun.cmake")
else()
    add_test(NAME theseed_scripting_hotupdate_test
        COMMAND theseed_scripting_hotupdate_test)
endif()
