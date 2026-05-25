#include "cuda_renderer.h"

#include <cuda_runtime.h>
#include <cuda_d3d11_interop.h>
#include <d3d11.h>
#include <cstdio>

#define CUDA_CHECK(call) do {                                              \
    cudaError_t _err = (call);                                             \
    if (_err != cudaSuccess) {                                             \
        fprintf(stderr, "CUDA error at %s:%d: %s\n",                       \
                __FILE__, __LINE__, cudaGetErrorString(_err));             \
    }                                                                      \
} while (0)

// Placeholder Phase-2 kernel: animated plasma into the shared surface.
// Phase 3 replaces this with the fluid sim's density write-out.
__global__ void animated_kernel(cudaSurfaceObject_t surface,
                                int width, int height, float time)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;

    float u = (float)x / (float)width;
    float v = (float)y / (float)height;

    float r = 0.5f + 0.5f * sinf(u * 10.0f + time);
    float g = 0.5f + 0.5f * sinf(v * 10.0f + time * 1.3f);
    float b = 0.5f + 0.5f * sinf((u + v) * 8.0f + time * 0.7f);

    uchar4 color = make_uchar4(
        (unsigned char)(r * 255.0f),
        (unsigned char)(g * 255.0f),
        (unsigned char)(b * 255.0f),
        255);

    // surf2Dwrite takes a byte offset for x, not a texel index.
    surf2Dwrite(color, surface, x * (int)sizeof(uchar4), y);
}

CudaRenderer::~CudaRenderer() {
    if (resource_) {
        cudaGraphicsUnregisterResource(resource_);
        resource_ = nullptr;
    }
}

bool CudaRenderer::init(ID3D11Texture2D* shared_texture, int width, int height) {
    width_  = width;
    height_ = height;

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
    return true;
}

void CudaRenderer::render(float time_seconds) {
    CUDA_CHECK(cudaGraphicsMapResources(1, &resource_));

    cudaArray_t array = nullptr;
    CUDA_CHECK(cudaGraphicsSubResourceGetMappedArray(&array, resource_, 0, 0));

    cudaResourceDesc res_desc{};
    res_desc.resType         = cudaResourceTypeArray;
    res_desc.res.array.array = array;

    cudaSurfaceObject_t surface = 0;
    CUDA_CHECK(cudaCreateSurfaceObject(&surface, &res_desc));

    dim3 block(16, 16);
    dim3 grid((width_  + block.x - 1) / block.x,
              (height_ + block.y - 1) / block.y);
    animated_kernel<<<grid, block>>>(surface, width_, height_, time_seconds);

    CUDA_CHECK(cudaDestroySurfaceObject(surface));
    CUDA_CHECK(cudaGraphicsUnmapResources(1, &resource_));
}
