// TaskDialogIndirect lives in Common Controls v6. Without this manifest
// dependency the function returns E_FAIL on every modern Windows.
#pragma comment(linker, "\"/manifestdependency:type='win32' " \
    "name='Microsoft.Windows.Common-Controls' version='6.0.0.0' " \
    "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' " \
    "language='*'\"")

#include "tray.h"

#include <commctrl.h>
#include <shellapi.h>
#include <cstdio>
#include <string>

#include "resource.h"  // resources/ is on the include path via CMake

namespace {

// Notification icon callback message. Must be in WM_APP..0xBFFF range
// (WM_USER is reserved by some shell components for their own use).
constexpr UINT WMA_TRAY_NOTIFY = WM_APP + 1;
constexpr UINT TRAY_ICON_ID    = 1;

// Menu command IDs. Distinct from any HMENU values used elsewhere.
enum : UINT {
    ID_PAUSE      = 1001,
    ID_CLEAR      = 1002,
    ID_NEXT_MODE  = 1003,
    ID_BLOOM      = 1004,
    ID_IDLE       = 1005,
    ID_AUTOSTART  = 1006,
    ID_ABOUT      = 1007,
    ID_EXIT       = 1008,
};

const wchar_t* const TRAY_CLASS_NAME = L"FluidWallpaperTray";
const wchar_t* const TOOLTIP_TEXT    = L"Fluid Wallpaper";

HWND               g_hwnd     = nullptr;
HICON              g_icon     = nullptr;
tray::Commands*    g_commands = nullptr;
tray::State*       g_state    = nullptr;

void show_context_menu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);

    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    const UINT checked = MF_STRING | MF_CHECKED;
    const UINT unchecked = MF_STRING;

    bool paused = g_state ? g_state->paused.load()        : false;
    bool bloom  = g_state ? g_state->bloom_enabled.load() : true;
    bool idle   = g_state ? g_state->idle_enabled.load()  : true;
    bool astart = autostart::is_enabled();

    AppendMenuW(menu, paused ? checked : unchecked, ID_PAUSE,
                paused ? L"Resume" : L"Pause");
    AppendMenuW(menu, MF_STRING, ID_CLEAR, L"Clear simulation\tC");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_NEXT_MODE, L"Next visual mode\tM");
    AppendMenuW(menu, bloom ? checked : unchecked, ID_BLOOM, L"Bloom\tB");
    AppendMenuW(menu, idle  ? checked : unchecked, ID_IDLE,  L"Idle motion\tI");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, astart ? checked : unchecked, ID_AUTOSTART,
                L"Start with Windows");
    AppendMenuW(menu, MF_STRING, ID_ABOUT, L"About Fluid Wallpaper");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_EXIT, L"Exit");

    // The SetForegroundWindow + WM_NULL post is the standard MS-recommended
    // workaround for menus left on screen when the user clicks elsewhere.
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu,
                   TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
                   pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

void dispatch_command(UINT id) {
    if (!g_commands) return;
    switch (id) {
        case ID_PAUSE:     g_commands->toggle_pause.store(true);     break;
        case ID_CLEAR:     g_commands->clear.store(true);            break;
        case ID_NEXT_MODE: g_commands->next_mode.store(true);        break;
        case ID_BLOOM:     g_commands->toggle_bloom.store(true);     break;
        case ID_IDLE:      g_commands->toggle_idle.store(true);      break;
        case ID_AUTOSTART: g_commands->toggle_autostart.store(true); break;
        case ID_ABOUT:     g_commands->show_about.store(true);       break;
        case ID_EXIT:      g_commands->quit.store(true);             break;
    }
}

LRESULT CALLBACK tray_wnd_proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WMA_TRAY_NOTIFY) {
        // lParam holds the event (low word) — WM_LBUTTONUP, WM_RBUTTONUP,
        // WM_CONTEXTMENU, etc.
        UINT event = LOWORD(l);
        if (event == WM_RBUTTONUP || event == WM_CONTEXTMENU) {
            show_context_menu(hwnd);
        } else if (event == WM_LBUTTONDBLCLK) {
            dispatch_command(ID_PAUSE);
        }
        return 0;
    }
    if (msg == WM_COMMAND) {
        dispatch_command(LOWORD(w));
        return 0;
    }
    return DefWindowProcW(hwnd, msg, w, l);
}

}  // namespace

