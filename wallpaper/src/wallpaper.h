#pragma once
#include <windows.h>

// Returns the WorkerW window that sits behind the desktop icons, asking
// Progman to spawn one if needed. Falls back to Progman itself if the
// WorkerW handoff fails (some Windows configurations).
HWND find_workerw();

// Creates a borderless popup the size of the primary monitor, parents it
// to `workerw`, and shows it. Returns the window handle (nullptr on failure).
HWND create_wallpaper_window(HWND workerw, HINSTANCE hinstance, int width, int height);
