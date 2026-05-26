#include "cuda_renderer.h"

#include <cuda_runtime.h>
#include <cuda_d3d11_interop.h>
#include <d3d11.h>
#include <cstdio>

#include "input.h"

#define CUDA_CHECK(call) do {                                              \
    cudaError_t _err = (call);                                             \
    if (_err != cudaSuccess) {                                             \
        fprintf(stderr, "CUDA error at %s:%d: %s\n",                       \
                __FILE__, __LINE__, cudaGetErrorString(_err));             \
    }                                                                      \
} while (0)

// Phase 3 sim grid resolution. Display resolution is independent and passed
// at init() time; the render kernel upsamples via hardware bilinear filter.
// 512^2 hits the 60 FPS target on a Turing+ GPU with comfortable headroom.
namespace {
constexpr int SIM_WIDTH  = 512;
constexpr int SIM_HEIGHT = 512;
}

CudaRenderer::~CudaRenderer() {
    destroySimState(sim_);
    if (resource_) {
        cudaGraphicsUnregisterResource(resource_);
        resource_ = nullptr;
    }
}

bool CudaRenderer::init(ID3D11Texture2D* shared_texture,
                        int display_width, int display_height) {
    display_width_  = display_width;
    display_height_ = display_height;

    // Known issue, deferred: on multi-GPU systems CUDA may pick a different
    // device than DXGI. The fix is to read the DXGI adapter LUID and call
    // cudaSetDevice on the CUDA device whose cudaDeviceProp::luid matches.
    cudaError_t err = cudaGraphicsD3D11RegisterResource(
        &resource_, shared_texture,
        cudaGraphicsRegisterFlagsSurfaceLoadStore);
    if (err != cudaSuccess) {
        fprintf(stderr, "cudaGraphicsD3D11RegisterResource failed: %s\n",
                cudaGetErrorString(err));
        resource_ = nullptr;
        return false;
    }

    if (!createSimState(sim_, SIM_WIDTH, SIM_HEIGHT)) {
        fprintf(stderr, "createSimState failed\n");
        return false;
    }
    return true;
}

void CudaRenderer::render(float dt, const MouseInput& mouse) {
    // 1. Advance the simulation. All work on private CUDA arrays — does not
    //    touch the DX11 texture.
    stepSimulation(sim_, mouse, dt);

    // 2. Map the DX11 shared texture and write the density grid into it.
    //    The cudaArray pointer is only valid between Map/Unmap, so the
    //    output surface object is built and torn down each frame.
    CUDA_CHECK(cudaGraphicsMapResources(1, &resource_));

    cudaArray_t out_array = nullptr;
    CUDA_CHECK(cudaGraphicsSubResourceGetMappedArray(&out_array, resource_, 0, 0));

    cudaResourceDesc rd{};
    rd.resType         = cudaResourceTypeArray;
    rd.res.array.array = out_array;

    cudaSurfaceObject_t out_surf = 0;
    CUDA_CHECK(cudaCreateSurfaceObject(&out_surf, &rd));

    renderDensityToOutput(sim_.density, out_surf,
                          display_width_, display_height_);

    CUDA_CHECK(cudaDestroySurfaceObject(out_surf));
    CUDA_CHECK(cudaGraphicsUnmapResources(1, &resource_));
}
