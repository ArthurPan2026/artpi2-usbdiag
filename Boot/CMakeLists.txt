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

message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Compiler options
set(STM32_MCU_FLAGS  "-mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard " )

#Linker options
set(STM32_LINKER_SCRIPT stm32h7r7xx_flash.ld)
set(STM32_LINKER_OPTION  )

# Set the project name
set(CMAKE_PROJECT_NAME artpi2-usbdiag_Boot)
project(${CMAKE_PROJECT_NAME})

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})
set_property(TARGET ${CMAKE_PROJECT_NAME} APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/stm32h7r7xx_flash.ld")
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/wifi_flash_contract.cmake")
work_protect_wifi_flash(${CMAKE_PROJECT_NAME})

# Include mx-generated
include("mx-generated.cmake")

# Keep __FILE__ independent of the checkout/staging directory. HAL assertions
# embed these paths in .rodata, and the internal Boot image has a 48 KiB limit.
get_filename_component(ARTPI2_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." REALPATH)
set(ARTPI2_SOURCE_PREFIX_MAP
    "-fmacro-prefix-map=${ARTPI2_SOURCE_ROOT}=.")
target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE
    "${ARTPI2_SOURCE_PREFIX_MAP}")
target_compile_options(STM32_Drivers PRIVATE
    "${ARTPI2_SOURCE_PREFIX_MAP}")

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    STM32H7RS
)

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/User
    ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/aps256xx
    ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/w35t51nwtbie
    ${CMAKE_CURRENT_SOURCE_DIR}/../packages/watchdog_crashlog/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../Drivers/ART-Pi2
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/User/extmem_manager.c
    ${CMAKE_CURRENT_SOURCE_DIR}/User/watchdog_flash_log.c
    ${CMAKE_CURRENT_SOURCE_DIR}/User/watchdog_trace_flash.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/aps256xx/aps256xx.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/w35t51nwtbie/w35t51nwtbie.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../packages/watchdog_crashlog/src/watchdog_record.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../packages/watchdog_crashlog/src/watchdog_trace.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Drivers/ART-Pi2/watchdog_h7r7_port.c
)

# The internal Boot image is capped at 48 KiB to protect the WiFi NV sector.
# Keep the two flash-log writers size-optimized; debug symbols remain enabled.
set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/User/watchdog_flash_log.c
    ${CMAKE_CURRENT_SOURCE_DIR}/User/watchdog_trace_flash.c
    PROPERTIES COMPILE_OPTIONS "-Os")

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined libraries
)

if(NOT CMAKE_OBJCOPY)
    message(FATAL_ERROR "The ARM toolchain must define CMAKE_OBJCOPY")
endif()
set(BOOT_IMAGE "${CMAKE_CURRENT_BINARY_DIR}/artpi2-usbdiag_Boot.bin")
set(BOOT_IMAGE_SHA256 "${BOOT_IMAGE}.sha256")
add_custom_command(
    OUTPUT "${BOOT_IMAGE}" "${BOOT_IMAGE_SHA256}"
    COMMAND "${CMAKE_OBJCOPY}" -O binary --gap-fill 0xff
        --only-section=.isr_vector
        --only-section=.text
        --only-section=.rodata
        --only-section=.ARM
        --only-section=.preinit_array
        --only-section=.init_array
        --only-section=.fini_array
        --only-section=.rti_fn
        --only-section=FSymTab
        --only-section=.data
        --only-section=.ramfunc
        "$<TARGET_FILE:${CMAKE_PROJECT_NAME}>" "${BOOT_IMAGE}"
    COMMAND "${CMAKE_COMMAND}"
        -DINPUT_FILE=${BOOT_IMAGE}
        -DOUTPUT_FILE=${BOOT_IMAGE_SHA256}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/write_sha256.cmake"
    DEPENDS ${CMAKE_PROJECT_NAME}
        "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/write_sha256.cmake"
    VERBATIM
    COMMENT "Creating and hashing the internal Boot image")
add_custom_target(artpi2-usbdiag_Boot_image ALL
    DEPENDS "${BOOT_IMAGE}" "${BOOT_IMAGE_SHA256}")

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Validate that STM32CubeMX code is compatible with C standard
if((CMAKE_C_STANDARD EQUAL 90) OR (CMAKE_C_STANDARD EQUAL 99))
    message(ERROR "Generated code requires C11 or higher")
endif()
