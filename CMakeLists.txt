cmake_minimum_required(VERSION 3.15)
project(HPR_Idle_Detection_Extension LANGUAGES CXX C)

# Force C++23 standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_C_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Lua static lib
file(GLOB_RECURSE LUA_SOURCES "external/lua/*.c")
list(FILTER LUA_SOURCES EXCLUDE REGEX ".*(lua|luac)\\.c$")
add_library(lua_static STATIC ${LUA_SOURCES})

# Define the shared library and its source files
add_library(HPR_Idle_Detection_Extension SHARED 
    src/library.cpp
)
set_target_properties(HPR_Idle_Detection_Extension PROPERTIES PREFIX "")

if (MSVC)
    target_compile_options(HPR_Idle_Detection_Extension PRIVATE /MP $<$<CONFIG:Release,MinSizeRel>:/O1 /GL /Gy /Gw>)
    target_link_options(HPR_Idle_Detection_Extension PRIVATE $<$<CONFIG:Release,MinSizeRel>:/LTCG /OPT:REF /OPT:ICF>)
    target_compile_options(lua_static PRIVATE /MP $<$<CONFIG:Release,MinSizeRel>:/O1 /GL /Gy /Gw>)
endif()

# Specify include directories for the library and its consumers
target_include_directories(lua_static PUBLIC external/lua/)
target_include_directories(HPR_Idle_Detection_Extension PUBLIC external/lua/)
target_include_directories(HPR_Idle_Detection_Extension PUBLIC 
    include
)
target_include_directories(HPR_Idle_Detection_Extension PRIVATE external/sol2/)

target_link_libraries(HPR_Idle_Detection_Extension PRIVATE
    lua_static
)

if (UNIX AND NOT APPLE)
    find_package(PkgConfig REQUIRED)

    # ── GIO / GLib (GNOME D-Bus idle detection) ───────────────────────────
    pkg_check_modules(GIO REQUIRED gio-2.0)
    target_include_directories(HPR_Idle_Detection_Extension PRIVATE ${GIO_INCLUDE_DIRS})
    target_link_libraries(HPR_Idle_Detection_Extension PRIVATE ${GIO_LIBRARIES})

    # ── Wayland idle detection (ext-idle-notify-v1) ───────────────────────
    pkg_check_modules(WAYLAND_CLIENT wayland-client)
    pkg_check_modules(WAYLAND_PROTOCOLS wayland-protocols)

    if (WAYLAND_CLIENT_FOUND AND WAYLAND_PROTOCOLS_FOUND)
        # Locate wayland-scanner executable
        find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

        # Locate the ext-idle-notify-v1 XML from the system protocols dir
        pkg_get_variable(WL_PROTOCOLS_DIR wayland-protocols pkgdatadir)
        set(EXT_IDLE_XML "${WL_PROTOCOLS_DIR}/staging/ext-idle-notify/ext-idle-notify-v1.xml")

        # Output paths for generated files (placed in the build directory)
        set(EXT_IDLE_HEADER "${CMAKE_CURRENT_BINARY_DIR}/ext-idle-notify-v1-client-protocol.h")
        set(EXT_IDLE_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/ext-idle-notify-v1-protocol.c")

        # Generate the client header
        add_custom_command(
            OUTPUT  "${EXT_IDLE_HEADER}"
            COMMAND "${WAYLAND_SCANNER}" client-header "${EXT_IDLE_XML}" "${EXT_IDLE_HEADER}"
            DEPENDS "${EXT_IDLE_XML}"
            COMMENT "Generating ext-idle-notify-v1 client header"
            VERBATIM
        )

        # Generate the protocol glue source
        add_custom_command(
            OUTPUT  "${EXT_IDLE_SOURCE}"
            COMMAND "${WAYLAND_SCANNER}" private-code "${EXT_IDLE_XML}" "${EXT_IDLE_SOURCE}"
            DEPENDS "${EXT_IDLE_XML}"
            COMMENT "Generating ext-idle-notify-v1 protocol source"
            VERBATIM
        )

        # Pull the generated files into the shared library build
        target_sources(HPR_Idle_Detection_Extension PRIVATE
            "${EXT_IDLE_SOURCE}"
            "${EXT_IDLE_HEADER}"
        )

        # Make the generated header findable from library.cpp
        target_include_directories(HPR_Idle_Detection_Extension PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

        target_include_directories(HPR_Idle_Detection_Extension PRIVATE ${WAYLAND_CLIENT_INCLUDE_DIRS})
        target_link_libraries(HPR_Idle_Detection_Extension PRIVATE ${WAYLAND_CLIENT_LIBRARIES})

        # Signal library.cpp to compile the Wayland path
        target_compile_definitions(HPR_Idle_Detection_Extension PRIVATE HAVE_WAYLAND_IDLE)

        message(STATUS "Wayland ext-idle-notify-v1 support enabled")
    else()
        message(STATUS "Wayland client or protocols not found — Wayland idle detection disabled")
    endif()
endif()


add_custom_command(TARGET HPR_Idle_Detection_Extension POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/src/idle_detection.lua"
        "$<TARGET_FILE_DIR:HPR_Idle_Detection_Extension>/idle_detection.lua"
)

if(WIN32)
    set(HPR_EXTENSION_DIR "$ENV{APPDATA}/HPR/HPR_Config/extensions/HPR_Idle_Detection_Extension")
else()
    set(HPR_EXTENSION_DIR "$ENV{HOME}/.config/HPR/extensions/HPR_Idle_Detection_Extension")
endif()

add_custom_command(TARGET HPR_Idle_Detection_Extension POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${HPR_EXTENSION_DIR}"

    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:HPR_Idle_Detection_Extension>"
        "${HPR_EXTENSION_DIR}/"

    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE_DIR:HPR_Idle_Detection_Extension>/idle_detection.lua"
        "${HPR_EXTENSION_DIR}/"

    COMMAND_EXPAND_LISTS
)