cmake_minimum_required(VERSION 3.16)
project(unio-pipe VERSION 0.1.0 LANGUAGES C CXX)
# LANGUAGES C: we have no .c files of our own, but the Windows
# build links msquic's `core` and `platform` static archives
# directly (see the msquic block below — the upstream monolithic
# static merge is broken under msvc-wine). Those targets are C
# libraries, and when CMake propagates their INTERFACE properties
# to the C++ unio-pipe target it needs the C compiler registered
# to resolve C feature requirements. On Linux this is a no-op.
# NB: VERSION is consumed by libvpl's version.rc.in (FetchContent
# pulls it in and populates @CMAKE_PROJECT_VERSION_MAJOR@ from the
# top-level project's version). Leaving it unset produces
# VER_PRODUCTVERSION ,,0,0 which the resource compiler rejects.

# Day-1 scope of PR 6: compile, open a UDS control socket (Linux)
# or named pipe (Windows), accept JSON commands. No GPU, no
# encoder, no transport yet — those are the following weeks'
# work.  This file's job is to make the directory actually build.

# C++20 project-wide. On Windows we now build via containerized
# VS 2022 (packaging/docker/Dockerfile.win-cross), which ships
# cppwinrt with native C++20 coroutine support — the old /await
# hack used to pacify VS 2019's cppwinrt is no longer needed.
# Linux Clang 18 / GCC 13 both support C++20 cleanly. Net gain:
# concepts, ranges, std::span, designated initializers, native
# coroutines, constexpr <algorithm>.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# On Windows, statically link the MSVC runtime (/MT) instead of
# dynamically (/MD). Two reasons:
#   1. Matches msquic's own build flags — no cross-CRT mismatch
#      when we call into the msquic-bundled OpenSSL statics.
#   2. Drops MSVCP140 / VCRUNTIME140 / VCRUNTIME140_1 from the
#      ship list — unio-pipe.exe only needs Windows system DLLs
#      + msquic.dll + libvpl.dll, never the VC++ Redistributable.
# Size cost: ~200 KB. Worth it.
if(WIN32)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

# Strict-warning flags go on the unio-pipe target only, not
# project-wide. add_compile_options() leaks flags into every
# FetchContent target (msquic, libvpl, opensslquic) which then
# fail -Werror=pedantic on MSVC-ish anonymous structs and
# zero-size arrays they don't care to clean up. Applied via
# target_compile_options(unio-pipe ...) below.
set(UNIO_PIPE_CXX_FLAGS "")
if(NOT MSVC)
    set(UNIO_PIPE_CXX_FLAGS
        -Wall -Wextra -Wpedantic -Werror -Wno-unused-parameter)
else()
    # /WX == -Werror. Intentional MSVC warning-overrides:
    #   C4324 — "struct padded due to alignment specifier" — the
    #           whole point of alignas(64) on SpscRing's head/tail.
    #   C4996 — "consider fopen_s/_dupenv_s" — POSIX fopen is
    #           portable-enough for our purposes.
    #   C4201 — "nameless struct/union" — common in libvpl and
    #           other vendor headers we include.
    # /await kept for VS-2019 cppwinrt compatibility (pre-VS 2022
    # workaround). Containerized builds use VS 2022 which doesn't
    # need it — /await is a harmless no-op under /std:c++20.
    set(UNIO_PIPE_CXX_FLAGS /W4 /WX /wd4324 /wd4201 /await)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

set(UNIO_PIPE_SOURCES
    src/main.cpp
    src/control_socket.cpp
    src/caps.cpp
    src/capability_probe.cpp
    src/cuda_runtime.cpp
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
    src/encoder_nvenc_linux.cpp
    src/decoder_vaapi.cpp
    src/decoder_d3d11va.cpp
    src/decoder_nvdec.cpp
    src/decoder_onevpl.cpp
    src/presenter_egl_x11.cpp
    src/presenter_dxgi.cpp
    src/quic_transport.cpp
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND UNIO_PIPE_SOURCES src/capture_xcomposite.cpp)
endif()

if(WIN32)
    list(APPEND UNIO_PIPE_SOURCES
        src/capture_wgc.cpp
        src/encoder_nvenc.cpp
        src/encoder_onevpl.cpp)
endif()

add_executable(unio-pipe ${UNIO_PIPE_SOURCES})

# Apply strict warnings only to our own code. Third-party deps
# pulled in via FetchContent (msquic, libvpl, opensslquic) get
# their own flags — see UNIO_PIPE_CXX_FLAGS note above.
target_compile_options(unio-pipe PRIVATE ${UNIO_PIPE_CXX_FLAGS})

