/*! @file main.cpp
 *  @brief unio-ui entry point.
 *
 *  Picks the platform-specific event loop at build time and hands
 *  off. Implementations live in `src/platform/win32/app_win32.cpp`
 *  and `src/platform/x11/app_x11.cpp`.
 */

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
