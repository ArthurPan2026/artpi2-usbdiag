if(NOT EXISTS "${ELF}" OR NOT EXISTS "${BIN}" OR NOT EXISTS "${READELF}")
    message(FATAL_ERROR "ELF, XIP image, or readelf is missing for watchdog journal bounds check")
endif()

set(xip_begin 1879048192) # 0x70000000
set(xip_limit 0x73FAF000) # 64-KiB trace sidecar plus the 260-KiB watchdog journal
execute_process(COMMAND "${READELF}" -W -l "${ELF}"
    RESULT_VARIABLE status OUTPUT_VARIABLE headers ERROR_VARIABLE errors)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "readelf failed while checking XIP journal bounds: ${errors}")
endif()

string(REPLACE "\n" ";" lines "${headers}")
set(loads 0)
foreach(line IN LISTS lines)
    if(line MATCHES "^[ \t]*LOAD[ \t]+0x[0-9a-fA-F]+[ \t]+(0x[0-9a-fA-F]+)[ \t]+(0x[0-9a-fA-F]+)[ \t]+(0x[0-9a-fA-F]+)[ \t]+(0x[0-9a-fA-F]+)")
        math(EXPR virtual "${CMAKE_MATCH_1}")
        math(EXPR physical "${CMAKE_MATCH_2}")
        math(EXPR physical_end "${CMAKE_MATCH_2}+${CMAKE_MATCH_3}")
        math(EXPR virtual_end "${CMAKE_MATCH_1}+${CMAKE_MATCH_4}")
        math(EXPR loads "${loads}+1")
        if((physical LESS 1946157056 AND physical_end GREATER xip_limit) OR
           (virtual LESS 1946157056 AND virtual_end GREATER xip_limit))
            message(FATAL_ERROR "ELF LOAD overlaps reserved watchdog trace/journal tail at 0x73FAF000: ${line}")
        endif()
    endif()
endforeach()
if(loads EQUAL 0)
    message(FATAL_ERROR "No ELF LOAD segments parsed; cannot certify watchdog log boundary")
endif()

file(SIZE "${BIN}" bin_size)
math(EXPR max_image_size "${xip_limit}-${xip_begin}")
if(bin_size GREATER max_image_size)
    message(FATAL_ERROR "XIP binary (${bin_size} bytes) exceeds reserved boundary (${max_image_size} bytes)")
endif()
message(STATUS "${loads} ELF LOAD segments and ${bin_size}-byte XIP image stay below the 64-KiB trace sidecar and 260-KiB watchdog journal")