target_include_directories(unio-pipe PRIVATE include)

# Embed the git commit the helper was built from. The matrix
# test runner (tools/matrix_test.py) uses this to verify every
# host in a cross-machine run is running bit-identical code —
# mtime heuristics can't catch "this host was built from a
# different branch", the embedded commit string can. A dirty
# working tree at build time gets the "-dirty" suffix so ad-hoc
# local builds are obvious in the logs.
#
# Resolution order:
#   1. -DUNIO_BUILD_COMMIT=<sha> passed to cmake (used by
#      matrix_test.py --sync when pushing source to a remote
#      host that doesn't have the .git/ metadata — the
#      orchestrator knows the commit, hands it across)
#   2. git rev-parse on the source tree (normal local builds)
#   3. "unknown" fallback
if(NOT UNIO_BUILD_COMMIT)
    find_package(Git QUIET)
    if(Git_FOUND)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} rev-parse --short=12 HEAD
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            OUTPUT_VARIABLE UNIO_BUILD_COMMIT
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} status --porcelain
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            OUTPUT_VARIABLE UNIO_BUILD_DIRTY
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(UNIO_BUILD_DIRTY)
            set(UNIO_BUILD_COMMIT "${UNIO_BUILD_COMMIT}-dirty")
        endif()
    endif()
endif()
if(NOT UNIO_BUILD_COMMIT)
    set(UNIO_BUILD_COMMIT "unknown")
endif()
target_compile_definitions(unio-pipe PRIVATE
    UNIO_BUILD_COMMIT="${UNIO_BUILD_COMMIT}")
message(STATUS "unio-pipe build commit: ${UNIO_BUILD_COMMIT}")

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
    # Static-link msquic into unio-pipe so the shipped targets
    # are single-file binaries on both Linux and Windows:
    #   dist/linux-x64/unio-pipe         (no libmsquic.so.2)
    #   dist/win-x64/unio-pipe.exe       (no msquic.dll)
    # No RPATH dance on Linux, no PATH / DLL-in-same-dir concern
    # on Windows. Trade-off is a ~5 MB heavier binary and no
    # drop-in msquic upgrade — fine for our ship model.
    set(QUIC_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        msquic
        GIT_REPOSITORY https://github.com/microsoft/msquic.git
        GIT_TAG        v2.4.5
        GIT_SHALLOW    TRUE
        GIT_SUBMODULES_RECURSE TRUE
    )
    FetchContent_MakeAvailable(msquic)

    # Static msquic needs its transitive deps re-stated — the
    # dynamic build linked them privately, the static build
    # expects the consumer to.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # Linux: msquic builds a real monolithic libmsquic.a
        # (GNU `ar -M` handles absolute paths fine). Attach numa
        # + atomic + the bundled openssl3 statics to the msquic
        # target's INTERFACE so CMake emits them AFTER
        # libmsquic.a on the link line — Ubuntu's default
        # `--as-needed` would otherwise drop anything placed
        # before the archive that actually references it.
        target_link_libraries(msquic INTERFACE
            "${msquic_BINARY_DIR}/../opensslquic-build/openssl3/lib/libssl.a"
            "${msquic_BINARY_DIR}/../opensslquic-build/openssl3/lib/libcrypto.a"
            numa    # platform_posix.c — NUMA worker placement
            atomic) # fallback for archs where libstdc++ can't inline atomics
        target_link_libraries(unio-pipe PRIVATE msquic)
    else()
        # Windows (msvc-wine cross-build): msquic's monolithic
        # static merge is broken here. It generates a response
        # file full of Unix-style absolute paths
        # (/build/.../core.lib) and hands it to lib.exe via
        # `lib @file`. msvc-wine's wrapper rewrites argv paths
        # to `z:/...` but cannot reach paths inside the @-file,
        # so lib.exe sees /build/... as unknown switches and
        # silently drops every component, leaving a near-empty
        # msquic.lib. Sidestep the broken merge by linking
        # msquic's component archives directly — `core` +
        # `platform` + `msquic_static` have all the exports the
        # monolithic was supposed to aggregate. This is what
        # msquic's upstream comment ("Users can link to
        # msquic_static directly if they don't need the
        # monolithic library") explicitly supports.
        target_link_libraries(unio-pipe PRIVATE
            msquic_static core platform)
        target_compile_definitions(unio-pipe PRIVATE
            QUIC_BUILD_STATIC)
        # openssl3 statics from msquic's bundled build (already
        # wired in via the WIN32 openssl block further below,
        # but quic_transport.cpp's self-signed cert code also
        # pulls in libs that msquic's monolithic would have
        # aggregated — wbemuuid + winmm for the platform layer).
        target_link_libraries(unio-pipe PRIVATE
            wbemuuid winmm ntdll
            iphlpapi)   # IP Helper API: msquic's platform layer
                        # calls GetIpNetEntry2 / ResolveIpNetEntry2
                        # / ConvertInterfaceLuidToAlias /
                        # ConvertInterfaceIndexToLuid and core's
                        # binding code calls
                        # {Set,Get}CurrentThreadCompartmentId.
    endif()
    target_include_directories(unio-pipe PRIVATE
        "${msquic_SOURCE_DIR}/src/inc")
