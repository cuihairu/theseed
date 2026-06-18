add_executable(theseed_scripting_hotupdate_test HotUpdateTest.cpp)

set_target_properties(theseed_scripting_hotupdate_test
    PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

target_link_libraries(
    theseed_scripting_hotupdate_test
    PRIVATE theseed_project_options
    PRIVATE theseed_scripting)

# Windows Defender 在某些构建产物上会触发临时锁定（permission denied）。
# 通过 wrapper batch 把 exe 复制到 %TEMP% 后执行，避开锁定。
if(WIN32)
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/run_hotupdate.bat"
        CONTENT
            "@echo off\r\n copy /Y \"%~dp0..\\..\\bin\\theseed_scripting_hotupdate_test.exe\" \"%TEMP%\\hu_test.exe\" >nul\r\n \"%TEMP%\\hu_test.exe\" %*\r\n")
    add_test(NAME theseed_scripting_hotupdate_test
        COMMAND "${CMAKE_CURRENT_BINARY_DIR}/run_hotupdate.bat")
else()
    add_test(NAME theseed_scripting_hotupdate_test
        COMMAND theseed_scripting_hotupdate_test)
endif()
