# EmbedBinary.cmake — generate a header that declares a binary
# file's bytes as a `const unsigned char[]` plus its size. Used
# for bundling Inter TTFs into xorio-ui without breaking the
# single-file ship.
#
# Usage:
#   include(cmake/EmbedBinary.cmake)
#   embed_binary_as_header(
#       INPUT  ${CMAKE_SOURCE_DIR}/xorio-ui/vendor/inter/Inter-Regular.ttf
#       OUTPUT ${CMAKE_BINARY_DIR}/generated/Inter-Regular.embed.h
#       SYMBOL inter_regular)
#
# The generated header exposes:
#   inline constexpr unsigned char inter_regular_data[];
#   inline constexpr std::size_t   inter_regular_size;
#
# Why not xxd: avoids an extra apt package in the Docker image;
# keeps the build pure-CMake so Windows cross-builds don't need
# the POSIX tool on PATH.
#
# Why not `#embed` (C++23): msvc-wine's MSVC 17.x doesn't support
# it yet; revisit when we drop pre-C++23 support.

function(embed_binary_as_header)
    cmake_parse_arguments(A "" "INPUT;OUTPUT;SYMBOL" "" ${ARGN})
    if(NOT EXISTS "${A_INPUT}")
        message(FATAL_ERROR "embed_binary_as_header: missing input ${A_INPUT}")
    endif()

    file(READ "${A_INPUT}" hex HEX)
    file(SIZE "${A_INPUT}" size)

    # Turn "deadbeef" into "0xde,0xad,0xbe,0xef,". Chunk every 12
    # bytes so the generated header is human-diff-able if inspected
    # (one full line ≈ 72 cols). The regex `(..){12}` eats 12
    # two-char groups and re-inserts a newline after the last one.
    string(REGEX REPLACE "(..)" "0x\\1," hex "${hex}")
    string(REGEX REPLACE "((0x..,){12})" "\\1\n    " hex "${hex}")

    set(content "// Auto-generated from ${A_INPUT}. DO NOT EDIT.\n")
    string(APPEND content "// Licence: whatever the source file ships as — see neighbour LICENSE.txt.\n")
    string(APPEND content "#pragma once\n")
    string(APPEND content "#include <cstddef>\n\n")
    string(APPEND content "inline constexpr unsigned char ${A_SYMBOL}_data[] = {\n    ${hex}\n};\n\n")
    string(APPEND content "inline constexpr std::size_t ${A_SYMBOL}_size = ${size};\n")

    get_filename_component(out_dir "${A_OUTPUT}" DIRECTORY)
    file(MAKE_DIRECTORY "${out_dir}")
    file(WRITE "${A_OUTPUT}" "${content}")
endfunction()
