// PTZ Control for Windows: entry point.
#include "app.h"

#include <commctrl.h>
#include <gdiplus.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    // ssh runs this executable as SSH_ASKPASS; relay its prompt and exit before any UI.
    if (ptz_is_askpass_invocation()) return ptz_askpass_main(0, nullptr);

    INITCOMMONCONTROLSEX controls{sizeof controls, ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    Gdiplus::GdiplusStartupInput gdiplus_input;
    ULONG_PTR gdiplus = 0;
    Gdiplus::GdiplusStartup(&gdiplus, &gdiplus_input, nullptr);

    HWND window = create_console(instance, show);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (console_translate(msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    (void)window;
    Gdiplus::GdiplusShutdown(gdiplus);
    return static_cast<int>(msg.wParam);
}
