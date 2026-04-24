/// @file main.cpp
/// @brief Executable entry point; dispatches to the OS-specific app loop.

#include "platform/app.hpp"

#if defined(_WIN32)

#include <windows.h>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    return unio_ui::platform::run({});
}

#else

int main(int, char**) {
    return unio_ui::platform::run({});
}

#endif