namespace tray {

bool init(HINSTANCE hinst, Commands* commands, State* state) {
    g_commands = commands;
    g_state    = state;

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = tray_wnd_proc;
    wc.hInstance     = hinst;
    wc.lpszClassName = TRAY_CLASS_NAME;
    RegisterClassExW(&wc);

    // Top-level but unmapped: needed for SetForegroundWindow to work before
    // TrackPopupMenu. HWND_MESSAGE windows can't take foreground, which
    // breaks click-outside menu dismissal.
    g_hwnd = CreateWindowExW(0, TRAY_CLASS_NAME, L"", 0,
                             0, 0, 0, 0,
                             nullptr, nullptr, hinst, nullptr);
    if (!g_hwnd) return false;

    g_icon = (HICON)LoadImageW(hinst,
                               MAKEINTRESOURCEW(IDI_TRAY_ICON),
                               IMAGE_ICON,
                               GetSystemMetrics(SM_CXSMICON),
                               GetSystemMetrics(SM_CYSMICON),
                               LR_DEFAULTCOLOR);
    if (!g_icon) {
        // Fall back to a stock icon so the user still gets a tray entry.
        g_icon = LoadIconW(nullptr, IDI_APPLICATION);
    }

    NOTIFYICONDATAW nid{};
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = g_hwnd;
    nid.uID              = TRAY_ICON_ID;
    nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WMA_TRAY_NOTIFY;
    nid.hIcon            = g_icon;
    lstrcpynW(nid.szTip, TOOLTIP_TEXT, sizeof(nid.szTip) / sizeof(nid.szTip[0]));

    if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
        fprintf(stderr, "Shell_NotifyIcon(NIM_ADD) failed.\n");
        return false;
    }
    return true;
}

void shutdown() {
    if (g_hwnd) {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd   = g_hwnd;
        nid.uID    = TRAY_ICON_ID;
        Shell_NotifyIconW(NIM_DELETE, &nid);

        DestroyWindow(g_hwnd);
        g_hwnd = nullptr;
    }
    if (g_icon) {
        DestroyIcon(g_icon);
        g_icon = nullptr;
    }
    g_commands = nullptr;
    g_state    = nullptr;
}

}  // namespace tray

// -----------------------------------------------------------------------------
// Autostart: HKCU\Software\Microsoft\Windows\CurrentVersion\Run\fluid-wallpaper
// = "<quoted exe path>". The installer's optional autostart task writes the
// same value, so they're interchangeable.
// -----------------------------------------------------------------------------
namespace autostart {

namespace {
constexpr const wchar_t* RUN_KEY   = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* RUN_VALUE = L"fluid-wallpaper";

std::wstring quoted_exe_path() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n == MAX_PATH) return L"";
    std::wstring p;
    p.reserve(n + 2);
    p.push_back(L'"');
    p.append(buf, n);
    p.push_back(L'"');
    return p;
}
}  // namespace

bool is_enabled() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_QUERY_VALUE, &key)
        != ERROR_SUCCESS) {
        return false;
    }
    LONG r = RegQueryValueExW(key, RUN_VALUE, nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(key);
    return r == ERROR_SUCCESS;
}

bool enable() {
    auto path = quoted_exe_path();
    if (path.empty()) return false;

    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    LONG r = RegSetValueExW(
        key, RUN_VALUE, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(path.c_str()),
        static_cast<DWORD>((path.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return r == ERROR_SUCCESS;
}

bool disable() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &key)
        != ERROR_SUCCESS) {
        return true;  // already absent; treat as success
    }
    LONG r = RegDeleteValueW(key, RUN_VALUE);
    RegCloseKey(key);
    return r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND;
}

}  // namespace autostart

// -----------------------------------------------------------------------------
// About dialog. TaskDialogIndirect requires Common Controls v6 (see manifest
// pragma at the top of this file).
// -----------------------------------------------------------------------------
namespace {

HRESULT CALLBACK about_callback(HWND, UINT msg, WPARAM, LPARAM lParam, LONG_PTR) {
    if (msg == TDN_HYPERLINK_CLICKED) {
        ShellExecuteW(nullptr, L"open",
                      reinterpret_cast<LPCWSTR>(lParam),
                      nullptr, nullptr, SW_SHOWNORMAL);
    }
    return S_OK;
}

}  // namespace

void showAboutDialog(HWND parent) {
    HINSTANCE hinst = GetModuleHandleW(nullptr);
    HICON icon = (HICON)LoadImageW(hinst,
                                   MAKEINTRESOURCEW(IDI_APP_ICON),
                                   IMAGE_ICON,
                                   64, 64, LR_DEFAULTCOLOR);

    TASKDIALOGCONFIG cfg{};
    cfg.cbSize             = sizeof(cfg);
    cfg.hwndParent         = parent;
    cfg.hInstance          = hinst;
    cfg.dwFlags            = TDF_ENABLE_HYPERLINKS | TDF_ALLOW_DIALOG_CANCELLATION;
    if (icon) {
        cfg.dwFlags       |= TDF_USE_HICON_MAIN;
        cfg.hMainIcon      = icon;
    } else {
        cfg.pszMainIcon    = TD_INFORMATION_ICON;
    }
    cfg.pszWindowTitle     = L"About Fluid Wallpaper";
    cfg.pszMainInstruction = L"Fluid Wallpaper v0.1.0";
    cfg.pszContent =
        L"Real-time CUDA fluid simulation as a Windows desktop wallpaper.\n\n"
        L"Built by kritav.\n\n"
        L"<a href=\"https://github.com/kritav/fluid-wallpaper\">"
        L"View on GitHub</a>";
    cfg.dwCommonButtons    = TDCBF_OK_BUTTON;
    cfg.pfCallback         = about_callback;

    TaskDialogIndirect(&cfg, nullptr, nullptr, nullptr);
    if (icon) DestroyIcon(icon);
}
