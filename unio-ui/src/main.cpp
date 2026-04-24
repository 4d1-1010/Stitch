// unio-ui entry point. Platform-specific event loop lives in
// src/platform/{win32,x11}/app_*.cpp; this file just picks which
// one at build time and hands off.

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
