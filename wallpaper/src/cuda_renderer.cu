#include "cuda_renderer.h"

#include <cuda_runtime.h>
#include <cuda_d3d11_interop.h>
#include <d3d11.h>
#include <cstdio>
#include <cmath>

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

// Bloom tunables. Down-sample factor of 4 turns a 1080p blur into a 270p
// blur (16x cheaper) without hurting the look — bloom is supposed to be
// soft. Radius 12 at 1/4 res ≈ effective radius 48 at full res.
constexpr int   BLOOM_DOWNSCALE = 4;
constexpr int   BLOOM_RADIUS    = 12;
constexpr float BLOOM_SIGMA     = 6.0f;
constexpr float BLOOM_THRESHOLD = 0.35f;
constexpr float BLOOM_STRENGTH  = 0.7f;

const dim3 BLOCK(16, 16);
inline dim3 launchGrid(int w, int h) {
    return dim3((w + BLOCK.x - 1) / BLOCK.x, (h + BLOCK.y - 1) / BLOCK.y);
}

// Allocate a float4 cudaArray with linked tex/surf objects. Used twice (one
// for each ping-pong bloom buffer).
bool createFloat4Buffer(cudaArray_t& array,
                        cudaTextureObject_t& tex,
                        cudaSurfaceObject_t& surf,
                        int width, int height) {
    cudaChannelFormatDesc cd = cudaCreateChannelDesc<float4>();
    cudaError_t err = cudaMallocArray(&array, &cd, width, height,
                                      cudaArraySurfaceLoadStore);
    if (err != cudaSuccess) {
        fprintf(stderr, "bloom cudaMallocArray failed: %s\n",
                cudaGetErrorString(err));
        return false;
    }
    cudaResourceDesc rd{};
    rd.resType         = cudaResourceTypeArray;
    rd.res.array.array = array;

    cudaTextureDesc td{};
    td.addressMode[0]   = cudaAddressModeClamp;
    td.addressMode[1]   = cudaAddressModeClamp;
    td.filterMode       = cudaFilterModeLinear;
    td.readMode         = cudaReadModeElementType;
    td.normalizedCoords = 1;

    err = cudaCreateTextureObject(&tex, &rd, &td, nullptr);
    if (err != cudaSuccess) {
        fprintf(stderr, "bloom cudaCreateTextureObject failed: %s\n",
                cudaGetErrorString(err));
        return false;
    }
    err = cudaCreateSurfaceObject(&surf, &rd);
    if (err != cudaSuccess) {
        fprintf(stderr, "bloom cudaCreateSurfaceObject failed: %s\n",
                cudaGetErrorString(err));
        return false;
    }
    return true;
}

void destroyFloat4Buffer(cudaArray_t& array,
                         cudaTextureObject_t& tex,
                         cudaSurfaceObject_t& surf) {
    if (tex)   { cudaDestroyTextureObject(tex);  tex = 0; }
    if (surf)  { cudaDestroySurfaceObject(surf); surf = 0; }
    if (array) { cudaFreeArray(array);           array = nullptr; }
}

// --- Bloom kernels ---------------------------------------------------------

// Down-sample + luminance threshold. One bloom-pixel reads one
// representative output pixel from its 4x4 block — exact mean would be
// slightly cleaner but for soft bloom the difference is invisible.
__global__ void extractBrightKernel(
    cudaSurfaceObject_t outSurf,
    cudaSurfaceObject_t bloomSurf,
    int outW, int outH,
    int bloomW, int bloomH,
    float threshold)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= bloomW || y >= bloomH) return;

    int srcX = (x * outW) / bloomW + (outW / bloomW) / 2;
    int srcY = (y * outH) / bloomH + (outH / bloomH) / 2;
    if (srcX >= outW) srcX = outW - 1;
    if (srcY >= outH) srcY = outH - 1;

    uchar4 px;
    surf2Dread(&px, outSurf, srcX * static_cast<int>(sizeof(uchar4)), srcY);

    float r = px.x * (1.0f / 255.0f);
    float g = px.y * (1.0f / 255.0f);
    float b = px.z * (1.0f / 255.0f);
    float lum = 0.299f * r + 0.587f * g + 0.114f * b;

    // Soft knee — fade in over the threshold instead of hard-clipping.
    float t = fmaxf(0.0f, lum - threshold) / fmaxf(1e-3f, 1.0f - threshold);
    float4 bright = make_float4(r * t, g * t, b * t, 1.0f);
    surf2Dwrite(bright, bloomSurf, x * static_cast<int>(sizeof(float4)), y);
}

// Separable 1D gaussian blur. Caller picks the axis with `horizontal`.
// Sigma is small relative to radius so the tails are negligible but the
// center has a clean falloff.
__global__ void gaussianBlurKernel(
    cudaTextureObject_t srcTex,
    cudaSurfaceObject_t dstSurf,
    int width, int height,
    int radius, float sigma,
    int horizontal)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;

    float u  = (x + 0.5f) / width;
    float v  = (y + 0.5f) / height;
    float du = horizontal ? 1.0f / width  : 0.0f;
    float dv = horizontal ? 0.0f          : 1.0f / height;

    float r = 0, g = 0, b = 0, wsum = 0;
    float inv2s2 = 1.0f / (2.0f * sigma * sigma);
    for (int i = -radius; i <= radius; ++i) {
        float w  = expf(-static_cast<float>(i * i) * inv2s2);
        float4 s = tex2D<float4>(srcTex, u + i * du, v + i * dv);
        r += s.x * w;
        g += s.y * w;
        b += s.z * w;
        wsum += w;
    }
    float inv = 1.0f / wsum;
    float4 outVal = make_float4(r * inv, g * inv, b * inv, 1.0f);
    surf2Dwrite(outVal, dstSurf, x * static_cast<int>(sizeof(float4)), y);
}

