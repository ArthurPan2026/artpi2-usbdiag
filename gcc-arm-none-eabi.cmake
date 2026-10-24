set(CMAKE_SYSTEM_NAME               Generic)
set(CMAKE_SYSTEM_PROCESSOR          arm)

set(CMAKE_C_COMPILER_ID GNU)
set(CMAKE_CXX_COMPILER_ID GNU)

# Some default GCC settings.  STM32Cube's bundled GNU Arm toolchain is the
# reproducible compiler for this workspace; fall back to PATH for CI or a
# developer installation.  This also makes the checked-in CMake presets work
# from a clean build directory on Windows.
if(NOT DEFINED TOOLCHAIN_PREFIX)
    set(TOOLCHAIN_PREFIX arm-none-eabi-)
    if(WIN32)
        file(GLOB _STM32_GCC_CANDIDATES
            "C:/Users/*/AppData/Local/stm32cube/bundles/gnu-tools-for-stm32/*/bin/arm-none-eabi-gcc.exe")
        if(_STM32_GCC_CANDIDATES)
            list(SORT _STM32_GCC_CANDIDATES)
            list(REVERSE _STM32_GCC_CANDIDATES)
            list(GET _STM32_GCC_CANDIDATES 0 _STM32_GCC)
            get_filename_component(_STM32_GCC_BIN "${_STM32_GCC}" DIRECTORY)
            set(TOOLCHAIN_PREFIX "${_STM32_GCC_BIN}/arm-none-eabi-")
        endif()
    endif()
endif()

if(DEFINED _STM32_GCC_BIN)
    set(CMAKE_C_COMPILER   "${_STM32_GCC_BIN}/arm-none-eabi-gcc.exe")
    set(CMAKE_ASM_COMPILER "${CMAKE_C_COMPILER}")
    set(CMAKE_CXX_COMPILER "${_STM32_GCC_BIN}/arm-none-eabi-g++.exe")
    set(CMAKE_LINKER       "${_STM32_GCC_BIN}/arm-none-eabi-g++.exe")
    set(CMAKE_OBJCOPY      "${_STM32_GCC_BIN}/arm-none-eabi-objcopy.exe")
    set(CMAKE_SIZE         "${_STM32_GCC_BIN}/arm-none-eabi-size.exe")
else()
    set(CMAKE_C_COMPILER   ${TOOLCHAIN_PREFIX}gcc)
    set(CMAKE_ASM_COMPILER ${CMAKE_C_COMPILER})
    set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}g++)
    set(CMAKE_LINKER       ${TOOLCHAIN_PREFIX}g++)
    set(CMAKE_OBJCOPY      ${TOOLCHAIN_PREFIX}objcopy)
    set(CMAKE_SIZE         ${TOOLCHAIN_PREFIX}size)
endif()

set(CMAKE_EXECUTABLE_SUFFIX_ASM     ".elf")
set(CMAKE_EXECUTABLE_SUFFIX_C       ".elf")
set(CMAKE_EXECUTABLE_SUFFIX_CXX     ".elf")

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# MCU specific flags
set(TARGET_FLAGS "${STM32_MCU_FLAGS}")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${TARGET_FLAGS}")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -fdata-sections -ffunction-sections -fstack-usage")
set(CMAKE_ASM_FLAGS "${CMAKE_C_FLAGS} -x assembler-with-cpp -MMD -MP")

# The cyclomatic-complexity parameter must be defined for the Cyclomatic complexity feature in STM32CubeIDE to work.
# However, most GCC toolchains do not support this option, which causes a compilation error; for this reason, the feature is disabled by default.
# set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fcyclomatic-complexity")

set(CMAKE_C_FLAGS_DEBUG "-O2 -g3")
set(CMAKE_C_FLAGS_RELEASE "-O2 -g0")
set(CMAKE_CXX_FLAGS_DEBUG "-O2 -g3")
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -g0")

set(CMAKE_CXX_FLAGS "${CMAKE_C_FLAGS} -fno-rtti -fno-exceptions -fno-threadsafe-statics")

set(CMAKE_EXE_LINKER_FLAGS "${TARGET_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -T \"${CMAKE_SOURCE_DIR}/${STM32_LINKER_SCRIPT}\"")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} --specs=nano.specs")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-Map=${CMAKE_PROJECT_NAME}.map -Wl,--gc-sections")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--print-memory-usage")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${STM32_LINKER_OPTION}")
set(TOOLCHAIN_LINK_LIBRARIES "m")
