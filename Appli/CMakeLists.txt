cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release")
endif()

# RT-Thread configuration profile.  CMake remains the build authority for
# this CubeMX project; the profile only adds explicitly requested RT-Thread
# features to the checked-in rtconfig.h surface.
set(RTTHREAD_PROFILE "compat" CACHE STRING
    "RT-Thread profile: compat, performance, or release")
set_property(CACHE RTTHREAD_PROFILE PROPERTY STRINGS compat performance release)

set(RTTHREAD_ALLOCATOR "memheap" CACHE STRING
    "RT-Thread heap allocator: smallmem, memheap, or slab")
set_property(CACHE RTTHREAD_ALLOCATOR PROPERTY STRINGS smallmem memheap slab)

set(LVGL_PROFILE "performance" CACHE STRING
    "LVGL profile: performance or release")
set_property(CACHE LVGL_PROFILE PROPERTY STRINGS performance release)

option(LVGL_DRAW_DMA2D
    "Register the LVGL 9.6 DMA2D draw unit through the H7RS broker"
    ON)

if(RTTHREAD_ALLOCATOR STREQUAL "smallmem")
    set(RTTHREAD_ALLOCATOR_VALUE 0)
elseif(RTTHREAD_ALLOCATOR STREQUAL "memheap")
    set(RTTHREAD_ALLOCATOR_VALUE 1)
elseif(RTTHREAD_ALLOCATOR STREQUAL "slab")
    set(RTTHREAD_ALLOCATOR_VALUE 2)
else()
    message(FATAL_ERROR
        "RTTHREAD_ALLOCATOR must be smallmem, memheap, or slab")
endif()

if(RTTHREAD_PROFILE STREQUAL "compat")
    set(RTTHREAD_PROFILE_VALUE 0)
    set(RTTHREAD_PROFILE_DEFINES)
elseif(RTTHREAD_PROFILE STREQUAL "performance")
    set(RTTHREAD_PROFILE_VALUE 1)
    set(RTTHREAD_PROFILE_DEFINES
        RT_USING_CPU_USAGE_TRACER
        RT_CPU_USAGE_CALC_INTERVAL_MS=200)
elseif(RTTHREAD_PROFILE STREQUAL "release")
    set(RTTHREAD_PROFILE_VALUE 2)
    set(RTTHREAD_PROFILE_DEFINES)
else()
    message(FATAL_ERROR
        "RTTHREAD_PROFILE must be compat, performance, or release")
endif()

if(RTTHREAD_PROFILE STREQUAL "performance")
    set(WORK_EVIDENCE_DIAGNOSTICS_DEFINE WORK_EVIDENCE_DIAGNOSTICS=1)
else()
    set(WORK_EVIDENCE_DIAGNOSTICS_DEFINE WORK_EVIDENCE_DIAGNOSTICS=0)
endif()

if(LVGL_PROFILE STREQUAL "performance")
    set(LVGL_PROFILE_DEFINES LVGL_PROFILE_PERFORMANCE=1)
elseif(LVGL_PROFILE STREQUAL "release")
    set(LVGL_PROFILE_DEFINES LVGL_PROFILE_RELEASE=1)
else()
    message(FATAL_ERROR "LVGL_PROFILE must be performance or release")
endif()

if(LVGL_DRAW_DMA2D)
    set(LVGL_DRAW_DMA2D_DEFINE LVGL_DRAW_DMA2D_ENABLED=1)
else()
    set(LVGL_DRAW_DMA2D_DEFINE LVGL_DRAW_DMA2D_ENABLED=0)
endif()

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/User/config/include/rtconfig.h" RTTHREAD_CONFIG_TEXT)
if(NOT RTTHREAD_CONFIG_TEXT MATCHES
    "#define[ \t]+RT_VER_NUM[ \t]+0x50300")
    message(FATAL_ERROR
        "Appli/User/config/include/rtconfig.h must declare RT-Thread 5.3.0 (RT_VER_NUM 0x50300)")
endif()

message("Build type: " ${CMAKE_BUILD_TYPE})
message("RT-Thread profile: " ${RTTHREAD_PROFILE})
message("RT-Thread allocator: " ${RTTHREAD_ALLOCATOR})
message("LVGL profile: " ${LVGL_PROFILE})
message("LVGL DMA2D draw unit: " ${LVGL_DRAW_DMA2D})

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Compiler options
set(STM32_MCU_FLAGS  "-mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard " )

#Linker options
set(STM32_LINKER_SCRIPT stm32h7rsxx_ROMxspi2.ld)
set(STM32_LINKER_OPTION  )

# Set the project name
set(CMAKE_PROJECT_NAME artpi2-usbdiag_Appli)
project(${CMAKE_PROJECT_NAME})

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Keep __FILE__ strings independent of the checkout or package staging path.
# HAL, USB, LVGL, and Web diagnostics can include these strings in .rodata;
# mapping them keeps release binaries free of local build paths and makes a
# package build stable after the staging tree is moved into its final folder.
get_filename_component(ARTPI2_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." REALPATH)
set(ARTPI2_SOURCE_PREFIX_MAP "-fmacro-prefix-map=${ARTPI2_SOURCE_ROOT}=.")
target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE "${ARTPI2_SOURCE_PREFIX_MAP}")

# Include mx-generated
include("mx-generated.cmake")
target_compile_options(STM32_Drivers PRIVATE "${ARTPI2_SOURCE_PREFIX_MAP}")

set(THIRDPARTY_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty)
set(ARTPI2_ROOT ${THIRDPARTY_ROOT}/artpi2)
set(RTT_ROOT ${THIRDPARTY_ROOT}/rt-thread)
set(CHERRYUSB_ROOT ${THIRDPARTY_ROOT}/cherryusb)
set(WIFI_ROOT ${THIRDPARTY_ROOT}/wifi-host-driver)
set(WIFI_HOST_DRIVER_ROOT "${WIFI_ROOT}/WiFi_Host_Driver" CACHE PATH
    "Locally obtained Infineon WHD 3.3.3.26314 package root")
if(NOT EXISTS "${WIFI_HOST_DRIVER_ROOT}/src/whd_wifi.c" OR
   NOT EXISTS "${WIFI_HOST_DRIVER_ROOT}/resources/firmware/COMPONENT_43438/43438A1_bin.c")
    message(FATAL_ERROR
        "WHD package missing. Obtain the licensed Infineon WHD 3.3.3.26314 package, "
        "then pass -DWIFI_HOST_DRIVER_ROOT=<path-to-WiFi_Host_Driver>.")
