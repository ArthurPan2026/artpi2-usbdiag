# User-owned firmware modules contribute source files and public include paths
# to the existing Appli executable. They remain one firmware target.
set(APP_USER_SOURCES)
set(APP_USER_INCLUDE_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/config/include"
)

foreach(user_module IN ITEMS platform runtime display diagnostics usb wifi web ui)
    set(USER_MODULE_SOURCES)
    set(USER_MODULE_INCLUDE_DIRS)
    add_subdirectory("${user_module}")
    list(APPEND APP_USER_SOURCES ${USER_MODULE_SOURCES})
    list(APPEND APP_USER_INCLUDE_DIRS ${USER_MODULE_INCLUDE_DIRS})
endforeach()

list(REMOVE_DUPLICATES APP_USER_SOURCES)
list(REMOVE_DUPLICATES APP_USER_INCLUDE_DIRS)
set(APP_USER_SOURCES "${APP_USER_SOURCES}" PARENT_SCOPE)
set(APP_USER_INCLUDE_DIRS "${APP_USER_INCLUDE_DIRS}" PARENT_SCOPE)
