# 把测试可执行文件复制到 %TEMP% 后运行：Windows Defender 会短暂锁定
# 新写入的构建产物（permission denied），在临时目录执行副本避开锁定。
# 用法：cmake -DEXE=<path-to-exe> -P CopyAndRun.cmake
if(NOT DEFINED EXE OR EXE STREQUAL "")
    message(FATAL_ERROR "usage: cmake -DEXE=<path-to-exe> -P CopyAndRun.cmake")
endif()

file(TO_CMAKE_PATH "$ENV{TEMP}" tempDir)
get_filename_component(exeName "${EXE}" NAME)

if(tempDir STREQUAL "")
    # 无 TEMP 环境变量时退化为原地执行
    execute_process(COMMAND "${EXE}" RESULT_VARIABLE runResult)
    if(NOT runResult EQUAL 0)
        message(FATAL_ERROR "${exeName} exited with ${runResult}")
    endif()
    return()
endif()

set(copy "${tempDir}/${exeName}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${EXE}" "${copy}"
    RESULT_VARIABLE copyResult)
if(NOT copyResult EQUAL 0)
    message(FATAL_ERROR "copy ${EXE} -> ${copy} failed: ${copyResult}")
endif()

execute_process(COMMAND "${copy}" RESULT_VARIABLE runResult)
if(NOT runResult EQUAL 0)
    message(FATAL_ERROR "${exeName} exited with ${runResult}")
endif()
