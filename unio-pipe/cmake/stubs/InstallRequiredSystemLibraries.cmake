# No-op override of CMake's built-in InstallRequiredSystemLibraries
# module, loaded via CMAKE_MODULE_PATH only when we're cross-
# compiling for Windows from a non-Windows host (see the sibling
# unio-pipe/CMakeLists.txt).
#
# Why this exists: Intel libvpl's CMakeLists (pulled in via our
# FetchContent for the oneVPL dispatcher on Windows) unconditionally
# `include(InstallRequiredSystemLibraries)`. On Linux hosts that
# module fails hard with
#
#   cmake_host_system_information does not recognize <key> VS_17_DIR
#
# because that undocumented query is Windows-only. We don't actually
# need it — unio-pipe statically links the MSVC C++ runtime (/MT in
# the top-level CMakeLists, set via CMAKE_MSVC_RUNTIME_LIBRARY =
# MultiThreaded), so MSVCP140.dll / VCRUNTIME140.dll would never be
# shipped regardless of what the module decided.
#
# Intentionally empty: when CMake's include() resolver finds our
# stub first (CMAKE_MODULE_PATH is searched before the built-in
# module directory), libvpl's include call simply returns and the
# Windows cross-build proceeds.
