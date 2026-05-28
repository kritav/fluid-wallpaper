#pragma once

#include <chrono>

// Power / visibility policy. The main loop polls poll() every frame; the
// module rate-limits the expensive checks (foreground window, fullscreen
// app, battery status) to once per second. Monitor on/off is event-driven
// via WM_POWERBROADCAST on a dedicated message-only window owned here.
//
// Policy precedence (highest first):
//   user_paused / monitor_off / fullscreen_app : skip everything,  2 FPS
//   desktop fully obscured                     : skip everything,  5 FPS
//   on battery                                 : run normally,    30 FPS
//   default                                    : run normally,    60 FPS
namespace power {

// Create the message-only window and register for GUID_MONITOR_POWER_ON
// notifications. Returns false if the window or the registration failed —
// callers can keep running without monitor-state detection in that case.
bool register_notifications();
void unregister_notifications();

// Run the rate-limited foreground/fullscreen/battery polls. Cheap to call
// every iteration; internally a no-op until ≥1 s has elapsed.
void poll();

// Reserved for a future tray menu. The main loop already honors this via
// should_skip_*.
void set_user_paused(bool paused);
bool is_user_paused();

std::chrono::microseconds target_frame_time();
bool should_skip_simulation();
bool should_skip_render();

}  // namespace power
