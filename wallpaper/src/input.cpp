#include "input.h"

#include <windows.h>
#include <cmath>

namespace {

POINT g_last_pos = {0, 0};
POINT g_curr_pos = {0, 0};
bool  g_initialized = false;

}  // namespace

void updateMouseState() {
    g_last_pos = g_curr_pos;
    GetCursorPos(&g_curr_pos);
    // First call: prime both slots so the first computed delta is zero
    // rather than (curr - 0).
    if (!g_initialized) {
        g_last_pos = g_curr_pos;
        g_initialized = true;
    }
}

MouseInput getMouseInput(int screenWidth, int screenHeight) {
    MouseInput m;
    m.x  = static_cast<float>(g_curr_pos.x) / static_cast<float>(screenWidth);
    m.y  = static_cast<float>(g_curr_pos.y) / static_cast<float>(screenHeight);
    m.dx = static_cast<float>(g_curr_pos.x - g_last_pos.x) / static_cast<float>(screenWidth);
    m.dy = static_cast<float>(g_curr_pos.y - g_last_pos.y) / static_cast<float>(screenHeight);
    m.active = (fabsf(m.dx) > 1e-6f) || (fabsf(m.dy) > 1e-6f);
    return m;
}
