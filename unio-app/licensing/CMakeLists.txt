# licensing — vendor key material + the unio-license-tool CLI.
#
# Scope: dev-side tooling only. Not shipped to end users. The
# binary signs licence tokens with the vendor Ed25519 private key
# (dev/vendor_master.key); the *public* half lives next to it
# (dev/vendor_master.pub.raw) and is embedded into the runtime
# binary by sibling unio/ at configure time.
#
# Wire-format compatibility with the runtime app is enforced by
# linking the same `unio::license_format` static library — see
# unio/CMakeLists.txt.
#
# Linux-only today. The Windows path lands when a native (non
# msvc-wine) Windows pipeline is wired up; msvc-wine has no
# OpenSSL headers we can target here.

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    return()
endif()

add_executable(unio-license-tool
    src/license_tool.cpp
)
target_compile_options(unio-license-tool PRIVATE ${UNIO_APP_CXX_FLAGS})
target_link_libraries(unio-license-tool PRIVATE
    unio::license_format
    unio::crypto)
