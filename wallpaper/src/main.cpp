#include <windows.h>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "cuda_renderer.h"
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

    printf("Running at %dx%d. Close this console to exit.\n", width, height);

    const auto start = std::chrono::steady_clock::now();
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
        const float t  = std::chrono::duration<float>(now - start).count();
        cuda.render(t);
        renderer.render();
    }

    DestroyWindow(hwnd);
    return 0;
}
