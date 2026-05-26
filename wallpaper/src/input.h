#pragma once

// Mouse state sampled per-frame and normalized to the primary display so the
// fluid sim can read it without caring about screen resolution.
//
// Coordinates are in [0, 1] where (0,0) is top-left. Deltas are also in
// normalized units (a value of 0.01 == 1% of the screen width / height).
struct MouseInput {
    float x  = 0.5f;
    float y  = 0.5f;
    float dx = 0.0f;
    float dy = 0.0f;
    bool  active = false;
};

// Snapshot GetCursorPos() into the global "current" slot, shifting the
// previous "current" into "last" so the next getMouseInput() can compute a
// delta. Call once per frame, before getMouseInput().
void updateMouseState();

// Convert raw desktop pixel coords to normalized MouseInput. `active` is
// true iff the cursor moved at all since the previous frame; the fluid sim
// uses this as the trigger for dye/velocity injection.
MouseInput getMouseInput(int screenWidth, int screenHeight);
