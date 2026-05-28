// initguid.h must precede windows.h so DEFINE_GUID in <winnt.h> emits
// storage for GUID_MONITOR_POWER_ON instead of just declaring it extern.
#include <initguid.h>
#include <windows.h>
#include <shellapi.h>

#include "power.h"

#include <atomic>
#include <cstdio>

namespace {

// State flags. Atomic because handle_power_broadcast may be invoked from
// the wnd_proc on a thread other than main if the OS delivers the message
// asynchronously (it usually doesn't, but cheap insurance).
std::atomic<bool> g_monitor_on{true};
std::atomic<bool> g_obscured{false};
std::atomic<bool> g_fullscreen_app{false};
std::atomic<bool> g_on_battery{false};
std::atomic<bool> g_user_paused{false};

HWND          g_msg_window     = nullptr;
HPOWERNOTIFY  g_monitor_notify = nullptr;

std::chrono::steady_clock::time_point g_last_poll{};

void debug_log(const char* msg) {
#ifdef _DEBUG
    OutputDebugStringA(msg);
#else
    (void)msg;
#endif
}

void handle_power_broadcast(WPARAM wParam, LPARAM lParam) {
    if (wParam != PBT_POWERSETTINGCHANGE) return;
    auto* s = reinterpret_cast<POWERBROADCAST_SETTING*>(lParam);
    if (s->PowerSetting != GUID_MONITOR_POWER_ON) return;

    DWORD state = *reinterpret_cast<DWORD*>(s->Data);
    bool on = (state != 0);
    bool prev = g_monitor_on.exchange(on);
    if (prev != on) {
        debug_log(on ? "[power] monitor on\n" : "[power] monitor off\n");
    }
}

LRESULT CALLBACK msg_window_proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_POWERBROADCAST) {
        handle_power_broadcast(w, l);
        return TRUE;
    }
    return DefWindowProcW(hwnd, msg, w, l);
}

// Heuristic: a single foreground window fully covering the primary monitor.
// Misses tiled-windows-covering-the-desktop, which is intentional — handling
// that requires window-region intersection and isn't worth the complexity
// for a v1.
bool desktop_fully_obscured() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;

    wchar_t cls[256] = {0};
    GetClassNameW(fg, cls, 256);
    // Skip shell windows so the wallpaper doesn't think it's being obscured
    // by Explorer / itself / the taskbar.
    if (wcscmp(cls, L"Progman") == 0)                return false;
    if (wcscmp(cls, L"WorkerW") == 0)                return false;
    if (wcscmp(cls, L"Shell_TrayWnd") == 0)          return false;
    if (wcscmp(cls, L"FluidWallpaperWindow") == 0)   return false;

    HMONITOR mon = MonitorFromWindow(fg, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return false;

    RECT r;
    if (!GetWindowRect(fg, &r)) return false;

    return (r.left   <= mi.rcMonitor.left  &&
            r.top    <= mi.rcMonitor.top   &&
            r.right  >= mi.rcMonitor.right &&
            r.bottom >= mi.rcMonitor.bottom);
}

bool foreground_is_shell() {
    HWND fg = GetForegroundWindow();
    if (!fg) return true;  // no app foreground → treat as the desktop/shell
    wchar_t cls[256] = {0};
    GetClassNameW(fg, cls, 256);
    return wcscmp(cls, L"Progman") == 0
        || wcscmp(cls, L"WorkerW") == 0
        || wcscmp(cls, L"Shell_TrayWnd") == 0
        || wcscmp(cls, L"FluidWallpaperWindow") == 0;
}

bool fullscreen_app_running() {
    // Guard the desktop false-positive: SHQueryUserNotificationState reports a
    // busy / full-screen state whenever the *foreground window* is full-screen,
    // and the Windows desktop (Progman) is itself a full-screen window. Without
    // this check, clicking onto the desktop made the API say "fullscreen app
    // running", which paused the wallpaper exactly when it should be visible
    // (and it resumed when a normal windowed app was focused — the inverted
    // freeze). A real fullscreen app is never the shell, so bail out first.
    if (foreground_is_shell()) return false;

    QUERY_USER_NOTIFICATION_STATE state;
    HRESULT hr = SHQueryUserNotificationState(&state);
    if (FAILED(hr)) return false;
    return state == QUNS_RUNNING_D3D_FULL_SCREEN ||
           state == QUNS_PRESENTATION_MODE ||
           state == QUNS_BUSY;
}

bool on_battery_now() {
    SYSTEM_POWER_STATUS s;
    if (!GetSystemPowerStatus(&s)) return false;
    return s.ACLineStatus == 0;  // 0 = battery, 1 = AC, 255 = unknown
}

}  // namespace

namespace power {

bool register_notifications() {
    const wchar_t* CLS = L"FluidWallpaperPowerListener";
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = msg_window_proc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = CLS;
    RegisterClassExW(&wc);

    // HWND_MESSAGE: invisible message-only window. Not in the z-order, never
    // composited; exists solely to receive WM_POWERBROADCAST.
    g_msg_window = CreateWindowExW(
        0, CLS, L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (!g_msg_window) return false;

    g_monitor_notify = RegisterPowerSettingNotification(
        g_msg_window, &GUID_MONITOR_POWER_ON, DEVICE_NOTIFY_WINDOW_HANDLE);
    return g_monitor_notify != nullptr;
}

void unregister_notifications() {
    if (g_monitor_notify) {
        UnregisterPowerSettingNotification(g_monitor_notify);
        g_monitor_notify = nullptr;
    }
    if (g_msg_window) {
        DestroyWindow(g_msg_window);
        g_msg_window = nullptr;
    }
}

void poll() {
    auto now = std::chrono::steady_clock::now();
    if (now - g_last_poll < std::chrono::seconds(1)) return;
    g_last_poll = now;

    bool obs = desktop_fully_obscured();
    bool fs  = fullscreen_app_running();
    bool bat = on_battery_now();

    bool prev_obs = g_obscured.exchange(obs);
    bool prev_fs  = g_fullscreen_app.exchange(fs);
    bool prev_bat = g_on_battery.exchange(bat);

    if (prev_obs != obs || prev_fs != fs || prev_bat != bat) {
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "[power] %s %s %s %s\n",
                 obs ? "obscured"   : "visible",
                 fs  ? "fullscreen" : "windowed",
                 g_monitor_on.load() ? "monitor-on" : "monitor-off",
                 bat ? "battery"    : "ac");
        debug_log(buf);
    }
}

void set_user_paused(bool paused) { g_user_paused.store(paused); }
bool is_user_paused()             { return g_user_paused.load(); }

std::chrono::microseconds target_frame_time() {
    using namespace std::chrono;
    if (g_user_paused.load() || !g_monitor_on.load() || g_fullscreen_app.load()) {
        return duration_cast<microseconds>(milliseconds(500));  // 2 FPS heartbeat
    }
    if (g_obscured.load()) {
        return duration_cast<microseconds>(milliseconds(200));  // 5 FPS
    }
    if (g_on_battery.load()) {
        return microseconds(33333);                             // 30 FPS
    }
    return microseconds(16667);                                 // 60 FPS
}

bool should_skip_simulation() {
    return g_user_paused.load() || !g_monitor_on.load() ||
           g_fullscreen_app.load() || g_obscured.load();
}

bool should_skip_render() {
    // Currently identical to should_skip_simulation — the swap chain holds
    // the last presented frame, so when we stop simulating there's nothing
    // new to present. Kept as a separate predicate so we can decouple later
    // (e.g. if the user wants a low-FPS animated wallpaper while obscured).
    return should_skip_simulation();
}

}  // namespace power
