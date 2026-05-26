#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "cuda_renderer.h"
#include "input.h"
#include "renderer.h"
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

VisualMode nextMode(VisualMode m) {
    int n = static_cast<int>(m) + 1;
    if (n >= static_cast<int>(VisualMode::Count)) n = 0;
    return static_cast<VisualMode>(n);
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

    printf("Running at %dx%d.\n", width, height);
    printf("Hotkeys: M=mode  B=bloom  I=idle  C=clear  Esc=quit\n");

    RenderSettings settings;
    printf("[mode]  %s\n", visualModeName(settings.mode));
    printf("[bloom] %s\n", settings.bloom_enabled ? "on" : "off");
    printf("[idle]  %s\n", settings.idle_enabled ? "on" : "off");

    bool m_was = false, b_was = false, i_was = false, c_was = false, esc_was = false;

    auto start_time = std::chrono::steady_clock::now();
    auto last_time  = start_time;
    MSG msg{};
    while (!g_quit) {
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

        if (keyPressed(esc_was, VK_ESCAPE)) { g_quit = true; break; }
        if (keyPressed(m_was, 'M')) {
            settings.mode = nextMode(settings.mode);
            printf("[mode]  %s\n", visualModeName(settings.mode));
        }
        if (keyPressed(b_was, 'B')) {
            settings.bloom_enabled = !settings.bloom_enabled;
            printf("[bloom] %s\n", settings.bloom_enabled ? "on" : "off");
        }
        if (keyPressed(i_was, 'I')) {
            settings.idle_enabled = !settings.idle_enabled;
            printf("[idle]  %s\n", settings.idle_enabled ? "on" : "off");
        }
        if (keyPressed(c_was, 'C')) {
            cuda.clear();
            printf("[clear] simulation reset\n");
        }

        cuda.render(dt, mouse, settings);
        renderer.render();
    }

    DestroyWindow(hwnd);
    return 0;
}
