#include <windows.h>
#include <commctrl.h>  // TaskDialogIndirect for welcome / shortcuts dialogs
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include "cuda_renderer.h"
#include "input.h"
#include "power.h"
#include "renderer.h"
#include "tray.h"
#include "wallpaper.h"

namespace {

volatile bool g_quit = false;

BOOL WINAPI console_ctrl_handler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT ||
        type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT ||
        type == CTRL_SHUTDOWN_EVENT) {
        g_quit = true;
        return TRUE;
    }
    return FALSE;
}

// Edge-detected key press. GetAsyncKeyState's high bit is the current down
// state; we compare to last frame's so a held key fires only once. Caller
// owns the `was_down` flag (one per hotkey).
bool keyPressed(bool& was_down, int vk) {
    bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool pressed = down && !was_down;
    was_down = down;
    return pressed;
}

// GetAsyncKeyState is process-global — it reports keys pressed in *any* app,
// not just when the desktop is active. Without this gate, typing "M"/"B"/"C"
// in another window (e.g. a browser) would cycle the colormap, toggle bloom,
// or clear the sim. Only act on hotkeys when the desktop shell is foreground;
// the tray menu offers the same controls regardless of focus.
bool desktopIsForeground() {
    HWND fg = GetForegroundWindow();
    if (!fg) return true;  // nothing focused → treat as the desktop
    wchar_t cls[256] = {0};
    GetClassNameW(fg, cls, 256);
    return wcscmp(cls, L"Progman") == 0
        || wcscmp(cls, L"WorkerW") == 0
        || wcscmp(cls, L"FluidWallpaperWindow") == 0;
}

VisualMode nextMode(VisualMode m) {
    int n = static_cast<int>(m) + 1;
    if (n >= static_cast<int>(VisualMode::Count)) n = 0;
    return static_cast<VisualMode>(n);
}

// First-run state: REG_DWORD HasRunBefore under HKCU\Software\kritav\
// FluidWallpaper. Uninstall removes the whole subkey, so reinstalling
// triggers the welcome again.
constexpr const wchar_t* FIRST_RUN_KEY   = L"Software\\kritav\\FluidWallpaper";
constexpr const wchar_t* FIRST_RUN_VALUE = L"HasRunBefore";

bool isFirstRun() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, FIRST_RUN_KEY, 0, KEY_QUERY_VALUE, &key)
        != ERROR_SUCCESS) {
        return true;
    }
    DWORD value = 0;
    DWORD size  = sizeof(value);
    LONG  r     = RegQueryValueExW(key, FIRST_RUN_VALUE, nullptr, nullptr,
                                   reinterpret_cast<LPBYTE>(&value), &size);
    RegCloseKey(key);
    return r != ERROR_SUCCESS || value == 0;
}

void markFirstRunComplete() {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, FIRST_RUN_KEY, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return;
    }
    DWORD value = 1;
    RegSetValueExW(key, FIRST_RUN_VALUE, 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
}

void showShortcutsDialog(HWND parent) {
    TASKDIALOGCONFIG cfg{};
    cfg.cbSize             = sizeof(cfg);
    cfg.hwndParent         = parent;
    cfg.hInstance          = GetModuleHandleW(nullptr);
    cfg.dwFlags            = TDF_ALLOW_DIALOG_CANCELLATION;
    cfg.pszMainIcon        = TD_INFORMATION_ICON;
    cfg.pszWindowTitle     = L"Fluid Wallpaper Shortcuts";
    cfg.pszMainInstruction = L"Keyboard shortcuts";
    cfg.pszContent =
        L"M  —  cycle visual mode (plasma / inferno / viridis / cool / "
        L"velocity / velocity-colored)\n"
        L"B  —  toggle bloom\n"
        L"I  —  toggle idle motion\n"
        L"C  —  clear the simulation\n"
        L"Esc  —  quit\n\n"
        L"All of the above are also in the tray icon's right-click menu.";
    cfg.dwCommonButtons    = TDCBF_OK_BUTTON;
    TaskDialogIndirect(&cfg, nullptr, nullptr, nullptr);
}

