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

set(UNIO_PIPE_SOURCES
    src/main.cpp
    src/control_socket.cpp
    src/caps.cpp
    src/json_mini.cpp
    src/stream_manager.cpp
    src/encoder_vaapi.cpp
    src/decoder_vaapi.cpp
    src/quic_transport.cpp
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND UNIO_PIPE_SOURCES src/capture_xcomposite.cpp)
endif()

add_executable(unio-pipe ${UNIO_PIPE_SOURCES})

target_include_directories(unio-pipe PRIVATE include)

# Linux: needs pthread + Xlib + Xext (for MIT-SHM) for the
# XComposite capture.
if(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(unio-pipe PRIVATE Threads::Threads)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(X11 REQUIRED)
    target_link_libraries(unio-pipe PRIVATE
        ${X11_LIBRARIES} ${X11_Xext_LIB})
    target_include_directories(unio-pipe PRIVATE ${X11_INCLUDE_DIR})

    # OpenSSL — used by quic_transport.cpp to generate an
    # ephemeral self-signed cert for the inbound QUIC listener.
    # msquic already links against OpenSSL internally so this
    # doesn't add a new runtime dep.
    find_package(OpenSSL REQUIRED)
    target_link_libraries(unio-pipe PRIVATE
        OpenSSL::Crypto OpenSSL::SSL)

    # libva (MIT) — VA-API encode/decode. Two components: the
    # core dispatcher and the DRM backend that grabs a device
    # from /dev/dri/renderD128.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(VAAPI IMPORTED_TARGET libva libva-drm)
    if(VAAPI_FOUND)
        target_link_libraries(unio-pipe PRIVATE
            PkgConfig::VAAPI)
        target_compile_definitions(unio-pipe PRIVATE
            UNIO_PIPE_HAS_VAAPI=1)
    else()
        message(WARNING
            "libva / libva-drm not found via pkg-config. "
            "VA-API encoder will be a stub. apt install "
            "libva-dev libva-drm-dev to enable.")
    endif()
endif()

if(WIN32)
    target_link_libraries(unio-pipe PRIVATE ws2_32)
endif()

# --------------------------------------------------------------- msquic
#
# QUIC transport. Microsoft's msquic (MIT) — same library on
# Linux and Windows, which is the whole point of picking it. It's
# not in any Linux distro's package manager as of 2026 and
# Microsoft doesn't publish prebuilt release assets any more, so
# we fetch + build from source. Submodules pulled in
# --recurse-submodules --shallow-submodules take ~2 min on a
# cold clone; the minimal library build is ~40s after that.
#
# Users with a pre-built libmsquic (e.g. via a custom vendor
# tarball) can point MSQUIC_ROOT at the tree and we'll skip the
# fetch. That path also lets CI cache the artifact.
include(FetchContent)

if(DEFINED ENV{MSQUIC_ROOT} AND NOT MSQUIC_ROOT)
    # First-time seed from env — subsequent cmake invocations
    # don't re-read the env, so commit the value to the cache
    # alongside any explicit -DMSQUIC_ROOT overrides.
    set(MSQUIC_ROOT "$ENV{MSQUIC_ROOT}" CACHE PATH
        "Path to prebuilt msquic install tree")
endif()
if(MSQUIC_ROOT)
    find_library(MSQUIC_LIBRARY NAMES msquic
        HINTS "${MSQUIC_ROOT}/lib" "${MSQUIC_ROOT}"
        NO_DEFAULT_PATH REQUIRED)
    find_path(MSQUIC_INCLUDE_DIR msquic.h
        HINTS "${MSQUIC_ROOT}/include"
              "${MSQUIC_ROOT}/src/inc"
        NO_DEFAULT_PATH REQUIRED)
    target_link_libraries(unio-pipe PRIVATE "${MSQUIC_LIBRARY}")
    target_include_directories(unio-pipe PRIVATE "${MSQUIC_INCLUDE_DIR}")
    message(STATUS "Using prebuilt msquic at ${MSQUIC_ROOT}")
else()
    # Minimal msquic build flags: no tools, no tests, no perf
    # binaries, no logging subsystem. Shrinks the produced .so
    # from ~11 MB to ~5 MB and drops compile time by roughly half.
    set(QUIC_TLS "openssl3" CACHE STRING "" FORCE)
    set(QUIC_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(QUIC_BUILD_TEST  OFF CACHE BOOL "" FORCE)
    set(QUIC_BUILD_PERF  OFF CACHE BOOL "" FORCE)
    set(QUIC_ENABLE_LOGGING OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        msquic
        GIT_REPOSITORY https://github.com/microsoft/msquic.git
        GIT_TAG        v2.4.5
        GIT_SHALLOW    TRUE
        GIT_SUBMODULES_RECURSE TRUE
    )
    FetchContent_MakeAvailable(msquic)
    target_link_libraries(unio-pipe PRIVATE msquic)
    target_include_directories(unio-pipe PRIVATE
        "${msquic_SOURCE_DIR}/src/inc")
endif()
target_compile_definitions(unio-pipe PRIVATE UNIO_PIPE_HAS_MSQUIC=1)