endif()
file(READ "${WIFI_HOST_DRIVER_ROOT}/inc/whd_version.h" WHD_VERSION_TEXT)
if(NOT WHD_VERSION_TEXT MATCHES "WHD_VERSION[ \t]+\"3\\.3\\.3\\.26314\"")
    message(FATAL_ERROR
        "WIFI_HOST_DRIVER_ROOT must be the validated WHD 3.3.3.26314 release.")
endif()
set(WIFI_PORT_ROOT ${THIRDPARTY_ROOT}/wifi-host-driver-porting)
set(MONGOOSE_ROOT ${THIRDPARTY_ROOT}/mongoose)
set(LVGL_ROOT ${THIRDPARTY_ROOT}/lvgl)
set(LIBHARU_ROOT ${THIRDPARTY_ROOT}/libharu)
set(NEMA_ROOT ${LVGL_ROOT}/libs/nema_gfx)
set(NEMA_LIB ${NEMA_ROOT}/lib/core/cortex_m7/gcc/libnemagfx-float-abi-hard.a)

set(WORK_IDENTITY_INPUTS
    CMakeLists.txt
    User/config/build_identity.h.in
    mx-generated.cmake
    stm32h7rsxx_ROMxspi2.ld
    ../CMakeLists.txt
    ../CMakePresets.json
    ../cmake/project_contexts.cmake
    ../ArtPi2Studio.exe
    ../cmake/wifi_flash_contract.cmake
    ../cmake/check_flash_loads.cmake
    ../cmake/check_xip_journal.cmake
    ../tests/wifi_nv/check_wifi_nv_layout.cmake
    ../Boot/CMakeLists.txt
    ../Boot/Core/Src/main.c
    ../Boot/Core/Src/stm32h7rsxx_it.c
    ../Boot/Core/Startup/startup_stm32h7r7xx.s
    ../Boot/CMakePresets.json
    ../Boot/mx-generated.cmake
    ../Boot/User/extmem_manager.c
    ../Boot/User/extmem_manager.h
    ../Boot/User/watchdog_flash_log.c
    ../Boot/User/watchdog_flash_log.h
    ../Boot/stm32h7r7xx_flash.ld
    ../Boot/ThirdParty/w35t51nwtbie/w35t51nwtbie.c
    ../Boot/ThirdParty/w35t51nwtbie/w35t51nwtbie.h
    ../dependencies/third_party.lock
    ../packages/usb_enum_diag/src/usbdiag_core.h
    ../packages/usb_enum_diag/src/usbdiag_core.c
    ../packages/usb_enum_diag/src/usbdiag_session.h
    ../packages/usb_enum_diag/src/usbdiag_session.c
    ../packages/usb_enum_diag/src/usbdiag_version.h
    ../packages/usb_enum_diag/CMakeLists.txt
    ../packages/usb_enum_diag/Kconfig
    ../packages/usb_enum_diag/SConscript
    ../packages/usb_enum_diag/package.json
    ../packages/watchdog_crashlog/include/watchdog_record.h
    ../packages/watchdog_crashlog/src/watchdog_record.c
    ../packages/watchdog_crashlog/include/watchdog_trace.h
    ../packages/watchdog_crashlog/src/watchdog_trace.c
    ../packages/watchdog_crashlog/include/watchdog_rtt_supervisor.h
    ../packages/watchdog_crashlog/src/watchdog_rtt_supervisor.c
    ../packages/watchdog_crashlog/CMakeLists.txt
    ../packages/watchdog_crashlog/Kconfig
    ../packages/watchdog_crashlog/SConscript
    ../packages/watchdog_crashlog/README.md
    ../packages/watchdog_crashlog/package.json
    ../Drivers/ART-Pi2/watchdog_h7r7_port.h
    ../Drivers/ART-Pi2/watchdog_h7r7_port.c
    ../packages/wifi_profile_manager/include/wifi_profile_manager.h
    ../packages/wifi_profile_manager/src/wifi_profile_manager.c
    ../packages/wifi_profile_manager/include/wpm_nv_journal.h
    ../packages/wifi_profile_manager/src/wpm_nv_journal.c
    ../packages/wifi_profile_manager/tests/test_nv_journal.c
    ../packages/wifi_profile_manager/CMakeLists.txt
    ../packages/wifi_profile_manager/Kconfig
    ../packages/wifi_profile_manager/SConscript
    ../packages/wifi_profile_manager/README.md
    ../packages/wifi_profile_manager/package.json
    ThirdParty/cherryusb/core/usbh_core.c
    ThirdParty/cherryusb/core/usbh_core.h
)
file(GLOB_RECURSE WORK_IDENTITY_BOOT_FILES CONFIGURE_DEPENDS
    LIST_DIRECTORIES false
    "${CMAKE_CURRENT_SOURCE_DIR}/../Boot/Core/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Boot/User/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Boot/ThirdParty/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Boot/*.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Boot/*.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Boot/*.ld")
foreach(identity_path IN LISTS WORK_IDENTITY_BOOT_FILES)
    file(RELATIVE_PATH identity_relative "${CMAKE_CURRENT_SOURCE_DIR}"
         "${identity_path}")
    list(APPEND WORK_IDENTITY_INPUTS "${identity_relative}")
endforeach()
file(GLOB_RECURSE WORK_IDENTITY_USER_FILES CONFIGURE_DEPENDS LIST_DIRECTORIES false
    "${CMAKE_CURRENT_SOURCE_DIR}/User/*")
list(FILTER WORK_IDENTITY_USER_FILES EXCLUDE REGEX
    "(^|[/\\])(wifi_secrets\\.h|[^/\\]+\\.(pem|key|p12|pfx|secret))$")
foreach(identity_path IN LISTS WORK_IDENTITY_USER_FILES)
    file(RELATIVE_PATH identity_relative "${CMAKE_CURRENT_SOURCE_DIR}"
         "${identity_path}")
    list(APPEND WORK_IDENTITY_INPUTS "${identity_relative}")
