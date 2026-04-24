cmake_minimum_required(VERSION 3.16)
project(unio-ui VERSION 0.1.0 LANGUAGES CXX)

# Matches unio-pipe: C++20 project-wide, strict warnings on our
# own target only (third-party sources get their own flags).
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static MSVC CRT on Windows, same rationale as unio-pipe:
# single-file ship, no VCRuntime redist alongside the binary.
if(WIN32)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

set(UNIO_UI_CXX_FLAGS "")
if(NOT MSVC)
    set(UNIO_UI_CXX_FLAGS
        -Wall -Wextra -Wpedantic -Werror -Wno-unused-parameter)
else()
    set(UNIO_UI_CXX_FLAGS /W4 /WX /wd4324 /wd4201)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

set(UNIO_UI_SOURCES
    src/main.cpp
)

if(WIN32)
    list(APPEND UNIO_UI_SOURCES
        src/platform/win32/app_win32.cpp
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND UNIO_UI_SOURCES
        src/platform/x11/app_x11.cpp
    )
else()
    message(FATAL_ERROR
        "unio-ui: unsupported platform ${CMAKE_SYSTEM_NAME}. "
        "Windows + Linux are supported today; macOS is planned "
        "(see ARCHITECTURE.md §5).")
endif()

if(WIN32)
    # WIN32 target attribute → WinMain entry point, no console
    # window pops up alongside the GUI.
    add_executable(unio-ui WIN32 ${UNIO_UI_SOURCES})
else()
    add_executable(unio-ui ${UNIO_UI_SOURCES})
endif()

target_compile_options(unio-ui PRIVATE ${UNIO_UI_CXX_FLAGS})
target_include_directories(unio-ui PRIVATE src)

if(WIN32)
    target_link_libraries(unio-ui PRIVATE
        d3d11 dxgi dxguid
        user32 gdi32 kernel32
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Raw X11 + EGL + desktop GL. No GLFW, no SDL — see
    # ARCHITECTURE.md §5 decision 1a.
    find_package(X11 REQUIRED)
    target_link_libraries(unio-ui PRIVATE
        ${X11_LIBRARIES}
        EGL
        GL
    )
    target_include_directories(unio-ui PRIVATE ${X11_INCLUDE_DIR})
endif()

# Strip in release builds on Linux — matches unio-pipe's ship
# story. Windows binaries keep their symbols (the msvc-wine
# pipeline doesn't strip; symbols stay for future crash-dump
# work).
if(CMAKE_BUILD_TYPE STREQUAL "Release" AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_custom_command(TARGET unio-ui POST_BUILD
        COMMAND ${CMAKE_STRIP} $<TARGET_FILE:unio-ui>)
endif()