void showWelcomeDialog(HWND parent) {
    // Custom buttons so we can offer "Show keyboard shortcuts" alongside OK.
    const TASKDIALOG_BUTTON buttons[] = {
        {1001, L"Got it"},
        {1002, L"Show keyboard shortcuts"},
    };
    TASKDIALOGCONFIG cfg{};
    cfg.cbSize             = sizeof(cfg);
    cfg.hwndParent         = parent;
    cfg.hInstance          = GetModuleHandleW(nullptr);
    cfg.dwFlags            = TDF_ALLOW_DIALOG_CANCELLATION;
    cfg.pszMainIcon        = TD_INFORMATION_ICON;
    cfg.pszWindowTitle     = L"Welcome to Fluid Wallpaper";
    cfg.pszMainInstruction = L"Fluid Wallpaper is now running.";
    cfg.pszContent =
        L"Move your mouse around the desktop to see the fluid react.\n\n"
        L"Right-click the fluid icon in the system tray (near the clock) "
        L"for options like pause, clear, autostart, and exit.\n\n"
        L"Press M to cycle visual modes, B to toggle bloom, I to toggle "
        L"idle motion.";
    cfg.cButtons       = ARRAYSIZE(buttons);
    cfg.pButtons       = buttons;
    cfg.nDefaultButton = 1001;

    int clicked = 0;
    TaskDialogIndirect(&cfg, &clicked, nullptr, nullptr);
    if (clicked == 1002) showShortcutsDialog(parent);
}

}  // namespace