// Bilinear-upsample bloom and add to the output texture. Reads bloom via
// texture (free 4x upsample), reads/writes output via surface.
__global__ void addBloomKernel(
    cudaSurfaceObject_t outSurf,
    cudaTextureObject_t bloomTex,
    int outW, int outH,
    float strength)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= outW || y >= outH) return;

    float u = (x + 0.5f) / outW;
    float v = (y + 0.5f) / outH;
    float4 b = tex2D<float4>(bloomTex, u, v);

    uchar4 px;
    surf2Dread(&px, outSurf, x * static_cast<int>(sizeof(uchar4)), y);

    float r = px.x * (1.0f / 255.0f) + b.x * strength;
    float g = px.y * (1.0f / 255.0f) + b.y * strength;
    float bb = px.z * (1.0f / 255.0f) + b.z * strength;

    px.x = static_cast<unsigned char>(fminf(r,  1.0f) * 255.0f);
    px.y = static_cast<unsigned char>(fminf(g,  1.0f) * 255.0f);
    px.z = static_cast<unsigned char>(fminf(bb, 1.0f) * 255.0f);
    px.w = 255;
    surf2Dwrite(px, outSurf, x * static_cast<int>(sizeof(uchar4)), y);
}

}  // namespace

CudaRenderer::~CudaRenderer() {
    destroyFloat4Buffer(bloom_a_array_, bloom_a_tex_, bloom_a_surf_);
    destroyFloat4Buffer(bloom_b_array_, bloom_b_tex_, bloom_b_surf_);
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

    bloom_width_  = display_width  / BLOOM_DOWNSCALE;
    bloom_height_ = display_height / BLOOM_DOWNSCALE;
    if (!createFloat4Buffer(bloom_a_array_, bloom_a_tex_, bloom_a_surf_,
                            bloom_width_, bloom_height_)) return false;
    if (!createFloat4Buffer(bloom_b_array_, bloom_b_tex_, bloom_b_surf_,
                            bloom_width_, bloom_height_)) return false;
    return true;
}

void CudaRenderer::clear() {
    clearSimState(sim_);
}

void CudaRenderer::render(float dt, const MouseInput& mouse,
                          const RenderSettings& settings) {
    // 1. Idle perturbation (before the step so it propagates through diffuse
    //    + project in the same frame). Threshold is hard-coded at 3 seconds.
    if (settings.idle_enabled && settings.seconds_idle > 3.0f) {
        injectIdlePerturbation(sim_, settings.wall_time);
    }

    // 2. Advance the simulation. All work on private CUDA arrays — does not
    //    touch the DX11 texture.
    stepSimulation(sim_, mouse, dt);

    // 3. Map the DX11 shared texture and write into it. The cudaArray
    //    pointer is only valid between Map/Unmap, so the output surface
    //    object is built and torn down each frame.
    CUDA_CHECK(cudaGraphicsMapResources(1, &resource_));

    cudaArray_t out_array = nullptr;
    CUDA_CHECK(cudaGraphicsSubResourceGetMappedArray(&out_array, resource_, 0, 0));

    cudaResourceDesc rd{};
    rd.resType         = cudaResourceTypeArray;
    rd.res.array.array = out_array;

    cudaSurfaceObject_t out_surf = 0;
    CUDA_CHECK(cudaCreateSurfaceObject(&out_surf, &rd));

    // 4. Tone-map + colormap the sim into RGBA8 at display resolution.
    renderSimToOutput(sim_, out_surf,
                      display_width_, display_height_,
                      settings.mode);

    // 5. Bloom post-process (extract → blur H → blur V → add). All work in
    //    the 1/4-res float4 ping-pong buffers; only the final add touches
    //    the output texture.
    if (settings.bloom_enabled) {
        dim3 bloom_grid = launchGrid(bloom_width_, bloom_height_);
        extractBrightKernel<<<bloom_grid, BLOCK>>>(
            out_surf, bloom_a_surf_,
            display_width_, display_height_,
            bloom_width_, bloom_height_,
            BLOOM_THRESHOLD);

        gaussianBlurKernel<<<bloom_grid, BLOCK>>>(
            bloom_a_tex_, bloom_b_surf_,
            bloom_width_, bloom_height_,
            BLOOM_RADIUS, BLOOM_SIGMA, /*horizontal=*/1);
        gaussianBlurKernel<<<bloom_grid, BLOCK>>>(
            bloom_b_tex_, bloom_a_surf_,
            bloom_width_, bloom_height_,
            BLOOM_RADIUS, BLOOM_SIGMA, /*horizontal=*/0);

        addBloomKernel<<<launchGrid(display_width_, display_height_), BLOCK>>>(
            out_surf, bloom_a_tex_,
            display_width_, display_height_,
            BLOOM_STRENGTH);
    }

    CUDA_CHECK(cudaDestroySurfaceObject(out_surf));
    CUDA_CHECK(cudaGraphicsUnmapResources(1, &resource_));
}
