cmake_minimum_required(VERSION 3.16)
project(unio-pipe CXX)

# Day-1 scope of PR 6: compile, open a UDS control socket (Linux)
# or named pipe (Windows), accept JSON commands. No GPU, no
# encoder, no transport yet — those are the following weeks'
# work.  This file's job is to make the directory actually build.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Strict warnings. The helper is on the hot path; we want every
# uninitialised value and every narrowing conversion flagged at
# build time rather than inferred from a ten-minute session dump.
if(NOT MSVC)
    add_compile_options(-Wall -Wextra -Wpedantic -Werror
                        -Wno-unused-parameter)
else()
    add_compile_options(/W4 /WX)
endif()

add_executable(unio-pipe
    src/main.cpp
    src/control_socket.cpp
    src/caps.cpp
    src/json_mini.cpp
)

target_include_directories(unio-pipe PRIVATE include)

# Linux: needs pthread for the control-socket accept thread.
# Windows: WinSock (for the named-pipe path) — not yet wired.
if(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(unio-pipe PRIVATE Threads::Threads)
endif()

if(WIN32)
    target_link_libraries(unio-pipe PRIVATE ws2_32)
endif()
