set(USER_MODULE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/fault_diagnostics.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/rtthread_allocator_bench.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui_sample_profile.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/usb_perf.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/work_assert_capture.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/watchdog_supervisor.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../packages/watchdog_crashlog/src/watchdog_record.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../packages/watchdog_crashlog/src/watchdog_trace.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../packages/watchdog_crashlog/src/watchdog_rtt_supervisor.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../Drivers/ART-Pi2/watchdog_h7r7_port.c"
)

# The ISR reproducer is intentionally part of the performance Debug profile only.
if(RTTHREAD_PROFILE STREQUAL "performance")
    list(APPEND USER_MODULE_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/src/lvgl_isr_repro.c")
endif()

set(USER_MODULE_SOURCES "${USER_MODULE_SOURCES}" PARENT_SCOPE)
set(USER_MODULE_INCLUDE_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../packages/watchdog_crashlog/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../Drivers/ART-Pi2"
    PARENT_SCOPE
)
