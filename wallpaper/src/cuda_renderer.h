#pragma once

#include "fluid_sim.h"

struct ID3D11Texture2D;
struct cudaGraphicsResource;
struct MouseInput;

// Owns the simulation state plus the CUDA-registered handle to the DX11
// shared texture. Each frame: map the DX11 texture, run one stepSimulation,
// upsample density into the texture via renderDensityToOutput, unmap.
//
// The DX11 texture itself lives in Renderer; this class only borrows a raw
// pointer to register it with CUDA at init and unregister at shutdown.
class CudaRenderer {
public:
    CudaRenderer() = default;
    ~CudaRenderer();

    CudaRenderer(const CudaRenderer&)            = delete;
    CudaRenderer& operator=(const CudaRenderer&) = delete;

    bool init(ID3D11Texture2D* shared_texture,
              int display_width, int display_height);
    void render(float dt, const MouseInput& mouse);

private:
    cudaGraphicsResource* resource_       = nullptr;
    int                   display_width_  = 0;
    int                   display_height_ = 0;
    SimState              sim_{};
};
