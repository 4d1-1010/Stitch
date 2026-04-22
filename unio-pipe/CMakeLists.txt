cmake_minimum_required(VERSION 3.16)
project(unio-pipe CXX C)

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
    src/capability_probe.cpp
    src/json_mini.cpp
    src/stream_manager.cpp
    src/h264_parse.cpp
    src/latency_log.cpp
    # Per-vendor encoder / decoder / presenter sources all have
    # stubs at the bottom that return nullptr on the "other" OS,
    # so stream_manager's call sites compile + link on every
    # platform without cluttering the caller with #ifdefs. The
    # real implementations are OS-gated inside the files.
    src/encoder_vaapi.cpp
    src/decoder_vaapi.cpp
    src/decoder_d3d11va.cpp
    src/decoder_nvdec.cpp
    src/presenter_egl_x11.cpp
    src/presenter_egl_wayland.cpp
    src/presenter_dxgi.cpp
    src/quic_transport.cpp
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND UNIO_PIPE_SOURCES src/capture_xcomposite.cpp)
    list(APPEND UNIO_PIPE_SOURCES src/capture_pipewire.cpp)
endif()

if(WIN32)
    list(APPEND UNIO_PIPE_SOURCES
        src/capture_wgc.cpp
        src/encoder_nvenc.cpp)
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
        ${X11_LIBRARIES} ${X11_Xext_LIB}
        ${CMAKE_DL_LIBS})  # dlopen for NVDEC runtime probe
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

