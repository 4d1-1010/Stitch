# Vendored Dear ImGui v1.92.7. MIT. See README.md for pinning +
# update instructions.

# Core library — platform-independent, consumed by every unio-ui
# target. Excludes backend (imgui_impl_*) files; those are added
# per-platform in unio-ui/CMakeLists.txt.
add_library(imgui STATIC
    imgui.cpp
    imgui_widgets.cpp
    imgui_draw.cpp
    imgui_tables.cpp
    imgui_demo.cpp
    misc/cpp/imgui_stdlib.cpp
)

target_include_directories(imgui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/backends
    ${CMAKE_CURRENT_SOURCE_DIR}/misc/cpp
)

# ImGui's own build is noisy under -Wpedantic / /W4 — we apply
# strict flags to our own target only (see the parent
# CMakeLists.txt). Silence anything the upstream code can't avoid
# when it gets pulled in by a /W4 consumer. ImGui upstream targets
# /W3 + -Wall; we don't try to exceed that.
if(MSVC)
    target_compile_options(imgui PRIVATE /W3)
else()
    target_compile_options(imgui PRIVATE
        -Wall -Wno-unused-function -Wno-missing-field-initializers)
endif()

# Private define: ImGui builds with _CRT_SECURE_NO_WARNINGS
# defined anyway at file scope, but setting it here keeps MSVC's
# warning suite quiet for the whole TU batch.
if(MSVC)
    target_compile_definitions(imgui PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
