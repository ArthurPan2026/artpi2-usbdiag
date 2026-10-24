cmake_minimum_required(VERSION 3.22)
project(watchdog_record_host C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_executable(watchdog_record_host
    ../../packages/watchdog_crashlog/tests/test_record.c
    ../../packages/watchdog_crashlog/src/watchdog_record.c)
target_include_directories(watchdog_record_host PRIVATE
    ../../packages/watchdog_crashlog/include)
enable_testing()
add_test(NAME watchdog_record_host COMMAND watchdog_record_host)

add_executable(watchdog_trace_host
    ../../packages/watchdog_crashlog/tests/test_trace.c
    ../../packages/watchdog_crashlog/src/watchdog_trace.c)
target_include_directories(watchdog_trace_host PRIVATE
    ../../packages/watchdog_crashlog/include)
add_test(NAME watchdog_trace_host COMMAND watchdog_trace_host)

add_executable(watchdog_h7r7_reset_gate_host test_h7r7_reset_gate.c)
target_include_directories(watchdog_h7r7_reset_gate_host PRIVATE
    ../../Drivers/ART-Pi2
    ../../packages/watchdog_crashlog/include)
add_test(NAME watchdog_h7r7_reset_gate_host COMMAND watchdog_h7r7_reset_gate_host)

add_executable(watchdog_flash_policy_host test_flash_policy.c)
target_include_directories(watchdog_flash_policy_host PRIVATE
    ../../packages/watchdog_crashlog/include)
add_test(NAME watchdog_flash_policy_host COMMAND watchdog_flash_policy_host)
