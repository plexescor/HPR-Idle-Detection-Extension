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
    pkg_check_modules(GIO REQUIRED gio-2.0)
    target_include_directories(HPR_Idle_Detection_Extension PRIVATE ${GIO_INCLUDE_DIRS})
    target_link_libraries(HPR_Idle_Detection_Extension PRIVATE ${GIO_LIBRARIES})
endif()