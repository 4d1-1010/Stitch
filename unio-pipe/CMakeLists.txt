cmake_minimum_required(VERSION 3.16)
project(unio-pipe CXX)

# Day-1 scope of PR 6: compile, open a UDS control socket (Linux)
# or named pipe (Windows), accept JSON commands. No GPU, no
# encoder, no transport yet — those are the following weeks'
# work.  This file's job is to make the directory actually build.

# C++17 project-wide. C++/WinRT in VS 2019's header set still
# pulls <experimental/coroutine>, which the MSVC STL refuses
# under C++20. We don't use any C++20-only features in our own
# code, so bumping the standard isn't useful. If we ever need
# concepts / std::span / etc. we can swap to C++20 + /await or
# update the Windows SDK.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Strict warnings. The helper is on the hot path; we want every
# uninitialised value and every narrowing conversion flagged at
# build time rather than inferred from a ten-minute session dump.
if(NOT MSVC)
    add_compile_options(-Wall -Wextra -Wpedantic -Werror
                        -Wno-unused-parameter)
else()
    # /WX == -Werror; the warnings below are all intentional design
    # choices that MSVC flags but GCC/Clang don't:
    #   C4324 — "struct padded due to alignment specifier". That's
    #           the whole point of alignas(64) on SpscRing's
    #           head_/tail_ — we want them on separate cache lines.
    #   C4996 — "deprecated: consider fopen_s/_dupenv_s". The POSIX
    #           equivalents are fine for our use case and keep the
    #           code portable across Linux + Windows without ifdefs.
    # /await enables pre-C++20 coroutine support (the mode
    # C++/WinRT's VS-2019 base.h is written against). Under C++17
    # this is the sanctioned way; under C++20 we'd need
    # /await:strict, but cppwinrt hasn't caught up there.
    add_compile_options(/W4 /WX /wd4324 /await)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

set(UNIO_PIPE_SOURCES
    src/main.cpp
    src/control_socket.cpp
    src/caps.cpp
    src/json_mini.cpp
    src/stream_manager.cpp
    src/h264_parse.cpp
    # Per-vendor encoder / decoder / presenter sources all have
    # stubs at the bottom that return nullptr on the "other" OS,
    # so stream_manager's call sites compile + link on every
    # platform without cluttering the caller with #ifdefs. The
    # real implementations are OS-gated inside the files.
    src/encoder_vaapi.cpp
    src/decoder_vaapi.cpp
    src/decoder_d3d11va.cpp
    src/presenter_egl_x11.cpp
    src/presenter_dxgi.cpp
    src/quic_transport.cpp
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND UNIO_PIPE_SOURCES src/capture_xcomposite.cpp)
endif()

if(WIN32)
    list(APPEND UNIO_PIPE_SOURCES src/capture_wgc.cpp)
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
    # doesn't add a new runtime dep on Linux.
    find_package(OpenSSL REQUIRED)
    target_link_libraries(unio-pipe PRIVATE
        OpenSSL::Crypto OpenSSL::SSL)

    # pkg-config-driven probes for the Linux-only media deps.
    find_package(PkgConfig REQUIRED)

    # EGL + GLES for the X11 presenter.
    pkg_check_modules(EGL REQUIRED IMPORTED_TARGET egl glesv2)
    target_link_libraries(unio-pipe PRIVATE PkgConfig::EGL)

    # libva (MIT) — VA-API encode/decode. Two components: the
    # core dispatcher and the DRM backend that grabs a device
    # from /dev/dri/renderD128.
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
    # Networking (ws2_32) + named pipes (advapi32 for default
    # security descriptor). The MSVC CRT already pulls kernel32.
    # crypt32 + bcrypt are needed by libcrypto's Windows backends.
    # gdi32 is pulled in by CreateWindowEx; d3d11 / dxgi are
    # linked inline via #pragma comment(lib, ...) from the
    # decoder / presenter sources.
    target_link_libraries(unio-pipe PRIVATE
        ws2_32 advapi32 crypt32 bcrypt user32 gdi32)
    # UNICODE is expected by WinRT / D3D11; define it for future
    # capture_wgc + presenter_dxgi sources and so WCHAR-based
    # Windows headers compile cleanly even in our MBCS-ish paths.
    target_compile_definitions(unio-pipe PRIVATE
        UNICODE _UNICODE
        WIN32_LEAN_AND_MEAN NOMINMAX)
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
    #
    # TLS backend: openssl3 on both OSes. Schannel on Windows
    # sounds attractive (native, no extra DLL) but Windows 10
    # 22H2 ships with TLS 1.3 disabled by default in Schannel,
    # and QUIC requires TLS 1.3. Rather than forcing a registry
    # change at install time, we bundle OpenSSL and get the same
    # TLS code path on every platform. msquic produces an
    # ABI-compatible DLL either way — our code doesn't care.
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

# Windows has no system OpenSSL, so quic_transport.cpp's
# self-signed cert generator reuses the OpenSSL that msquic built
# under our feet (QUIC_TLS=openssl3). The build tree path is
# stable across msquic versions as long as FetchContent populates
# it under ${msquic_BINARY_DIR}/../opensslquic-build.
if(WIN32 AND NOT MSQUIC_ROOT)
    set(_unio_openssl_dir
        "${msquic_BINARY_DIR}/../opensslquic-build/openssl3/release")
    target_include_directories(unio-pipe PRIVATE
        "${_unio_openssl_dir}/include")
    target_link_libraries(unio-pipe PRIVATE
        "${_unio_openssl_dir}/lib/libcrypto.lib"
        "${_unio_openssl_dir}/lib/libssl.lib")
endif()

# Windows: ship msquic.dll next to unio-pipe.exe so the loader
# finds it without a custom PATH. Unix doesn't need this — the
# shared lib ends up in the build tree too, but we invoke the
# binary with LD_LIBRARY_PATH pointing at the install tree (see
# packaging/) or link statically in release builds.
if(WIN32 AND TARGET msquic)
    add_custom_command(TARGET unio-pipe POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:msquic>
            $<TARGET_FILE_DIR:unio-pipe>)
endif()
