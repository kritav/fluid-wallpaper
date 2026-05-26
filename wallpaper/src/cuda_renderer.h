#pragma once

#include <cuda_runtime.h>

#include "fluid_sim.h"

struct ID3D11Texture2D;
struct cudaGraphicsResource;
struct MouseInput;

// Per-frame visual settings driven by hotkeys in main.cpp.
struct RenderSettings {
    VisualMode mode           = VisualMode::DensityPlasma;
    bool       bloom_enabled  = true;
    bool       idle_enabled   = true;
    float      seconds_idle   = 0.0f;   // time since cursor last moved
    float      wall_time      = 0.0f;   // monotonic seconds since launch
};

// Owns the simulation state, the CUDA-registered handle to the DX11 shared
// texture, and the down-sampled float4 bloom buffers. Each frame: optionally
// inject idle perturbations, step the sim, map the DX11 texture, run the
// render kernel, optionally apply bloom, unmap.
class CudaRenderer {
public:
    CudaRenderer() = default;
    ~CudaRenderer();

    CudaRenderer(const CudaRenderer&)            = delete;
    CudaRenderer& operator=(const CudaRenderer&) = delete;

    bool init(ID3D11Texture2D* shared_texture,
              int display_width, int display_height);
    void render(float dt, const MouseInput& mouse, const RenderSettings& settings);

    // Hotkey C: zero all sim fields. Display goes black on the next frame.
    void clear();

private:
    cudaGraphicsResource* resource_       = nullptr;
    int                   display_width_  = 0;
    int                   display_height_ = 0;
    SimState              sim_{};

    // Down-sampled float4 bloom workspace (display / 4 on each axis). Two
    // buffers for ping-pong gaussian blur: horizontal pass writes a→b,
    // vertical pass writes b→a.
    int                   bloom_width_  = 0;
    int                   bloom_height_ = 0;
    cudaArray_t           bloom_a_array_ = nullptr;
    cudaTextureObject_t   bloom_a_tex_   = 0;
    cudaSurfaceObject_t   bloom_a_surf_  = 0;
    cudaArray_t           bloom_b_array_ = nullptr;
    cudaTextureObject_t   bloom_b_tex_   = 0;
    cudaSurfaceObject_t   bloom_b_surf_  = 0;
};
