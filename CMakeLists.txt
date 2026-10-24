cmake_minimum_required(VERSION 3.22)
include(ExternalProject)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# In order to use CMake for cross-compiling
set(CMAKE_CXX_COMPILER_FORCED "true")

# Set the project name
set(CMAKE_PROJECT_NAME artpi2-usbdiag)

# Forward the RT-Thread profile to the CubeMX-generated Appli
# ExternalProject.  The default remains the compatibility profile so a plain
# root build does not silently enable diagnostic overhead.
set(RTTHREAD_PROFILE "compat" CACHE STRING
    "RT-Thread profile forwarded to Appli")
set_property(CACHE RTTHREAD_PROFILE PROPERTY STRINGS compat performance release)
set(RTTHREAD_ALLOCATOR "memheap" CACHE STRING
    "RT-Thread allocator forwarded to Appli")
set_property(CACHE RTTHREAD_ALLOCATOR PROPERTY STRINGS smallmem memheap slab)

set(LVGL_PROFILE "performance" CACHE STRING
    "LVGL profile forwarded to Appli")
set_property(CACHE LVGL_PROFILE PROPERTY STRINGS performance release)
option(LVGL_DRAW_DMA2D
    "Register the LVGL 9.6 DMA2D draw unit through the H7RS broker"
    ON)
set(WIFI_HOST_DRIVER_ROOT
    "${CMAKE_SOURCE_DIR}/Appli/ThirdParty/wifi-host-driver/WiFi_Host_Driver"
    CACHE PATH "Locally obtained Infineon WHD 3.3.3.26314 package root")

# Core project settings
project(${CMAKE_PROJECT_NAME})

# Project-owned contexts keep each preset's Boot and Appli artifacts isolated
# from source and CubeMX's generated files.
include("cmake/project_contexts.cmake")
