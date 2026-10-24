cmake_minimum_required(VERSION 3.16)
project(usb_enum_diag VERSION 0.1.0 LANGUAGES C)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/usbdiag_version.h"
    USBDIAG_VERSION_HEADER)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/package.json"
    USBDIAG_PACKAGE_METADATA)
if(NOT USBDIAG_VERSION_HEADER MATCHES
        "USB_ENUM_DIAG_VERSION[ \t]+\\\"${PROJECT_VERSION}\\\"")
    message(FATAL_ERROR "usbdiag_version.h does not match project version ${PROJECT_VERSION}")
endif()
if(NOT USBDIAG_PACKAGE_METADATA MATCHES
        "\\\"version\\\"[ \t]*:[ \t]*\\\"v?${PROJECT_VERSION}\\\"")
    message(FATAL_ERROR "package.json has no fixed ${PROJECT_VERSION} release entry")
endif()

add_library(usbdiag_core STATIC
    src/usbdiag_core.c
    src/usbdiag_session.c)
add_library(usb_enum_diag::core ALIAS usbdiag_core)
target_include_directories(usbdiag_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include/usb_enum_diag>)
set_target_properties(usbdiag_core PROPERTIES EXPORT_NAME core)
set_target_properties(usbdiag_core PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
configure_package_config_file(
    cmake/usb_enum_diagConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/usb_enum_diagConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/usb_enum_diag")
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/usb_enum_diagConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion)

install(TARGETS usbdiag_core EXPORT usb_enum_diagTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(EXPORT usb_enum_diagTargets
    NAMESPACE usb_enum_diag::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/usb_enum_diag")
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/usb_enum_diagConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/usb_enum_diagConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/usb_enum_diag")
install(FILES src/usbdiag_core.h src/usbdiag_session.h src/usbdiag_version.h
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/usb_enum_diag")

option(USBDIAG_BUILD_TESTS "Build the deterministic host tests" ON)
if(USBDIAG_BUILD_TESTS)
    enable_testing()
    add_executable(usbdiag_core_test tests/test_core.c)
    target_link_libraries(usbdiag_core_test PRIVATE usbdiag_core)
    set_target_properties(usbdiag_core_test PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO)
    add_test(NAME usbdiag_core_test COMMAND usbdiag_core_test)

    add_executable(usbdiag_session_test tests/test_session.c)
    target_link_libraries(usbdiag_session_test PRIVATE usbdiag_core)
    set_target_properties(usbdiag_session_test PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO)
    add_test(NAME usbdiag_session_test COMMAND usbdiag_session_test)

    foreach(test_target IN ITEMS
            usbdiag_core_test usbdiag_session_test)
        if(MSVC)
            target_compile_options(${test_target} PRIVATE /UNDEBUG)
        else()
            target_compile_options(${test_target} PRIVATE -UNDEBUG)
        endif()
    endforeach()

    add_executable(usbdiag_browser_rules tests/emit_browser_rules.c)
    target_link_libraries(usbdiag_browser_rules PRIVATE usbdiag_core)
    set_target_properties(usbdiag_browser_rules PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO)
endif()
