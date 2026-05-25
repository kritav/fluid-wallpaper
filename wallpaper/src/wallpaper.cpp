#include "wallpaper.h"

namespace {

void enumerate_for_workerw(HWND* out) {
    EnumWindows([](HWND top, LPARAM lparam) -> BOOL {
        // The WorkerW is the next sibling of the one that hosts SHELLDLL_DefView
        HWND shell_view = FindWindowExW(top, nullptr, L"SHELLDLL_DefView", nullptr);
        if (shell_view) {
            auto* o = reinterpret_cast<HWND*>(lparam);
            *o = FindWindowExW(nullptr, top, L"WorkerW", nullptr);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(out));
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_ERASEBKGND:
            return 1;
    }
    return DefWindowProcW(hwnd, msg, w, l);
}

}  // namespace

HWND find_workerw() {
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (!progman) return nullptr;

    // 0x052C is undocumented but stable since Windows 8: it asks Progman
    // to spawn a WorkerW behind the desktop icons. Try the classic
    // (wParam=0) form first, then the wParam=0xD variant used by Lively
    // and other Win11-friendly engines.
    DWORD_PTR ignored = 0;
    SendMessageTimeoutW(progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, &ignored);

    HWND workerw = nullptr;
    enumerate_for_workerw(&workerw);

    if (!workerw) {
        SendMessageTimeoutW(progman, 0x052C, static_cast<WPARAM>(0xD),
                            static_cast<LPARAM>(0), SMTO_NORMAL, 1000, &ignored);
        SendMessageTimeoutW(progman, 0x052C, static_cast<WPARAM>(0xD),
                            static_cast<LPARAM>(1), SMTO_NORMAL, 1000, &ignored);
        Sleep(50);
        enumerate_for_workerw(&workerw);
    }
    return workerw ? workerw : progman;
}

HWND create_wallpaper_window(HWND workerw, HINSTANCE hinstance, int width, int height) {
    const wchar_t* CLASS_NAME = L"FluidWallpaperWindow";

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = hinstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    // Standalone diagnostic mode (workerw == nullptr)
    const DWORD style    = workerw ? (WS_POPUP            | WS_VISIBLE)
                                   : (WS_OVERLAPPEDWINDOW | WS_VISIBLE);
    const DWORD ex_style = workerw ?  WS_EX_NOACTIVATE : 0;

    HWND hwnd = CreateWindowExW(
        ex_style,
        CLASS_NAME,
        L"Fluid Wallpaper",
        style,
        CW_USEDEFAULT, CW_USEDEFAULT, width, height,
        nullptr, nullptr, hinstance, nullptr);

    if (hwnd && workerw) {
        SetParent(hwnd, workerw);

        // WS_POPUP windows don't participate in normal child z-ordering,
        // so HWND_INSERTAFTER below wouldn't behave intuitively on them.
        // Flip to WS_CHILD now that we have a parent.
        LONG_PTR style_bits = GetWindowLongPtrW(hwnd, GWL_STYLE);
        style_bits = (style_bits & ~WS_POPUP) | WS_CHILD;
        SetWindowLongPtrW(hwnd, GWL_STYLE, style_bits);

        // Slot ourselves directly below SHELLDLL_DefView so icons render
        // on top of us while we still cover Progman's wallpaper paint.
        HWND defview = FindWindowExW(workerw, nullptr, L"SHELLDLL_DefView", nullptr);
        const HWND insert_after = defview ? defview : HWND_TOP;
        SetWindowPos(hwnd, insert_after, 0, 0, 0, 0,
                     SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED);
    }
    return hwnd;
}
