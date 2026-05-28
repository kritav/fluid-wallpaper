#pragma once

#include <windows.h>
#include <atomic>

// System-tray icon + right-click menu. The menu posts edge-triggered commands
// the main loop drains each frame; the main loop posts back current toggle
// state so the menu can draw checkmarks.
namespace tray {

// Edge-triggered "do this once" commands set by menu clicks, drained by main.
struct Commands {
    std::atomic<bool> quit{false};
    std::atomic<bool> toggle_pause{false};
    std::atomic<bool> clear{false};
    std::atomic<bool> next_mode{false};
    std::atomic<bool> toggle_bloom{false};
    std::atomic<bool> toggle_idle{false};
    std::atomic<bool> toggle_autostart{false};
    std::atomic<bool> show_about{false};
};

// Snapshot of toggle state used to draw checkmarks in the menu. Main loop
// keeps these up to date as settings change.
struct State {
    std::atomic<bool> paused{false};
    std::atomic<bool> bloom_enabled{true};
    std::atomic<bool> idle_enabled{true};
};

bool init(HINSTANCE hinst, Commands* commands, State* state);
void shutdown();

}  // namespace tray

// Registry-backed autostart toggle, shared between the tray menu and the
// installer's optional autostart task.
namespace autostart {
bool is_enabled();
bool enable();
bool disable();
}

// Modal "About" dialog. Exposed for the tray menu and any future caller.
void showAboutDialog(HWND parent);
