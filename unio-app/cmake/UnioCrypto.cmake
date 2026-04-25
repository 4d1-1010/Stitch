# UnioCrypto.cmake — internal helper that defines the
# `unio::crypto` interface target consumed by every TU in
# `unio-app` that signs / verifies / AEADs.
#
# Sourcing per platform:
#   * Linux:   system OpenSSL via find_package(OpenSSL).
#              Distro packages (libssl-dev / openssl-devel) ship
#              the headers + .so we link against.
#   * Windows: pre-built static OpenSSL from the cross-compile
#              image (`unio-app/packaging/docker/Dockerfile.win-cross`
#              builds libcrypto.lib under /opt/openssl-win/ at
#              image-build time). Native Windows builds set
#              UNIO_OPENSSL_ROOT to point at a local install
#              (e.g. C:/Program Files/OpenSSL-Win64).
#
# Override:
#   -DUNIO_OPENSSL_ROOT=<path>   (Windows; default
#                                Z:/opt/openssl-win in the
#                                cross-compile image)
#
# Scope: private to unio-app. When unio-pipe lands in this tree
# (Phase 3 of the C++ port) and brings msquic's bundled openssl3
# into the same configure pass, this file picks up the unified
# source — until then unio-app owns its Windows libcrypto outright.

if(TARGET unio::crypto)
    return()
endif()

add_library(unio_crypto INTERFACE)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(OpenSSL REQUIRED)
    target_link_libraries(unio_crypto INTERFACE OpenSSL::Crypto)
elseif(WIN32)
    if(NOT DEFINED UNIO_OPENSSL_ROOT)
        # Default for the msvc-wine cross-compile image: install
        # lives at /opt/openssl-win on the Linux host filesystem.
        # Native Windows builds should pass
        # -DUNIO_OPENSSL_ROOT=C:/path/to/openssl on the cmake CLI.
        set(UNIO_OPENSSL_ROOT "/opt/openssl-win"
            CACHE PATH "Root of the static OpenSSL install for Windows")
    endif()
    if(NOT EXISTS "${UNIO_OPENSSL_ROOT}/lib/libcrypto.lib")
        message(FATAL_ERROR
            "unio::crypto on Windows: libcrypto.lib not found at "
            "${UNIO_OPENSSL_ROOT}/lib/. Either rebuild the cross-"
            "compile image (it provisions OpenSSL at "
            "/opt/openssl-win) or pass "
            "-DUNIO_OPENSSL_ROOT=<install-prefix> to cmake.")
    endif()
    # Path handling under msvc-wine cross-compile is asymmetric:
    #
    #   * Include path: hand cmake the Linux absolute form
    #     (/opt/openssl-win/include). Linux cmake passes it
    #     through as `-I/opt/...` and wine auto-maps the POSIX
    #     root to the Z: drive when cl.exe opens the file. Using
    #     a `Z:/...` form here would trip Linux cmake's relative-
    #     path detection and emit `-I/src/Z:/opt/...` instead.
    #
    #   * Library path: must reach link.exe as a wine drive-
    #     lettered absolute (`Z:/opt/...`). A POSIX absolute on
    #     the link line is silently misparsed by link.exe (the
    #     `/OPT` family is a real switch). And we cannot pass the
    #     `Z:/...` form through `target_link_libraries` because
    #     Linux cmake doesn't recognise it as an absolute file
    #     path and applies platform name-mangling, producing
    #     `libcrypto.lib.lib`. `target_link_options` puts the
    #     string on the link line verbatim — link.exe picks it up
    #     as a positional input file, no mangling, no /OPT
    #     misparse.
    if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux"
       AND UNIO_OPENSSL_ROOT MATCHES "^/")
        set(_unio_crypto_lib "Z:${UNIO_OPENSSL_ROOT}/lib/libcrypto.lib")
    else()
        set(_unio_crypto_lib "${UNIO_OPENSSL_ROOT}/lib/libcrypto.lib")
    endif()
    target_include_directories(unio_crypto INTERFACE
        "${UNIO_OPENSSL_ROOT}/include")
    target_link_options(unio_crypto INTERFACE
        "${_unio_crypto_lib}")
    target_link_libraries(unio_crypto INTERFACE
        # libcrypto on Windows reaches into these system libs for
        # DPAPI helpers (crypt32), the BCrypt PRNG (bcrypt), and
        # the socket-layer headers some EVP helpers transitively
        # touch (ws2_32). Plain library names — cmake's mangling
        # adds the .lib suffix correctly here.
        crypt32 bcrypt ws2_32)
else()
    message(FATAL_ERROR
        "unio::crypto: unsupported platform ${CMAKE_SYSTEM_NAME}.")
endif()

add_library(unio::crypto ALIAS unio_crypto)
