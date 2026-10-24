cmake_minimum_required(VERSION 3.22)
project(watchdog_crashlog C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_library(watchdog_crashlog
    src/watchdog_record.c
    src/watchdog_trace.c)
target_include_directories(watchdog_crashlog PUBLIC include)

option(WATCHDOG_CRASHLOG_BUILD_TESTS "Build the host record-core tests" ON)
if(WATCHDOG_CRASHLOG_BUILD_TESTS)
    enable_testing()
    add_executable(watchdog_crashlog_record_test tests/test_record.c)
    target_link_libraries(watchdog_crashlog_record_test
        PRIVATE watchdog_crashlog)
    add_test(NAME watchdog_crashlog_record_core
        COMMAND watchdog_crashlog_record_test)
    add_executable(watchdog_crashlog_trace_test tests/test_trace.c)
    target_link_libraries(watchdog_crashlog_trace_test
        PRIVATE watchdog_crashlog)
    add_test(NAME watchdog_crashlog_trace_core
        COMMAND watchdog_crashlog_trace_test)
endif()

option(WATCHDOG_CRASHLOG_BUILD_RTTHREAD_ADAPTER
    "Build the optional RT-Thread supervisor adapter" OFF)
set(WATCHDOG_CRASHLOG_RTTHREAD_INCLUDE_DIR "" CACHE PATH
    "Directory containing rtthread.h")
if(WATCHDOG_CRASHLOG_BUILD_RTTHREAD_ADAPTER)
    if(NOT EXISTS "${WATCHDOG_CRASHLOG_RTTHREAD_INCLUDE_DIR}/rtthread.h")
        message(FATAL_ERROR
            "Set WATCHDOG_CRASHLOG_RTTHREAD_INCLUDE_DIR to the RT-Thread include directory")
    endif()
    add_library(watchdog_crashlog_rtt src/watchdog_rtt_supervisor.c)
    target_include_directories(watchdog_crashlog_rtt PUBLIC include
        "${WATCHDOG_CRASHLOG_RTTHREAD_INCLUDE_DIR}")
    target_link_libraries(watchdog_crashlog_rtt PUBLIC watchdog_crashlog)
endif()

if(MSVC)
    target_compile_options(watchdog_crashlog PRIVATE /W4)
    if(WATCHDOG_CRASHLOG_BUILD_TESTS)
        target_compile_options(watchdog_crashlog_record_test PRIVATE /W4)
        target_compile_options(watchdog_crashlog_trace_test PRIVATE /W4)
    endif()
else()
    target_compile_options(watchdog_crashlog PRIVATE
        -Wall -Wextra -Werror -pedantic)
    if(WATCHDOG_CRASHLOG_BUILD_TESTS)
        target_compile_options(watchdog_crashlog_record_test PRIVATE
            -Wall -Wextra -Werror -pedantic)
        target_compile_options(watchdog_crashlog_trace_test PRIVATE
            -Wall -Wextra -Werror -pedantic)
    endif()
endif()