endif()
target_compile_definitions(unio-pipe PRIVATE UNIO_PIPE_HAS_MSQUIC=1)

# NVIDIA Video Codec SDK headers — FFmpeg/nv-codec-headers (MIT).
# Ships nvEncodeAPI.h + nvcuvid.h + cuviddec.h, plus minimal
# cuda.h-equivalent declarations. We dlopen / LoadLibrary the
# actual runtime DLLs / .so files at runtime — no build-time
# link dep on NVIDIA's proprietary SDK or driver. Headers land
# on every OS we target (Linux uses them for NVENC-Linux +
# NVDEC via CudaRuntime; Windows uses them for the native
# NVENC path).
FetchContent_Declare(
    nv_codec_headers
    GIT_REPOSITORY https://github.com/FFmpeg/nv-codec-headers.git
    GIT_TAG        n12.2.72.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(nv_codec_headers)
target_include_directories(unio-pipe PRIVATE
    "${nv_codec_headers_SOURCE_DIR}/include")

# Intel oneVPL (libvpl) — Windows only. MIT per Intel's SPDX.
# Provides the MFX headers AND builds libvpl.dll (the user-
# facing dispatcher) so pre-oneVPL-2.x Intel drivers (Coffee
# Lake era) light up via the bundled redist. Linux Intel uses
# VA-API, not oneVPL — no reason to build this on Linux.
if(WIN32)
    set(BUILD_TESTS        OFF CACHE BOOL "" FORCE)
    set(BUILD_EXAMPLES     OFF CACHE BOOL "" FORCE)
    set(BUILD_DEV          OFF CACHE BOOL "" FORCE)
    set(BUILD_TOOLS        OFF CACHE BOOL "" FORCE)
    set(INSTALL_EXAMPLES   OFF CACHE BOOL "" FORCE)
    set(INSTALL_DEV        OFF CACHE BOOL "" FORCE)
    set(BUILD_EXPERIMENTAL OFF CACHE BOOL "" FORCE)
    # libvpl's CMakeLists unconditionally
    # `include(InstallRequiredSystemLibraries)`. On a Linux host
    # (our msvc-wine cross-build) that module fails hard at
    # cmake_host_system_information(VS_*_DIR) — those queries are
    # Windows-only. Since we link /MT (static MSVC CRT), the
    # redist DLLs would never ship anyway, so prepend a no-op
    # stub directory to CMAKE_MODULE_PATH. See
    # cmake/stubs/InstallRequiredSystemLibraries.cmake for the
    # full rationale.
    if(NOT CMAKE_HOST_WIN32)
        list(PREPEND CMAKE_MODULE_PATH
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/stubs")
    endif()
    FetchContent_Declare(
        libvpl_headers
        GIT_REPOSITORY https://github.com/intel/libvpl.git
        GIT_TAG        v2.15.0
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(libvpl_headers)
    target_include_directories(unio-pipe PRIVATE
        "${libvpl_headers_SOURCE_DIR}/api")
    if(TARGET VPL)
        target_compile_options(VPL PRIVATE /W0 /WX-)
        add_custom_command(TARGET unio-pipe POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:VPL>
                $<TARGET_FILE_DIR:unio-pipe>
            COMMENT "Copying libvpl.dll next to unio-pipe.exe")
    endif()
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

# (No post-build DLL copy needed. msquic is static-linked into
# unio-pipe on both Windows and Linux — QUIC_BUILD_SHARED=OFF
# above. With static msquic, the `msquic` target exported by
# FetchContent is an INTERFACE that aggregates msquic_static +
# msquic_lib, so $<TARGET_FILE:msquic> has no meaning anyway.)
