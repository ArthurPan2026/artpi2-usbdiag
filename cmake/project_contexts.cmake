include(ExternalProject)

if(NOT CMAKE_TOOLCHAIN_FILE)
    message(FATAL_ERROR "Configure the root project with gcc-arm-none-eabi.cmake")
endif()

set(_project_build_root "${CMAKE_BINARY_DIR}")
set(_artpi2_profile "${CMAKE_BUILD_TYPE}")
if(_artpi2_profile STREQUAL "")
    set(_artpi2_profile "Release")
endif()

set(ARTPI2_APPLI_BINARY_DIR
    "${CMAKE_SOURCE_DIR}/Appli/build/${_artpi2_profile}" CACHE PATH
    "Appli CMake binary directory")
set(ARTPI2_BOOT_BINARY_DIR
    "${CMAKE_SOURCE_DIR}/Boot/build/${_artpi2_profile}" CACHE PATH
    "Boot CMake binary directory")
get_filename_component(ARTPI2_APPLI_BINARY_DIR "${ARTPI2_APPLI_BINARY_DIR}"
    ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
get_filename_component(ARTPI2_BOOT_BINARY_DIR "${ARTPI2_BOOT_BINARY_DIR}"
    ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")

function(_artpi2_assert_disjoint_paths first_name first_path second_name second_path)
    file(TO_CMAKE_PATH "${first_path}" _first_path)
    file(TO_CMAKE_PATH "${second_path}" _second_path)
    cmake_path(NORMAL_PATH _first_path)
    cmake_path(NORMAL_PATH _second_path)
    if(WIN32)
        string(TOLOWER "${_first_path}" _first_compare)
        string(TOLOWER "${_second_path}" _second_compare)
    else()
        set(_first_compare "${_first_path}")
        set(_second_compare "${_second_path}")
    endif()
    string(REGEX REPLACE "/+$" "" _first_compare "${_first_compare}")
    string(REGEX REPLACE "/+$" "" _second_compare "${_second_compare}")
    if(_first_compare STREQUAL _second_compare)
        message(FATAL_ERROR
            "${first_name} and ${second_name} build directories must be distinct: ${first_path}")
    endif()
    string(FIND "${_second_compare}/" "${_first_compare}/" _first_prefix)
    string(FIND "${_first_compare}/" "${_second_compare}/" _second_prefix)
    if(_first_prefix EQUAL 0 OR _second_prefix EQUAL 0)
        message(FATAL_ERROR
            "${first_name} and ${second_name} build directories must not contain one another:\n"
            "  ${first_name}: ${first_path}\n  ${second_name}: ${second_path}")
    endif()
endfunction()

_artpi2_assert_disjoint_paths("Appli" "${ARTPI2_APPLI_BINARY_DIR}"
    "Boot" "${ARTPI2_BOOT_BINARY_DIR}")
_artpi2_assert_disjoint_paths("Appli" "${ARTPI2_APPLI_BINARY_DIR}"
    "root coordinator" "${_project_build_root}")
_artpi2_assert_disjoint_paths("Boot" "${ARTPI2_BOOT_BINARY_DIR}"
    "root coordinator" "${_project_build_root}")

function(_artpi2_validate_existing_cache component_name source_dir binary_dir)
    set(_cache_file "${binary_dir}/CMakeCache.txt")
    if(NOT EXISTS "${_cache_file}")
        return()
    endif()

    file(STRINGS "${_cache_file}" _cache_home_line
        REGEX "^CMAKE_HOME_DIRECTORY:INTERNAL=")
    file(STRINGS "${_cache_file}" _cache_build_type_line
        REGEX "^CMAKE_BUILD_TYPE:STRING=")
    file(STRINGS "${_cache_file}" _cache_generator_line
        REGEX "^CMAKE_GENERATOR:INTERNAL=")
    file(STRINGS "${_cache_file}" _cache_toolchain_line
        REGEX "^CMAKE_TOOLCHAIN_FILE:FILEPATH=")
    foreach(_line_var IN ITEMS _cache_home_line _cache_build_type_line
            _cache_generator_line _cache_toolchain_line)
        if(NOT "${${_line_var}}" STREQUAL "")
            string(REGEX REPLACE "^[^=]*=" "" ${_line_var} "${${_line_var}}")
        endif()
    endforeach()

    get_filename_component(_expected_source "${source_dir}" ABSOLUTE)
    get_filename_component(_expected_toolchain "${CMAKE_TOOLCHAIN_FILE}" ABSOLUTE
        BASE_DIR "${CMAKE_SOURCE_DIR}")
    set(_cache_problems "")
    if(NOT _cache_home_line)
        string(APPEND _cache_problems "\n  missing CMAKE_HOME_DIRECTORY")
    else()
        get_filename_component(_cached_source "${_cache_home_line}" ABSOLUTE)
        if(WIN32)
            string(TOLOWER "${_cached_source}" _cached_source_compare)
            string(TOLOWER "${_expected_source}" _expected_source_compare)
        else()
            set(_cached_source_compare "${_cached_source}")
            set(_expected_source_compare "${_expected_source}")
        endif()
        if(NOT _cached_source_compare STREQUAL _expected_source_compare)
            string(APPEND _cache_problems
                "\n  source is '${_cache_home_line}', expected '${_expected_source}'")
        endif()
    endif()
    if(NOT _cache_build_type_line STREQUAL _artpi2_profile)
        string(APPEND _cache_problems
            "\n  configuration is '${_cache_build_type_line}', expected '${_artpi2_profile}'")
    endif()
    if(NOT _cache_generator_line STREQUAL CMAKE_GENERATOR)
        string(APPEND _cache_problems
            "\n  generator is '${_cache_generator_line}', expected '${CMAKE_GENERATOR}'")
    endif()
    if(_cache_toolchain_line)
        get_filename_component(_cached_toolchain "${_cache_toolchain_line}" ABSOLUTE
            BASE_DIR "${binary_dir}")
        if(WIN32)
            string(TOLOWER "${_cached_toolchain}" _cached_toolchain_compare)
            string(TOLOWER "${_expected_toolchain}" _expected_toolchain_compare)
        else()
            set(_cached_toolchain_compare "${_cached_toolchain}")
            set(_expected_toolchain_compare "${_expected_toolchain}")
        endif()
        if(NOT _cached_toolchain_compare STREQUAL _expected_toolchain_compare)
            string(APPEND _cache_problems
                "\n  toolchain is '${_cache_toolchain_line}', expected '${_expected_toolchain}'")
        endif()
    else()
        string(APPEND _cache_problems "\n  missing CMAKE_TOOLCHAIN_FILE")
    endif()
    if(_cache_problems)
        message(FATAL_ERROR
            "Refusing to reuse incompatible ${component_name} CMake cache at '${_cache_file}':"
            "${_cache_problems}\nRemove or select a different build directory only after reviewing its contents.")
    endif()
endfunction()

_artpi2_validate_existing_cache("Appli" "${CMAKE_SOURCE_DIR}/Appli"
    "${ARTPI2_APPLI_BINARY_DIR}")
_artpi2_validate_existing_cache("Boot" "${CMAKE_SOURCE_DIR}/Boot"
    "${ARTPI2_BOOT_BINARY_DIR}")

set(_boot_elf "${ARTPI2_BOOT_BINARY_DIR}/artpi2-usbdiag_Boot.elf")

set(ARTPI2_STUDIO_EXE "" CACHE FILEPATH
    "ART-Pi2 Studio executable used by Appli host-side resource generation and checks")
if(NOT ARTPI2_STUDIO_EXE AND DEFINED ENV{ARTPI2_STUDIO_EXE} AND
   NOT "$ENV{ARTPI2_STUDIO_EXE}" STREQUAL "")
    set(ARTPI2_STUDIO_EXE "$ENV{ARTPI2_STUDIO_EXE}")
endif()
if(NOT ARTPI2_STUDIO_EXE)
    set(_bundled_studio "${CMAKE_SOURCE_DIR}/ArtPi2Studio.exe")
    if(EXISTS "${_bundled_studio}" AND NOT IS_DIRECTORY "${_bundled_studio}")
        set(ARTPI2_STUDIO_EXE "${_bundled_studio}")
    endif()
endif()
if(NOT ARTPI2_STUDIO_EXE)
    message(FATAL_ERROR "ART-Pi2 Studio was not found. Use the GUI or set -DARTPI2_STUDIO_EXE=<project-root>/ArtPi2Studio.exe.")
endif()
get_filename_component(ARTPI2_STUDIO_EXE "${ARTPI2_STUDIO_EXE}" ABSOLUTE
    BASE_DIR "${CMAKE_SOURCE_DIR}")
if(NOT EXISTS "${ARTPI2_STUDIO_EXE}" OR IS_DIRECTORY "${ARTPI2_STUDIO_EXE}")
    message(FATAL_ERROR "ARTPI2_STUDIO_EXE does not name an existing file: ${ARTPI2_STUDIO_EXE}")
endif()

ExternalProject_Add(artpi2-usbdiag_Appli
    SOURCE_DIR "${CMAKE_SOURCE_DIR}/Appli"
    BINARY_DIR "${ARTPI2_APPLI_BINARY_DIR}"
    PREFIX "${_project_build_root}/_external/Appli"
    CMAKE_GENERATOR "${CMAKE_GENERATOR}"
    CMAKE_ARGS
        "-DCMAKE_TOOLCHAIN_FILE:FILEPATH=${CMAKE_TOOLCHAIN_FILE}"
        "-DCMAKE_BUILD_TYPE:STRING=${CMAKE_BUILD_TYPE}"
        "-DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=ON"
        "-DRTTHREAD_PROFILE:STRING=${RTTHREAD_PROFILE}"
        "-DRTTHREAD_ALLOCATOR:STRING=${RTTHREAD_ALLOCATOR}"
        "-DLVGL_PROFILE:STRING=${LVGL_PROFILE}"
        "-DLVGL_DRAW_DMA2D:BOOL=${LVGL_DRAW_DMA2D}"
        "-DARTPI2_STUDIO_EXE:FILEPATH=${ARTPI2_STUDIO_EXE}"
        "-DWIFI_HOST_DRIVER_ROOT:PATH=${WIFI_HOST_DRIVER_ROOT}"
    BUILD_ALWAYS TRUE
    INSTALL_COMMAND ""
    USES_TERMINAL_BUILD TRUE
)

ExternalProject_Add(artpi2-usbdiag_Boot
    SOURCE_DIR "${CMAKE_SOURCE_DIR}/Boot"
    BINARY_DIR "${ARTPI2_BOOT_BINARY_DIR}"
    PREFIX "${_project_build_root}/_external/Boot"
    CMAKE_GENERATOR "${CMAKE_GENERATOR}"
    CMAKE_ARGS
        "-DCMAKE_TOOLCHAIN_FILE:FILEPATH=${CMAKE_TOOLCHAIN_FILE}"
        "-DCMAKE_BUILD_TYPE:STRING=${CMAKE_BUILD_TYPE}"
        "-DCMAKE_EXPORT_COMPILE_COMMANDS:BOOL=ON"
    BUILD_ALWAYS TRUE
    BUILD_BYPRODUCTS "${_boot_elf}"
    INSTALL_COMMAND ""
    USES_TERMINAL_BUILD TRUE
)

set(_boot_sha256 "${_boot_elf}.sha256")
add_custom_command(
    OUTPUT "${_boot_sha256}"
    COMMAND "${CMAKE_COMMAND}"
        -DINPUT_FILE=${_boot_elf}
        -DOUTPUT_FILE=${_boot_sha256}
        -P "${CMAKE_SOURCE_DIR}/cmake/write_sha256.cmake"
    DEPENDS artpi2-usbdiag_Boot "${_boot_elf}"
        "${CMAKE_SOURCE_DIR}/cmake/write_sha256.cmake"
    VERBATIM
    COMMENT "Hashing the Boot image")
add_custom_target(artpi2-usbdiag_Boot_hash ALL DEPENDS "${_boot_sha256}")