# libpipewire + SPA — PipeWire capture backend (Wayland).
    pkg_check_modules(PIPEWIRE IMPORTED_TARGET libpipewire-0.3 libspa-0.2)
    if(PIPEWIRE_FOUND)
        target_link_libraries(unio-pipe PRIVATE PkgConfig::PIPEWIRE)
        # Treat PipeWire headers as SYSTEM so their pedantic warnings
        # don't propagate to our own code (restores -Wpedantic safety).
        get_target_property(PW_INC PkgConfig::PIPEWIRE
                            INTERFACE_INCLUDE_DIRECTORIES)
        if(PW_INC)
            target_include_directories(unio-pipe SYSTEM PRIVATE ${PW_INC})
        endif()
        target_compile_definitions(unio-pipe PRIVATE
            UNIO_PIPE_HAS_PIPEWIRE=1)
    else()
        message(WARNING
            "libpipewire / libspa not found via pkg-config. "
            "PipeWire capture will be a stub. apt install "
            "libpipewire-0.3-dev libspa-0.2-dev to enable.")
    endif()

    # libdbus-1 — D-Bus API for xdg-desktop-portal ScreenCast calls.
    # Dual-licensed AFL-2.1 OR GPL-2.0+; we take the AFL-2.1 side
    # which is commercial-safe. sdbus-c++ (LGPL-2.1) was rejected
    # for the commercial installer posture (PR 32 review).
    #
    # Optional: X11-only minimal distros may not have libdbus-1-dev.
    # When absent, the capture path refuses Wayland at runtime
    # instead of failing at configure time.
    pkg_check_modules(DBUS QUIET IMPORTED_TARGET dbus-1)
    if(DBUS_FOUND)
        target_link_libraries(unio-pipe PRIVATE PkgConfig::DBUS)
        target_compile_definitions(unio-pipe PRIVATE
            UNIO_PIPE_HAS_DBUS=1)
    else()
        message(WARNING
            "libdbus-1 not found via pkg-config. "
            "Wayland capture will be unavailable at runtime. "
            "apt install libdbus-1-dev to enable.")
    endif()

    # Wayland client + xdg-shell protocol for the Wayland presenter.
    # wayland-client is needed for wl_display, wl_registry,
    # wl_compositor, wl_surface, wl_callback.
    # xdg-shell is generated from the protocol XML at build time
    # via the wayland-client-codegen module.
    pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
    target_link_libraries(unio-pipe PRIVATE PkgConfig::WAYLAND_CLIENT)

    # Generate xdg-shell client protocol code from the XML.
    find_program(WAYLAND_SCANNER wayland-scanner)
    if(WAYLAND_SCANNER)
        set(XDG_SHELL_PROTOCOL
            "${CMAKE_CURRENT_SOURCE_DIR}/protocols/xdg-shell.xml")
        if(EXISTS "${XDG_SHELL_PROTOCOL}")
            set(XDG_SHELL_CLIENT_HEADER
                "${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-client-protocol.h")
            set(XDG_SHELL_CLIENT_C
                "${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-protocol.c")
            add_custom_command(OUTPUT "${XDG_SHELL_CLIENT_HEADER}"
                COMMAND ${CMAKE_COMMAND} -E make_directory
                    "${CMAKE_CURRENT_BINARY_DIR}"
                COMMAND ${WAYLAND_SCANNER}
                    client-header
                    "${XDG_SHELL_PROTOCOL}"
                    "${XDG_SHELL_CLIENT_HEADER}"
                DEPENDS "${XDG_SHELL_PROTOCOL}"
                COMMENT "Generating xdg-shell client header")
            add_custom_command(OUTPUT "${XDG_SHELL_CLIENT_C}"
                COMMAND ${CMAKE_COMMAND} -E make_directory
                    "${CMAKE_CURRENT_BINARY_DIR}"
                COMMAND ${WAYLAND_SCANNER}
                    private-code
                    "${XDG_SHELL_PROTOCOL}"
                    "${XDG_SHELL_CLIENT_C}"
                DEPENDS "${XDG_SHELL_PROTOCOL}"
                COMMENT "Generating xdg-shell client code")
            add_custom_target(xdg-shell-protocol ALL
                DEPENDS "${XDG_SHELL_CLIENT_HEADER}" "${XDG_SHELL_CLIENT_C}")
            target_include_directories(unio-pipe PRIVATE
                "${CMAKE_CURRENT_BINARY_DIR}")
            # Compile the generated C code directly into the main
            # target so the hidden-visibility symbols are visible
            # at link time (not exported through a DSO).
            set_source_files_properties("${XDG_SHELL_CLIENT_C}"
                PROPERTIES LANGUAGE C)
            target_sources(unio-pipe PRIVATE "${XDG_SHELL_CLIENT_C}")
        else()
            message(WARNING "xdg-shell.xml not found; Wayland presenter may not compile")
        endif()
    else()
        message(WARNING "wayland-scanner not found; Wayland presenter disabled")
    endif()

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
# Normalize on a single "have msquic" flag to avoid double-linking
# when MSQUIC_ROOT is set (MSQUIC_SYSTEM was never set in that branch,
# causing the FetchContent block to run and link a second copy).
set(MSQUIC_HAVE OFF)

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
    set(MSQUIC_HAVE ON)
else()
    # Try system-installed msquic (distro package with cmake config).
    find_package(msquic)
    if(msquic_FOUND)
        target_link_libraries(unio-pipe PRIVATE msquic)
        message(STATUS "Using system msquic")
        set(MSQUIC_HAVE ON)
    endif()
endif()

if(NOT MSQUIC_HAVE)
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

# Windows NVENC — pull FFmpeg/nv-codec-headers (MIT) for the
# nvEncodeAPI.h header only. We LoadLibrary the DLL at runtime
# (it ships with every NVIDIA driver as nvEncodeAPI64.dll in
# System32), so there's no build-time link dep on NVIDIA's SDK
# or driver.
if(WIN32)
    FetchContent_Declare(
        nv_codec_headers
        GIT_REPOSITORY https://github.com/FFmpeg/nv-codec-headers.git
        GIT_TAG        n12.2.72.0
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(nv_codec_headers)
    target_include_directories(unio-pipe PRIVATE
        "${nv_codec_headers_SOURCE_DIR}/include")
endif()

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