int main(int argc, char** argv) {
    // Diagnostic mode: --standalone renders into a normal half-screen
    // popup window without touching WorkerW. Use this to verify the
    // renderer in isolation
    bool standalone = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--standalone") == 0) standalone = true;
    }

    // GUI-subsystem build: no console on a normal launch, so the app runs
    // silently and won't be killed when a terminal closes. --standalone spins
    // up a console so printf diagnostics (and Ctrl+C) are available.
    if (standalone && AllocConsole()) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);

    HWND workerw = nullptr;
    if (!standalone) {
        workerw = find_workerw();
        if (!workerw) {
            fprintf(stderr, "Could not locate WorkerW window. Aborting.\n");
            return 1;
        }
    }

    int width  = GetSystemMetrics(SM_CXSCREEN);
    int height = GetSystemMetrics(SM_CYSCREEN);
    if (standalone) {
        width  /= 2;
        height /= 2;
    }

    HINSTANCE hinst = GetModuleHandleW(nullptr);
    HWND hwnd = create_wallpaper_window(workerw, hinst, width, height);
    if (!hwnd) {
        fprintf(stderr, "Failed to create wallpaper window.\n");
        return 1;
    }

    Renderer renderer;
    if (!renderer.init(hwnd, width, height)) {
        fprintf(stderr, "Renderer init failed.\n");
        DestroyWindow(hwnd);
        return 1;
    }

    CudaRenderer cuda;
    if (!cuda.init(renderer.shared_texture(), width, height)) {
        fprintf(stderr, "CUDA renderer init failed.\n");
        DestroyWindow(hwnd);
        return 1;
    }

    // Monitor-power notifications are delivered to a hidden message-only
    // window owned by power.cpp. The wallpaper window is a WS_CHILD of
    // WorkerW, so it can't reliably receive WM_POWERBROADCAST itself.
    if (!power::register_notifications()) {
        fprintf(stderr, "power::register_notifications failed (continuing "
                        "without monitor-state detection)\n");
    }

    // Tray icon + right-click menu. Same WS_CHILD reasoning as power: tray
    // owns a hidden top-level window for the notify-icon callback.
    tray::Commands tray_cmds;
    tray::State    tray_state;
    if (!tray::init(hinst, &tray_cmds, &tray_state)) {
        fprintf(stderr, "tray::init failed (continuing without tray icon)\n");
    }

    printf("Running at %dx%d.\n", width, height);
    printf("Hotkeys: M=mode  B=bloom  I=idle  C=clear  Esc=quit\n");

    RenderSettings settings;
    printf("[mode]  %s\n", visualModeName(settings.mode));
    printf("[bloom] %s\n", settings.bloom_enabled ? "on" : "off");
    printf("[idle]  %s\n", settings.idle_enabled ? "on" : "off");

    bool m_was = false, b_was = false, i_was = false, c_was = false, esc_was = false;
    bool first_run_pending = isFirstRun();
    bool any_frame_drawn   = false;

    auto start_time = std::chrono::steady_clock::now();
    auto last_time  = start_time;
    MSG msg{};
    while (!g_quit) {
        auto frame_start = std::chrono::steady_clock::now();

        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g_quit = true;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (g_quit) break;

        const auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - last_time).count();
        last_time = now;
        // Clamp dt tight: above 1/60s a single advection step can blow up
        // the stable-fluids solver (manifests as bright sludge sliding to
        // a corner before dissipation pulls it back). Cap at one 60 Hz
        // frame even if rendering hitches; floor avoids degenerate zero.
        dt = std::clamp(dt, 1.0f / 240.0f, 1.0f / 60.0f);

        updateMouseState();
        MouseInput mouse = getMouseInput(width, height);

        // Reset the idle counter on any cursor motion; otherwise tick up.
        if (mouse.active) settings.seconds_idle = 0.0f;
        else              settings.seconds_idle += dt;
        settings.wall_time = std::chrono::duration<float>(now - start_time).count();

        // Advance edge-detection every frame regardless of focus so a key
        // held across a focus change can't fire a stale edge later; only act
        // on the press when the desktop itself is foreground.
        bool esc_p = keyPressed(esc_was, VK_ESCAPE);
        bool m_p   = keyPressed(m_was, 'M');
        bool b_p   = keyPressed(b_was, 'B');
        bool i_p   = keyPressed(i_was, 'I');
        bool c_p   = keyPressed(c_was, 'C');
        if (desktopIsForeground()) {
            if (esc_p) { g_quit = true; break; }
            if (m_p) {
                settings.mode = nextMode(settings.mode);
                printf("[mode]  %s\n", visualModeName(settings.mode));
            }
            if (b_p) {
                settings.bloom_enabled = !settings.bloom_enabled;
                printf("[bloom] %s\n", settings.bloom_enabled ? "on" : "off");
            }
            if (i_p) {
                settings.idle_enabled = !settings.idle_enabled;
                printf("[idle]  %s\n", settings.idle_enabled ? "on" : "off");
            }
            if (c_p) {
                cuda.clear();
                printf("[clear] simulation reset\n");
            }
        }

        // Tray menu reads these to draw checkmarks; sync before draining
        // commands so a freshly-opened menu shows correct state.
        tray_state.paused.store(power::is_user_paused());
        tray_state.bloom_enabled.store(settings.bloom_enabled);
        tray_state.idle_enabled.store(settings.idle_enabled);

        if (tray_cmds.quit.exchange(false))         { g_quit = true; break; }
        if (tray_cmds.toggle_pause.exchange(false)) {
            bool now_paused = !power::is_user_paused();
            power::set_user_paused(now_paused);
            printf("[pause] %s\n", now_paused ? "paused" : "resumed");
        }
        if (tray_cmds.clear.exchange(false)) {
            cuda.clear();
            printf("[clear] simulation reset\n");
        }
        if (tray_cmds.next_mode.exchange(false)) {
            settings.mode = nextMode(settings.mode);
            printf("[mode]  %s\n", visualModeName(settings.mode));
        }
        if (tray_cmds.toggle_bloom.exchange(false)) {
            settings.bloom_enabled = !settings.bloom_enabled;
            printf("[bloom] %s\n", settings.bloom_enabled ? "on" : "off");
        }
        if (tray_cmds.toggle_idle.exchange(false)) {
            settings.idle_enabled = !settings.idle_enabled;
            printf("[idle]  %s\n", settings.idle_enabled ? "on" : "off");
        }
        if (tray_cmds.toggle_autostart.exchange(false)) {
            bool now_enabled;
            if (autostart::is_enabled()) {
                autostart::disable();
                now_enabled = false;
            } else {
                autostart::enable();
                now_enabled = true;
            }
            printf("[autostart] %s\n", now_enabled ? "enabled" : "disabled");
        }
        if (tray_cmds.show_about.exchange(false)) {
            showAboutDialog(hwnd);
        }

        power::poll();

        // First-run welcome: fires after the first frame is already on screen
        // so the user sees the wallpaper running behind the dialog instead of
        // a blank desktop.
        if (first_run_pending && any_frame_drawn) {
            showWelcomeDialog(hwnd);
            markFirstRunComplete();
            first_run_pending = false;
        }

        // Skip both sim and render when nobody can see us. DX11's flip
        // swap chain holds the last presented frame, so the desktop keeps
        // showing the most recent image without us doing anything.
        if (!power::should_skip_simulation()) {
            cuda.render(dt, mouse, settings);
        }
        if (!power::should_skip_render()) {
            renderer.render();
            any_frame_drawn = true;
        }

        // Explicit frame pacing. With vsync disabled in Present() this is
        // the only thing keeping us at 60 FPS; on a 144 Hz monitor without
        // this we'd happily render 144 frames/sec for no benefit. When
        // obscured/paused the target stretches to 200–500 ms.
        auto target  = power::target_frame_time();
        auto elapsed = std::chrono::steady_clock::now() - frame_start;
        if (elapsed < target) {
            std::this_thread::sleep_for(target - elapsed);
        }
    }

    tray::shutdown();
    power::unregister_notifications();
    DestroyWindow(hwnd);
    return 0;
}
