cmake_minimum_required(VERSION 3.22)
project(wifi_profile_manager C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_library(wifi_profile_manager src/wifi_profile_manager.c)
target_include_directories(wifi_profile_manager PUBLIC include)

enable_testing()
add_executable(wifi_profile_manager_test tests/test_core.c)
target_link_libraries(wifi_profile_manager_test PRIVATE wifi_profile_manager)

option(WPM_BUILD_NV_JOURNAL "Build the optional two-sector NV journal" ON)
if(WPM_BUILD_NV_JOURNAL)
    add_library(wifi_profile_manager_nv src/wpm_nv_journal.c)
    add_library(wifi_profile_manager::nv ALIAS wifi_profile_manager_nv)
    target_include_directories(wifi_profile_manager_nv PUBLIC include)
    add_executable(wifi_profile_manager_nv_test tests/test_nv_journal.c)
    target_link_libraries(wifi_profile_manager_nv_test
        PRIVATE wifi_profile_manager_nv)
    add_test(NAME wifi_profile_manager_nv_journal
        COMMAND wifi_profile_manager_nv_test)
endif()

if(MSVC)
    target_compile_options(wifi_profile_manager PRIVATE /W4)
    target_compile_options(wifi_profile_manager_test PRIVATE /W4)
    if(WPM_BUILD_NV_JOURNAL)
        target_compile_options(wifi_profile_manager_nv PRIVATE /W4)
        target_compile_options(wifi_profile_manager_nv_test PRIVATE /W4)
    endif()
else()
    target_compile_options(wifi_profile_manager PRIVATE -Wall -Wextra -Werror -pedantic)
    target_compile_options(wifi_profile_manager_test PRIVATE -Wall -Wextra -Werror -pedantic)
    if(WPM_BUILD_NV_JOURNAL)
        target_compile_options(wifi_profile_manager_nv PRIVATE
            -Wall -Wextra -Werror -pedantic)
        target_compile_options(wifi_profile_manager_nv_test PRIVATE
            -Wall -Wextra -Werror -pedantic)
    endif()
endif()
add_test(NAME wifi_profile_manager_core COMMAND wifi_profile_manager_test)

# The implementation is C99, but its public header is also usable by C++ apps.
include(CheckLanguage)
check_language(CXX)
if(CMAKE_CXX_COMPILER)
    enable_language(CXX)
    add_executable(wifi_profile_manager_cpp_consumer_test
        tests/test_cpp_consumer.cpp)
    target_link_libraries(wifi_profile_manager_cpp_consumer_test
        PRIVATE wifi_profile_manager)
    set_target_properties(wifi_profile_manager_cpp_consumer_test PROPERTIES
        CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(wifi_profile_manager_cpp_consumer_test PRIVATE /W4)
    else()
        target_compile_options(wifi_profile_manager_cpp_consumer_test PRIVATE
            -Wall -Wextra -Werror -pedantic)
    endif()
    add_test(NAME wifi_profile_manager_cpp_consumer
        COMMAND wifi_profile_manager_cpp_consumer_test)
endif()