endforeach()
file(GLOB_RECURSE WORK_IDENTITY_CUBEMX_FILES CONFIGURE_DEPENDS LIST_DIRECTORIES false
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.ioc")
foreach(identity_path IN LISTS WORK_IDENTITY_CUBEMX_FILES)
    file(RELATIVE_PATH identity_relative "${CMAKE_CURRENT_SOURCE_DIR}"
         "${identity_path}")
    list(APPEND WORK_IDENTITY_INPUTS "${identity_relative}")
endforeach()
file(GLOB_RECURSE WORK_IDENTITY_BOARD_FILES CONFIGURE_DEPENDS LIST_DIRECTORIES false
    "${ARTPI2_ROOT}/*"
    "${WIFI_PORT_ROOT}/*")
foreach(identity_path IN LISTS WORK_IDENTITY_BOARD_FILES)
    file(RELATIVE_PATH identity_relative "${CMAKE_CURRENT_SOURCE_DIR}"
         "${identity_path}")
    list(APPEND WORK_IDENTITY_INPUTS "${identity_relative}")
endforeach()
file(GLOB_RECURSE WORK_IDENTITY_LIBHARU_FILES CONFIGURE_DEPENDS LIST_DIRECTORIES false
    "${LIBHARU_ROOT}/include/*"
    "${LIBHARU_ROOT}/src/*"
    "${LIBHARU_ROOT}/LICENSE"
    "${LIBHARU_ROOT}/PATCHES.md")
foreach(identity_path IN LISTS WORK_IDENTITY_LIBHARU_FILES)
    file(RELATIVE_PATH identity_relative "${CMAKE_CURRENT_SOURCE_DIR}"
         "${identity_path}")
    list(APPEND WORK_IDENTITY_INPUTS "${identity_relative}")
endforeach()
list(REMOVE_DUPLICATES WORK_IDENTITY_INPUTS)
list(SORT WORK_IDENTITY_INPUTS)
set(WORK_IDENTITY_DEPENDENCIES)
foreach(identity_file IN LISTS WORK_IDENTITY_INPUTS)
    get_filename_component(identity_dependency
        "${CMAKE_CURRENT_SOURCE_DIR}/${identity_file}" REALPATH)
    list(APPEND WORK_IDENTITY_DEPENDENCIES "${identity_dependency}")
endforeach()
list(REMOVE_DUPLICATES WORK_IDENTITY_DEPENDENCIES)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             ${WORK_IDENTITY_DEPENDENCIES})
set(WORK_IDENTITY_MATERIAL
    "profile=${RTTHREAD_PROFILE};allocator=${RTTHREAD_ALLOCATOR};"
    "lvgl_profile=${LVGL_PROFILE};draw_dma2d=${LVGL_DRAW_DMA2D};"
    "build_type=${CMAKE_BUILD_TYPE};compiler=${CMAKE_C_COMPILER_ID}-"
    "${CMAKE_C_COMPILER_VERSION};")
if(CMAKE_TOOLCHAIN_FILE AND EXISTS "${CMAKE_TOOLCHAIN_FILE}")
    file(SHA256 "${CMAKE_TOOLCHAIN_FILE}" WORK_TOOLCHAIN_SHA256)
    string(APPEND WORK_IDENTITY_MATERIAL
        "toolchain_sha256=${WORK_TOOLCHAIN_SHA256};")
endif()
foreach(identity_file IN LISTS WORK_IDENTITY_INPUTS)
    if(IS_ABSOLUTE "${identity_file}")
        set(identity_path "${identity_file}")
    else()
        set(identity_path "${CMAKE_CURRENT_SOURCE_DIR}/${identity_file}")
    endif()
    if(NOT EXISTS "${identity_path}")
        message(FATAL_ERROR "Build identity input is missing: ${identity_path}")
    endif()
    file(SHA256 "${identity_path}" identity_hash)
    string(APPEND WORK_IDENTITY_MATERIAL "${identity_file}=${identity_hash};")
endforeach()
string(SHA256 WORK_SOURCE_FINGERPRINT "${WORK_IDENTITY_MATERIAL}")
set(WORK_IDENTITY_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/identity")
file(MAKE_DIRECTORY "${WORK_IDENTITY_GENERATED_DIR}")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/User/config/build_identity.h.in"
    "${WORK_IDENTITY_GENERATED_DIR}/build_identity.h"
    @ONLY)

if(NOT EXISTS ${NEMA_LIB})
    message(FATAL_ERROR "Project-local M7 NemaGFX library is missing: ${NEMA_LIB}")
endif()

file(READ "${LVGL_ROOT}/include/lvgl/lv_version.h" LVGL_VERSION_TEXT)
if(NOT LVGL_VERSION_TEXT MATCHES
    "LVGL_VERSION_MAJOR[ \t]+9" OR
   NOT LVGL_VERSION_TEXT MATCHES
    "LVGL_VERSION_MINOR[ \t]+6" OR
   NOT LVGL_VERSION_TEXT MATCHES
    "LVGL_VERSION_PATCH[ \t]+0")
    message(FATAL_ERROR
        "Project LVGL tree must be official LVGL 9.6.0")
endif()

