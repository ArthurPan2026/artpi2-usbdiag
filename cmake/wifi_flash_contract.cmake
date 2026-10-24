# Project-owned protection; generated CubeMX files do not allocate WiFi NV.
function(work_protect_wifi_flash target)
    get_filename_component(repo "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
    if(NOT CMAKE_READELF)
        find_program(CMAKE_READELF arm-none-eabi-readelf REQUIRED)
    endif()
    set(layout "${repo}/tests/wifi_nv/check_wifi_nv_layout.cmake")
    set(segments "${repo}/cmake/check_flash_loads.cmake")
    add_custom_target(work_wifi_nv_layout ALL
        COMMAND "${CMAKE_COMMAND}" "-DREPO_ROOT=${repo}" -P "${layout}"
        DEPENDS "${layout}" "${repo}/Appli/User/wifi/include/wifi_nv_layout.h"
            "${repo}/Boot/stm32h7r7xx_flash.ld"
        COMMENT "Checking reserved WiFi Flash and Boot bounds" VERBATIM)
    add_dependencies(${target} work_wifi_nv_layout)
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${segments}")
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" "-DELF=$<TARGET_FILE:${target}>"
            "-DREADELF=${CMAKE_READELF}" -P "${segments}"
        COMMENT "Rejecting ELF load segments overlapping reserved WiFi Flash" VERBATIM)
endfunction()
