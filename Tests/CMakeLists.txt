# Tests of com.recomp.netplay (no engine needed).
#
#   cmake -S Tests -B build/tests && cmake --build build/tests
#   build/tests/netplay_selftest [loss_percent]      host + 2 clients in one process
#
# With an N64 game library (the recomp runtime's embedding API, e.g. ssb64_recomp):
#   -DNETPLAY_N64_LIB=<path to the static library> -DNETPLAY_N64_INCLUDE=<com.recomp.n64/Native/include>
# also builds netplay_n64, a headless netplay runner (two of them, on one or two machines, play
# one game; see README.md).
cmake_minimum_required(VERSION 3.16)
project(netplay_tests C)

set(NETPLAY_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/../Source")
add_library(netplay_core STATIC "${NETPLAY_SOURCE}/netplay.c")
target_include_directories(netplay_core PUBLIC "${NETPLAY_SOURCE}")
if(WIN32)
    target_link_libraries(netplay_core PUBLIC ws2_32)
    target_compile_definitions(netplay_core PUBLIC _CRT_SECURE_NO_WARNINGS)
else()
    find_package(Threads REQUIRED) # netplay_set_background
    target_link_libraries(netplay_core PUBLIC Threads::Threads)
endif()

add_executable(netplay_selftest netplay_selftest.c)
target_link_libraries(netplay_selftest PRIVATE netplay_core)
add_executable(netplay_lan netplay_lan.c) # lists the sessions on the LAN
target_link_libraries(netplay_lan PRIVATE netplay_core)

set(NETPLAY_N64_LIB "" CACHE FILEPATH "N64 game library (recomp or decomp build) for netplay_n64")
set(NETPLAY_N64_INCLUDE "" CACHE PATH "com.recomp.n64/Native/include")
if(NETPLAY_N64_LIB)
    enable_language(CXX) # the game libraries hold a little C++ (their section tables)
    add_executable(netplay_n64 netplay_n64.c)
    target_include_directories(netplay_n64 PRIVATE "${NETPLAY_N64_INCLUDE}")
    target_link_libraries(netplay_n64 PRIVATE netplay_core "${NETPLAY_N64_LIB}")
    set_target_properties(netplay_n64 PROPERTIES LINKER_LANGUAGE CXX)

    # the game-player side (NetplaySession + NetplayN64.h), as an editor player node uses it
    add_executable(netplay_player netplay_player.cpp "${NETPLAY_SOURCE}/NetplaySession.cpp")
    target_include_directories(netplay_player PRIVATE "${NETPLAY_N64_INCLUDE}")
    target_link_libraries(netplay_player PRIVATE netplay_core "${NETPLAY_N64_LIB}")
    set_target_properties(netplay_player PROPERTIES CXX_STANDARD 17)
    foreach(t netplay_n64 netplay_player)
        if(WIN32)
            target_link_libraries(${t} PRIVATE winmm dbghelp)
        else()
            target_link_libraries(${t} PRIVATE pthread m)
        endif()
    endforeach()
endif()