# Keep the embedded source manifest explicit at the subsystem level.  This
# prevents host backends, alternate RTOS ports, tests and unrelated decoders
# from silently entering the firmware when the upstream tree grows.
set(LVGL_SOURCE_DIRS
    ${LVGL_ROOT}/src
    ${LVGL_ROOT}/src/core
    ${LVGL_ROOT}/src/display
    ${LVGL_ROOT}/src/draw/convert
    ${LVGL_ROOT}/src/draw/dma2d
    ${LVGL_ROOT}/src/draw/nema_gfx
    ${LVGL_ROOT}/src/draw/sw
    ${LVGL_ROOT}/src/font
    ${LVGL_ROOT}/src/indev
    ${LVGL_ROOT}/src/layouts
    ${LVGL_ROOT}/src/logging
    ${LVGL_ROOT}/src/misc
    ${LVGL_ROOT}/src/stdlib
    ${LVGL_ROOT}/src/themes
    ${LVGL_ROOT}/src/tick
)
set(LVGL_SOURCES)
foreach(LVGL_SOURCE_DIR IN LISTS LVGL_SOURCE_DIRS)
    if(LVGL_SOURCE_DIR STREQUAL "${LVGL_ROOT}/src")
        file(GLOB LVGL_SOURCE_FILES CONFIGURE_DEPENDS
            ${LVGL_SOURCE_DIR}/*.c)
    else()
        file(GLOB_RECURSE LVGL_SOURCE_FILES CONFIGURE_DEPENDS
            ${LVGL_SOURCE_DIR}/*.c)
    endif()
    list(APPEND LVGL_SOURCES ${LVGL_SOURCE_FILES})
endforeach()

# Only these widgets are instantiated by the project UI.  Keep this list
# explicit so an upstream LVGL widget cannot silently become firmware code.
set(LVGL_WIDGET_SOURCES
    ${LVGL_ROOT}/src/widgets/button/lv_button.c
    ${LVGL_ROOT}/src/widgets/buttonmatrix/lv_buttonmatrix.c
    ${LVGL_ROOT}/src/widgets/canvas/lv_canvas.c
    ${LVGL_ROOT}/src/widgets/image/lv_image.c
    ${LVGL_ROOT}/src/widgets/keyboard/lv_keyboard.c
    ${LVGL_ROOT}/src/widgets/label/lv_label.c
    ${LVGL_ROOT}/src/widgets/roller/lv_roller.c
    ${LVGL_ROOT}/src/widgets/textarea/lv_textarea.c
)
list(APPEND LVGL_SOURCES ${LVGL_WIDGET_SOURCES})

file(GLOB LVGL_DRAW_CORE_SOURCES CONFIGURE_DEPENDS
    ${LVGL_ROOT}/src/draw/*.c)
list(APPEND LVGL_SOURCES ${LVGL_DRAW_CORE_SOURCES})
file(GLOB LVGL_FS_SOURCES CONFIGURE_DEPENDS
    ${LVGL_ROOT}/src/fs/lv_fs.c)
list(APPEND LVGL_SOURCES ${LVGL_FS_SOURCES})
file(GLOB LVGL_IMAGE_SOURCES CONFIGURE_DEPENDS
    ${LVGL_ROOT}/src/image/lv_image_decoder.c
    ${LVGL_ROOT}/src/image/lv_bin_decoder.c)
list(APPEND LVGL_SOURCES ${LVGL_IMAGE_SOURCES})
if(LVGL_PROFILE STREQUAL "performance")
    list(APPEND LVGL_SOURCES
        ${LVGL_ROOT}/src/debugging/profiler/lv_profiler_builtin.c)
endif()
file(GLOB LVGL_OSAL_SOURCES CONFIGURE_DEPENDS
    ${LVGL_ROOT}/src/osal/lv_os.c
    ${LVGL_ROOT}/src/osal/lv_rtthread.c)
list(APPEND LVGL_SOURCES ${LVGL_OSAL_SOURCES})
file(GLOB LVGL_ST_LTDC_SOURCES CONFIGURE_DEPENDS
    ${LVGL_ROOT}/src/drivers/display/st_ltdc/*.c)
list(APPEND LVGL_SOURCES ${LVGL_ST_LTDC_SOURCES})

# Cortex-M7 uses the portable software blend path.  Keep the source manifest
# free of other CPU extensions and all non-embedded draw backends.
list(FILTER LVGL_SOURCES EXCLUDE REGEX
    "/(arm2d|dave2d|eve|helium|nanovg|neon|nuttx|opengles|qnx|riscv_v|sdl|sve2|vg_lite|wayland|windows|x11)/")
list(REMOVE_DUPLICATES LVGL_SOURCES)

set(RTT_IPC_SOURCES
    ${RTT_ROOT}/components/drivers/ipc/completion_comm.c
    ${RTT_ROOT}/components/drivers/ipc/completion_up.c
    ${RTT_ROOT}/components/drivers/ipc/waitqueue.c
    ${RTT_ROOT}/components/drivers/ipc/workqueue.c
)

# Mongoose is built as one amalgamated translation unit.  Its native CYW
# backend is deliberately disabled below; the project-local Infineon WHD
# WiFi5 driver owns the radio and this file only provides the TCP/IP/Web layer.
set(MONGOOSE_WIFI_SOURCES
    ${MONGOOSE_ROOT}/mongoose.c
    ${WIFI_HOST_DRIVER_ROOT}/resources/firmware/COMPONENT_43438/43438A1_bin.c
    ${WIFI_HOST_DRIVER_ROOT}/resources/clm/COMPONENT_43438/43438A1_clm_blob.c
)
set_source_files_properties(${MONGOOSE_WIFI_SOURCES}
    PROPERTIES COMPILE_OPTIONS "-mno-unaligned-access")

# Use the already validated WHD WiFi5 package for CYW43438. Keep the source
# list explicit so alternate bus backends do not enter the STM32 SDIO image.
set(WHD_CORE_SOURCES
    ${WIFI_HOST_DRIVER_ROOT}/src/whd_ap.c
    ${WIFI_HOST_DRIVER_ROOT}/src/whd_buffer_api.c
    ${WIFI_HOST_DRIVER_ROOT}/src/whd_cdc_bdc.c
    ${WIFI_HOST_DRIVER_ROOT}/src/whd_chip.c
    ${WIFI_HOST_DRIVER_ROOT}/src/whd_chip_constants.c
    ${WIFI_HOST_DRIVER_ROOT}/src/whd_clm.c
    ${WIFI_HOST_DRIVER_ROOT}/src/whd_debug.c
    ${WIFI_HOST_DRIVER_ROOT}/src/whd_events.c
    ${WIFI_HOST_DRIVER_ROOT}/src/whd_logging.c
    ${WIFI_HOST_DRIVER_ROOT}/src/whd_management.c
    ${WIFI_HOST_DRIVER_ROOT}/src/whd_network_if.c
    ${WIFI_HOST_DRIVER_ROOT}/src/whd_proto.c
    ${WIFI_HOST_DRIVER_ROOT}/src/whd_resource_if.c
    ${WIFI_HOST_DRIVER_ROOT}/src/whd_sdpcm.c
    ${WIFI_HOST_DRIVER_ROOT}/src/whd_thread.c
    ${WIFI_HOST_DRIVER_ROOT}/src/whd_utils.c
    ${WIFI_HOST_DRIVER_ROOT}/src/whd_wifi.c
    ${WIFI_HOST_DRIVER_ROOT}/src/whd_wifi_api.c
    ${WIFI_HOST_DRIVER_ROOT}/src/whd_wifi_p2p.c
)
set(WHD_BUS_SOURCES
    ${WIFI_HOST_DRIVER_ROOT}/src/bus_protocols/whd_bus.c
    ${WIFI_HOST_DRIVER_ROOT}/src/bus_protocols/whd_bus_common.c
    ${WIFI_HOST_DRIVER_ROOT}/src/bus_protocols/whd_bus_sdio_protocol.c
)
set(WHD_PORT_SOURCES
    ${WIFI_PORT_ROOT}/src/hal/cyhal_gpio.c
    ${WIFI_PORT_ROOT}/src/hal/whd_stm32_sdio.c
    ${WIFI_PORT_ROOT}/src/resources/whd_resources_memory.c
    ${WIFI_PORT_ROOT}/src/bsp/whd_mongoose_buffer.c
    ${WIFI_PORT_ROOT}/src/rtos/whd_rtos.c
)
set(WHD_SOURCES ${WHD_CORE_SOURCES} ${WHD_BUS_SOURCES} ${WHD_PORT_SOURCES})
set_source_files_properties(${WHD_SOURCES}
    PROPERTIES
    COMPILE_DEFINITIONS
    "WHD_USE_CUSTOM_MALLOC_IMPL;WPRINT_ENABLE_WHD_INFO;CYHAL_API_VERSION=1;CYBSP_SDIO_INTERFACE=0;CYBSP_SPI_INTERFACE=1;CYBSP_M2M_INTERFACE=2;CYBSP_WIFI_INTERFACE_TYPE=0"
    COMPILE_OPTIONS "-mno-unaligned-access")

set(RTT_KERNEL_SOURCES
    ${RTT_ROOT}/src/clock.c
    ${RTT_ROOT}/src/cpu_up.c
    ${RTT_ROOT}/src/defunct.c
    ${RTT_ROOT}/src/idle.c
    ${RTT_ROOT}/src/ipc.c
    ${RTT_ROOT}/src/irq.c
    ${RTT_ROOT}/src/kservice.c
    ${RTT_ROOT}/src/mem.c
    ${RTT_ROOT}/src/memheap.c
    ${RTT_ROOT}/src/mempool.c
    ${RTT_ROOT}/src/object.c
    ${RTT_ROOT}/src/scheduler_comm.c
    ${RTT_ROOT}/src/scheduler_up.c
    ${RTT_ROOT}/src/signal.c
    ${RTT_ROOT}/src/slab.c
    ${RTT_ROOT}/src/thread.c
    ${RTT_ROOT}/src/timer.c
    ${RTT_ROOT}/src/components.c
    ${RTT_ROOT}/src/klibc/kerrno.c
    ${RTT_ROOT}/src/klibc/kstdio.c
    ${RTT_ROOT}/src/klibc/kstring.c
    ${RTT_ROOT}/src/klibc/rt_vsnprintf_tiny.c
)
set_source_files_properties(${RTT_KERNEL_SOURCES}
    PROPERTIES COMPILE_DEFINITIONS __RT_KERNEL_SOURCE__)

set(RTT_PORT_SOURCES
    ${RTT_ROOT}/libcpu/arm/common/atomic_arm.c
    ${RTT_ROOT}/libcpu/arm/cortex-m7/context_gcc.S
    ${RTT_ROOT}/libcpu/arm/cortex-m7/cpuport.c
    ${RTT_ROOT}/libcpu/arm/cortex-m7/cpu_cache.c
)

set(RTT_DRIVER_SOURCES
    ${RTT_ROOT}/components/drivers/core/device.c
    ${RTT_ROOT}/components/drivers/i2c/dev_i2c_core.c
    ${RTT_ROOT}/components/drivers/i2c/dev_i2c_dev.c
    ${RTT_ROOT}/components/drivers/pin/pin.c
    ${ARTPI2_ROOT}/libraries/drivers/drv_gpio.c
)

# Project-local copy of RT-Thread's generic DMA channel API.  The upstream
# Smart/OFW pool backend is replaced by the H7RS fixed-region backend below;
# the public RT_DMA API and channel lifecycle remain available to drivers.
set(RTT_DMA_SOURCES
    ${RTT_ROOT}/components/drivers/dma/dma.c
    ${RTT_ROOT}/components/drivers/dma/dma_pool.c
)

set(RTT_FINSH_SOURCES
    ${RTT_ROOT}/components/finsh/shell.c
    ${RTT_ROOT}/components/finsh/msh.c
    ${RTT_ROOT}/components/finsh/msh_parse.c
    ${RTT_ROOT}/components/finsh/cmd.c
)

set(CHERRYUSB_SOURCES
    ${CHERRYUSB_ROOT}/core/usbh_core.c
    ${CHERRYUSB_ROOT}/osal/usb_osal_rtthread.c
    ${CHERRYUSB_ROOT}/class/hub/usbh_hub.c
    ${CHERRYUSB_ROOT}/class/cdc/usbh_cdc_acm.c
    ${CHERRYUSB_ROOT}/class/hid/usbh_hid.c
    ${CHERRYUSB_ROOT}/class/msc/usbh_msc.c
    ${CHERRYUSB_ROOT}/port/dwc2/usb_hc_dwc2.c
    ${CHERRYUSB_ROOT}/port/dwc2/usb_glue_st.c
    ${CHERRYUSB_ROOT}/platform/rtthread/usb_msh.c
    ${CHERRYUSB_ROOT}/platform/rtthread/usb_check.c
)

set_source_files_properties(${CHERRYUSB_SOURCES}
    PROPERTIES
    COMPILE_DEFINITIONS __RTTHREAD__
    # CherryUSB protocol descriptors/setup packets are packed.  H7RS traps
    # unaligned word accesses when UNALIGN_TRP is enabled, while GCC -O2 can
    # otherwise coalesce packed member accesses into a 32-bit load/store.
    COMPILE_OPTIONS "-mno-unaligned-access")

set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/User/usb/src/usb_descriptor_store.c
    ${CMAKE_CURRENT_SOURCE_DIR}/User/usb/src/usb_evidence.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../packages/usb_enum_diag/src/usbdiag_core.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../packages/usb_enum_diag/src/usbdiag_session.c
    PROPERTIES COMPILE_OPTIONS "-mno-unaligned-access")
set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/../packages/usb_enum_diag/src/usbdiag_core.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../packages/usb_enum_diag/src/usbdiag_session.c
    PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)

add_subdirectory(User)
file(GLOB LIBHARU_SOURCES CONFIGURE_DEPENDS
    "${LIBHARU_ROOT}/src/*.c")
set(LIBHARU_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/libharu")
file(MAKE_DIRECTORY "${LIBHARU_GENERATED_DIR}")
set(LIBHPDF_HAVE_LIBPNG "")
set(LIBHPDF_HAVE_ZLIB "")
set(LIBHPDF_DEBUG "")
set(LIBHPDF_DEBUG_TRACE "")
configure_file(
    "${LIBHARU_ROOT}/include/hpdf_config.h.cmake"
    "${LIBHARU_GENERATED_DIR}/hpdf_config.h")
set_source_files_properties(${LIBHARU_SOURCES}
    PROPERTIES
    COMPILE_DEFINITIONS "LIBHPDF_NO_ENCRYPTION=1"
    COMPILE_OPTIONS "-mno-unaligned-access")
list(APPEND APP_USER_SOURCES ${LIBHARU_SOURCES})
list(APPEND APP_USER_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../packages/usb_enum_diag/src/usbdiag_core.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../packages/usb_enum_diag/src/usbdiag_session.c
    ${MONGOOSE_WIFI_SOURCES}
    ${LVGL_SOURCES}
)

target_sources(STM32_Drivers PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../Drivers/STM32H7RSxx_HAL_Driver/Src/stm32h7rsxx_hal_tim.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Drivers/STM32H7RSxx_HAL_Driver/Src/stm32h7rsxx_hal_tim_ex.c
)
target_compile_definitions(STM32_Drivers PRIVATE
    HAL_TIM_MODULE_ENABLED
    USE_HAL_LTDC_REGISTER_CALLBACKS=1
    ${WORK_EVIDENCE_DIAGNOSTICS_DEFINE}
)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    ${RTT_KERNEL_SOURCES}
    ${RTT_PORT_SOURCES}
    ${RTT_DRIVER_SOURCES}
    ${RTT_DMA_SOURCES}
    ${RTT_IPC_SOURCES}
    ${RTT_FINSH_SOURCES}
    ${CHERRYUSB_SOURCES}
    ${WHD_SOURCES}
    ${APP_USER_SOURCES}
)

# Keep the Web page editable as ordinary source files while storing the
# shipped assets in XIP Flash. The existing Mongoose response buffer is
# 24,576 bytes, so each individual asset must fit without growing RAM use.
set(WEB_ASSET_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/User/web/assets")
set(WEB_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/web")
set(WEB_ASSET_SOURCE "${WEB_GENERATED_DIR}/web_assets.c")
set(WEB_ASSET_HEADER "${WEB_GENERATED_DIR}/web_assets.h")
set(WEB_ASSET_FILES
    "${WEB_ASSET_ROOT}/index.html"
    "${WEB_ASSET_ROOT}/style.css"
    "${WEB_ASSET_ROOT}/data.js"
    "${WEB_ASSET_ROOT}/asset_loader.js"
    "${WEB_ASSET_ROOT}/app.js"
    "${WEB_ASSET_ROOT}/optional_features.js"
    "${WEB_ASSET_ROOT}/evidence_poll.js"
    "${WEB_ASSET_ROOT}/tasks.js"
    "${WEB_ASSET_ROOT}/build_info.js"
    "${WEB_ASSET_ROOT}/history.js"
    "${WEB_ASSET_ROOT}/topology.js"
    "${WEB_ASSET_ROOT}/topology_raw.js"
    "${WEB_ASSET_ROOT}/report_pdf.js"
    "${WEB_ASSET_ROOT}/lists.js")
set(ARTPI2_STUDIO_EXE "" CACHE FILEPATH
    "ART-Pi2 Studio executable used for host-side resource generation and checks")
if(NOT ARTPI2_STUDIO_EXE AND DEFINED ENV{ARTPI2_STUDIO_EXE} AND
   NOT "$ENV{ARTPI2_STUDIO_EXE}" STREQUAL "")
    set(ARTPI2_STUDIO_EXE "$ENV{ARTPI2_STUDIO_EXE}")
endif()
if(NOT ARTPI2_STUDIO_EXE)
    set(_bundled_studio "${CMAKE_CURRENT_SOURCE_DIR}/../ArtPi2Studio.exe")
    if(EXISTS "${_bundled_studio}" AND NOT IS_DIRECTORY "${_bundled_studio}")
        set(ARTPI2_STUDIO_EXE "${_bundled_studio}")
    endif()
endif()
if(NOT ARTPI2_STUDIO_EXE)
    message(FATAL_ERROR "ART-Pi2 Studio was not found. Build from the GUI or set -DARTPI2_STUDIO_EXE=<project-root>/ArtPi2Studio.exe.")
endif()
get_filename_component(ARTPI2_STUDIO_EXE "${ARTPI2_STUDIO_EXE}" ABSOLUTE
    BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")
if(NOT EXISTS "${ARTPI2_STUDIO_EXE}" OR IS_DIRECTORY "${ARTPI2_STUDIO_EXE}")
    message(FATAL_ERROR "ARTPI2_STUDIO_EXE does not name an existing file: ${ARTPI2_STUDIO_EXE}")
endif()
add_custom_command(
    OUTPUT "${WEB_ASSET_SOURCE}" "${WEB_ASSET_HEADER}"
    COMMAND "${ARTPI2_STUDIO_EXE}" internal web-assets
        --asset-root "${WEB_ASSET_ROOT}"
        --output-c "${WEB_ASSET_SOURCE}"
        --output-h "${WEB_ASSET_HEADER}"
        --max-asset-bytes 24575
    DEPENDS ${WEB_ASSET_FILES} "${ARTPI2_STUDIO_EXE}"
    VERBATIM)
add_custom_target(work_web_assets
    DEPENDS "${WEB_ASSET_SOURCE}" "${WEB_ASSET_HEADER}")

# The PDF font and glyph map are embedded for the board-side libHaru renderer.
# They are not exposed as browser assets; the UI streams the finished PDF.
set(WEB_PDF_FONT "${WEB_ASSET_ROOT}/pdf/NotoSansSC-ARTPi2-subset.ttf")
set(WEB_PDF_FONT_COVERAGE "${WEB_ASSET_ROOT}/pdf_font_coverage.js")
set(WEB_PDF_ASSET_SOURCE "${WEB_GENERATED_DIR}/web_pdf_assets.c")
set(WEB_PDF_ASSET_HEADER "${WEB_GENERATED_DIR}/web_pdf_assets.h")
add_custom_command(
    OUTPUT "${WEB_PDF_ASSET_SOURCE}" "${WEB_PDF_ASSET_HEADER}"
    COMMAND "${ARTPI2_STUDIO_EXE}" internal pdf-assets
        --font "${WEB_PDF_FONT}"
        --coverage "${WEB_PDF_FONT_COVERAGE}"
        --output-c "${WEB_PDF_ASSET_SOURCE}"
        --output-h "${WEB_PDF_ASSET_HEADER}"
        --max-font-bytes 4194304
    DEPENDS "${WEB_PDF_FONT}" "${WEB_PDF_FONT_COVERAGE}" "${ARTPI2_STUDIO_EXE}"
    VERBATIM)
add_custom_target(work_web_pdf_assets
    DEPENDS "${WEB_PDF_ASSET_SOURCE}" "${WEB_PDF_ASSET_HEADER}")
add_dependencies(${CMAKE_PROJECT_NAME} work_web_assets work_web_pdf_assets)
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    "${WEB_ASSET_SOURCE}"
    "${WEB_PDF_ASSET_SOURCE}")
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    "${WEB_GENERATED_DIR}"
    "${WORK_IDENTITY_GENERATED_DIR}")

# One manifest is the source of truth for all four NotoSansSC sizes. Fail every
# normal Appli build if fixed LCD text is undeclared, either generated cmap
# drifts from the manifest, or the two font sizes differ. Runtime USB/SSID text
# is sanitized against the same compiled fonts before it reaches LVGL.
set(LVGL_FONT_MANIFEST
    "${CMAKE_CURRENT_SOURCE_DIR}/User/ui/assets/ui_font_symbols.json")
file(GLOB_RECURSE LVGL_FONT_AUDIT_INPUTS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/User/ui/src/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/User/ui/src/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/User/ui/include/*.h")
list(APPEND LVGL_FONT_AUDIT_INPUTS
    "${CMAKE_CURRENT_SOURCE_DIR}/User/usb/src/usb_evidence.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/User/usb/include/usb_evidence.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/User/usb/src/usb_desc_parser.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/User/usb/include/usb_desc_parser.h"
    "${LVGL_FONT_MANIFEST}")
add_custom_target(work_lvgl_font_coverage
    COMMAND "${ARTPI2_STUDIO_EXE}" internal lvgl-font-audit
        --project-root "${CMAKE_CURRENT_SOURCE_DIR}/.."
        --manifest "${LVGL_FONT_MANIFEST}"
    DEPENDS "${ARTPI2_STUDIO_EXE}" ${LVGL_FONT_AUDIT_INPUTS}
    COMMENT "Checking LCD text and all four LVGL font subsets against the unified glyph manifest"
    VERBATIM)
add_dependencies(${CMAKE_PROJECT_NAME} work_lvgl_font_coverage)
add_custom_target(work_ui_design_audit
    COMMAND "${ARTPI2_STUDIO_EXE}" internal ui-design-audit
        --project-root "${CMAKE_CURRENT_SOURCE_DIR}/.."
    DEPENDS "${ARTPI2_STUDIO_EXE}" ${LVGL_FONT_AUDIT_INPUTS}
    COMMENT "Checking instrument UI contrast, text lifetime and keyboard glyphs"
    VERBATIM)
add_dependencies(${CMAKE_PROJECT_NAME} work_ui_design_audit)

# CubeMX's generated ISR still contains the HAL SysTick/HCD handlers.  Keep
# that file intact and give those two generated entry points private names;
# project-owned handlers in rtthread_systick.c and CherryUSB's glue then own
# the actual vector symbols.
set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/stm32h7rsxx_it.c
    PROPERTIES COMPILE_DEFINITIONS
    "SysTick_Handler=work_cubemx_systick_handler;OTG_HS_IRQHandler=work_cubemx_otg_hs_handler;LTDC_IRQHandler=work_cubemx_ltdc_handler;DMA2D_IRQHandler=work_cubemx_dma2d_handler;GPU2D_IRQHandler=work_cubemx_gpu2d_handler;GPU2D_ER_IRQHandler=work_cubemx_gpu2d_er_handler;app_dma2d_irq_dispatch=work_cubemx_legacy_dma2d_dispatch;SDMMC2_IRQHandler=work_cubemx_sdmmc2_handler;MemManage_Handler=work_cubemx_memmanage_generated_handler;BusFault_Handler=work_cubemx_busfault_generated_handler;UsageFault_Handler=work_cubemx_usagefault_generated_handler"
)

# CubeMX generated SDMMC2 is retained for its pin/clock MSP routine and the
# canonical SDIO2 Init profile.  The RT-Thread/Infineon host owns the card
# transaction sequence, so the generated card-init wrapper is intentionally a
# harmless compatibility call.  This keeps regeneration safe.
set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/sdmmc.c
    PROPERTIES COMPILE_DEFINITIONS "HAL_SDIO_Init=work_hal_sdio_init_noop"
)

# The route/JSON builder is retained, but its former lwIP socket worker is
# disabled.  mongoose_service.c owns the event-driven HTTP transport.
set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/User/web/src/web_server.c
    PROPERTIES COMPILE_DEFINITIONS "MONGOOSE_WEB_BACKEND=1"
)

# Boot leaves interrupts disabled until the RT-Thread scheduler is running.
# Keep CubeMX's generated HCD wrapper harmless during the pre-scheduler
# peripheral phase; usb_host_port.c calls the real HAL_HCD_Init() afterward.
set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Src/usb_otg.c
    PROPERTIES COMPILE_DEFINITIONS
    "HAL_HCD_Init=work_hal_hcd_init_noop"
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    STM32H7RS
    WORK_RTTHREAD_PROFILE=${RTTHREAD_PROFILE_VALUE}
    ${WORK_EVIDENCE_DIAGNOSTICS_DEFINE}
    WORK_RTTHREAD_ALLOCATOR=${RTTHREAD_ALLOCATOR_VALUE}
    ${RTTHREAD_PROFILE_DEFINES}
    HAL_TIM_MODULE_ENABLED
    USE_HAL_LTDC_REGISTER_CALLBACKS=1
    BSP_USING_GPIO
    MG_ENABLE_TCPIP=1
    MG_ENABLE_TCPIP_DRIVER_INIT=0
    LV_CONF_INCLUDE_SIMPLE
    LV_LVGL_H_INCLUDE_SIMPLE
    LV_DISABLE_API_MAPPING
    ${LVGL_PROFILE_DEFINES}
    ${LVGL_DRAW_DMA2D_DEFINE}
)

# Enable CMake support for C and ASM languages.
enable_language(C ASM)

target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE
    -O2
    -g3
    -fdata-sections
    -ffunction-sections
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${APP_USER_INCLUDE_DIRS}
    "${LIBHARU_ROOT}/include"
    "${LIBHARU_GENERATED_DIR}"
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Inc
    ${RTT_ROOT}/include
    ${RTT_ROOT}/components/libc/compilers/common/include
    ${RTT_ROOT}/components/utilities/libadt/bitmap
    ${RTT_ROOT}/components/utilities/libadt/ref
    ${RTT_ROOT}/libcpu/arm/cortex-m7
    ${RTT_ROOT}/components/finsh
    ${RTT_ROOT}/components/drivers/include
    ${RTT_ROOT}/components/drivers/include/drivers
    ${RTT_ROOT}/components/drivers/core
    ${ARTPI2_ROOT}/libraries/drivers/include
    ${ARTPI2_ROOT}/board
    ${MONGOOSE_ROOT}
    # Porting headers must precede the generic External headers.  The former
    # supplies the ART-Pi2 pin/type definitions used by official WHD.
    ${WIFI_PORT_ROOT}/inc
    ${WIFI_PORT_ROOT}/inc/bsp
    ${WIFI_PORT_ROOT}/inc/hal
    ${WIFI_PORT_ROOT}/inc/rtos
    ${WIFI_ROOT}
    ${WIFI_HOST_DRIVER_ROOT}/inc
    ${WIFI_HOST_DRIVER_ROOT}/src/include
    ${WIFI_HOST_DRIVER_ROOT}/src
    ${WIFI_HOST_DRIVER_ROOT}/src/bus_protocols
    ${WIFI_HOST_DRIVER_ROOT}/resources/resource_imp
    ${WIFI_HOST_DRIVER_ROOT}/resources/clm/COMPONENT_43438
    ${WIFI_HOST_DRIVER_ROOT}/resources/firmware/COMPONENT_43438
    ${WIFI_HOST_DRIVER_ROOT}/resources/nvram/COMPONENT_43438/COMPONENT_AW-CU427-P
    ${LVGL_ROOT}/include
    ${LVGL_ROOT}
    ${LVGL_ROOT}/src
    ${NEMA_ROOT}/include/cortex_m55_m7
    ${CMAKE_CURRENT_SOURCE_DIR}/../packages/usb_enum_diag/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../packages/wifi_profile_manager/include
    ${CHERRYUSB_ROOT}/common
    ${CHERRYUSB_ROOT}/core
    ${CHERRYUSB_ROOT}/osal
    ${CHERRYUSB_ROOT}/class/hub
    ${CHERRYUSB_ROOT}/class/cdc
    ${CHERRYUSB_ROOT}/class/hid
    ${CHERRYUSB_ROOT}/class/msc
    ${CHERRYUSB_ROOT}/port/dwc2
    ${CHERRYUSB_ROOT}/platform/rtthread
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Project-local STM32H7RS NeoChrom library.  This is the GCC Cortex-M7
# hard-float build shipped with the board's GPU2D component package.
target_link_libraries(${CMAKE_PROJECT_NAME} ${NEMA_LIB})

if(NOT CMAKE_OBJCOPY)
    message(FATAL_ERROR "The ARM toolchain must define CMAKE_OBJCOPY")
endif()
if(NOT CMAKE_READELF)
    find_program(CMAKE_READELF arm-none-eabi-readelf REQUIRED)
endif()
set(APP_XIP_IMAGE "${CMAKE_CURRENT_BINARY_DIR}/artpi2-usbdiag_Appli.xip.bin")
set(APP_XIP_SHA256 "${APP_XIP_IMAGE}.sha256")
add_custom_command(
    OUTPUT "${APP_XIP_IMAGE}" "${APP_XIP_SHA256}"
    COMMAND "${CMAKE_OBJCOPY}" -O binary --gap-fill 0xff
        --only-section=.isr_vector
        --only-section=.text
        --only-section=.rodata
        --only-section=.usbh_class_info
        --only-section=.ARM
        --only-section=.preinit_array
        --only-section=.init_array
        --only-section=.fini_array
        --only-section=.rti_fn
        --only-section=FSymTab
        --only-section=.data
        --only-section=.ramfunc
        "$<TARGET_FILE:${CMAKE_PROJECT_NAME}>" "${APP_XIP_IMAGE}"
    COMMAND "${CMAKE_COMMAND}"
        -DINPUT_FILE=${APP_XIP_IMAGE}
        -DOUTPUT_FILE=${APP_XIP_SHA256}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/write_sha256.cmake"
    COMMAND "${CMAKE_COMMAND}"
        -DELF=$<TARGET_FILE:${CMAKE_PROJECT_NAME}>
        -DBIN=${APP_XIP_IMAGE}
        -DREADELF=${CMAKE_READELF}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/check_xip_journal.cmake"
    DEPENDS ${CMAKE_PROJECT_NAME}
        "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/check_xip_journal.cmake"
    VERBATIM
    COMMENT "Creating and hashing the external XSPI XIP image")
add_custom_target(artpi2-usbdiag_Appli_xip ALL
    DEPENDS "${APP_XIP_IMAGE}" "${APP_XIP_SHA256}")

include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/wifi_flash_contract.cmake")
work_protect_wifi_flash(${CMAKE_PROJECT_NAME})

# Validate that STM32CubeMX code is compatible with C standard
if((CMAKE_C_STANDARD EQUAL 90) OR (CMAKE_C_STANDARD EQUAL 99))
    message(ERROR "Generated code requires C11 or higher")
endif()
