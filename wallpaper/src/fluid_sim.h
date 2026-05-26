#pragma once

#include <cuda_runtime.h>

#include "input.h"  // MouseInput

// A single scalar field on the simulation grid. The same `cudaArray` is
// exposed via both a filtered texture object (for reads — advection's
// bilinear sample is a hardware op) and a surface object (for writes).
//
// Ping-pong is done by swapping whole Field structs with swapFields(); the
// texture/surface objects move with the array they were built on, so the
// next kernel launch picks up the new buffer without recreating handles.
struct Field {
    cudaArray_t         array   = nullptr;
    cudaTextureObject_t texObj  = 0;
    cudaSurfaceObject_t surfObj = 0;
    int                 width   = 0;
    int                 height  = 0;
};

// All simulation state. velX/velY/density use _tmp as the Jacobi/advection
// ping-pong target; pressure uses pressure_tmp; divergence is single-buffer
// (written once per project step, read-only after). scratch holds a frozen
// copy of velocity during diffusion (Jacobi reads from `orig` every sweep).
struct SimState {
    int width  = 0;
    int height = 0;

    Field velX,        velX_tmp;
    Field velY,        velY_tmp;
    Field density,     density_tmp;
    Field pressure,    pressure_tmp;
    Field divergence;
    Field scratch;
};

bool createField(Field& f, int width, int height);
void destroyField(Field& f);
void swapFields(Field& a, Field& b);
void copyField(Field& dst, const Field& src);
void clearField(Field& f, float value = 0.0f);

bool createSimState(SimState& s, int width, int height);
void destroySimState(SimState& s);

// One full Stam timestep: addForce → diffuse vel → project → advect vel →
// project → advect density → dissipate. Mirrors prototype/stable_fluids.py.
void stepSimulation(SimState& s, const MouseInput& mouse, float dt);

// Read the simulation density grid with bilinear filtering and write a
// grayscale RGBA8 image to the (typically larger) DX11 shared texture.
// Phase 4 will replace the grayscale colormap with something prettier.
void renderDensityToOutput(const Field& density,
                           cudaSurfaceObject_t outputSurf,
                           int outputWidth, int outputHeight);
