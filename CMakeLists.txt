cmake_minimum_required(VERSION 3.15)
project(com.recomp.netplay C CXX)

# The editor builds this addon itself (package.json); this file is for building it by hand on
# Windows, Linux or macOS:
#   cmake -S . -B Build -DPOLYPHASE_PATH=<engine> && cmake --build Build
# The core (Source/netplay.c) needs nothing but the OS's sockets; Tests/ builds it alone.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Polyphase Engine path: -DPOLYPHASE_PATH, $POLYPHASE_PATH, a PolyphaseConfig.cmake or
# Polyphase.sln above this folder, then the usual install locations.
function(_polyphase_find_via_walk OUT_VAR)
    set(_p "${CMAKE_CURRENT_SOURCE_DIR}")
    foreach(_i RANGE 1 6)
        get_filename_component(_p "${_p}" DIRECTORY)
        if(EXISTS "${_p}/PolyphaseConfig.cmake")
            include("${_p}/PolyphaseConfig.cmake")
            if(DEFINED POLYPHASE_PATH AND NOT POLYPHASE_PATH STREQUAL "")
                set(${OUT_VAR} "${POLYPHASE_PATH}" PARENT_SCOPE)
                return()
            endif()
        endif()
        if(EXISTS "${_p}/Polyphase.sln")
            set(${OUT_VAR} "${_p}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${OUT_VAR} "" PARENT_SCOPE)
endfunction()

if(NOT DEFINED POLYPHASE_PATH OR POLYPHASE_PATH STREQUAL "")
    if(DEFINED ENV{POLYPHASE_PATH} AND NOT "$ENV{POLYPHASE_PATH}" STREQUAL "")
        set(POLYPHASE_PATH "$ENV{POLYPHASE_PATH}")
    else()
        _polyphase_find_via_walk(_walk)
        if(NOT _walk STREQUAL "")
            set(POLYPHASE_PATH "${_walk}")
        elseif(WIN32 AND EXISTS "C:/Polyphase/Polyphase.exe")
            set(POLYPHASE_PATH "C:/Polyphase")
        elseif(UNIX AND EXISTS "/opt/Polyphase/PolyphaseEditor")
            set(POLYPHASE_PATH "/opt/Polyphase")
        endif()
    endif()
endif()
if(NOT POLYPHASE_PATH)
    message(FATAL_ERROR "Polyphase engine not found: pass -DPOLYPHASE_PATH=<engine folder>")
endif()
message(STATUS "Polyphase: using POLYPHASE_PATH = ${POLYPHASE_PATH}")

file(GLOB_RECURSE SOURCES "Source/*.cpp" "Source/*.c")
file(GLOB_RECURSE HEADERS "Source/*.h")
add_library(com.recomp.netplay SHARED ${SOURCES} ${HEADERS})

# Engine headers include vulkan/vulkan.h under API_VULKAN.
set(VULKAN_INCLUDE_DIR "")
if(DEFINED ENV{VULKAN_SDK})
    foreach(_inc "$ENV{VULKAN_SDK}/Include" "$ENV{VULKAN_SDK}/include")
        if(NOT VULKAN_INCLUDE_DIR AND EXISTS "${_inc}/vulkan/vulkan.h")
            set(VULKAN_INCLUDE_DIR "${_inc}")
        endif()
    endforeach()
endif()
if(NOT VULKAN_INCLUDE_DIR AND EXISTS "${POLYPHASE_PATH}/External/Vulkan/include/vulkan/vulkan.h")
    set(VULKAN_INCLUDE_DIR "${POLYPHASE_PATH}/External/Vulkan/include")
endif()
if(NOT VULKAN_INCLUDE_DIR)
    find_package(Vulkan QUIET)
    if(Vulkan_FOUND)
        set(VULKAN_INCLUDE_DIR "${Vulkan_INCLUDE_DIRS}")
    endif()
endif()

target_include_directories(com.recomp.netplay PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Source
    ${POLYPHASE_PATH}/Engine/Source
    ${POLYPHASE_PATH}/Engine/Source/Engine
    ${POLYPHASE_PATH}/Engine/Source/Plugins
    ${POLYPHASE_PATH}/External
    ${POLYPHASE_PATH}/External/Lua
    ${POLYPHASE_PATH}/External/glm
    ${POLYPHASE_PATH}/External/Imgui
    ${POLYPHASE_PATH}/External/ImGuizmo
    ${VULKAN_INCLUDE_DIR})

target_compile_definitions(com.recomp.netplay PRIVATE
    OCTAVE_PLUGIN_EXPORT
    EDITOR=1
    LUA_ENABLED=1
    GLM_FORCE_RADIANS
    API_VULKAN=1
    NETPLAY_EXPORTS)

if(WIN32)
    target_compile_definitions(com.recomp.netplay PRIVATE PLATFORM_WINDOWS=1 NOMINMAX _CRT_SECURE_NO_WARNINGS)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(_cfg DebugEditor)
    else()
        set(_cfg ReleaseEditor)
    endif()
    target_link_directories(com.recomp.netplay PRIVATE
        "${POLYPHASE_PATH}" "${POLYPHASE_PATH}/lib"
        "${POLYPHASE_PATH}/Standalone/Build/Windows/x64/${_cfg}"
        "${POLYPHASE_PATH}/External/Lua/Build/Windows/x64/${_cfg}")
    target_link_libraries(com.recomp.netplay PRIVATE Polyphase Lua ws2_32)
elseif(APPLE)
    target_compile_definitions(com.recomp.netplay PRIVATE PLATFORM_MAC=1)
    set_target_properties(com.recomp.netplay PROPERTIES LINK_FLAGS "-undefined dynamic_lookup")
else()
    target_compile_definitions(com.recomp.netplay PRIVATE PLATFORM_LINUX=1)
endif()
