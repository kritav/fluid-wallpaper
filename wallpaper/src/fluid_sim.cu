#include "fluid_sim.h"

#include <cstdio>
#include <cmath>

#define CUDA_CHECK(call) do {                                              \
    cudaError_t _err = (call);                                             \
    if (_err != cudaSuccess) {                                             \
        fprintf(stderr, "CUDA error at %s:%d (%s): %s\n",                  \
                __FILE__, __LINE__, #call, cudaGetErrorString(_err));      \
    }                                                                      \
} while (0)

// All sim-side quantities are in normalized-coordinate units. Velocity has
// units of "normalized lengths per second" — a value of 1.0 crosses the
// whole grid in one second. Advection is then simply prevU = u - vx*dt
// (no width/height factor), which removes one of Phase 3's classic bugs.
namespace tuning {
constexpr float VISCOSITY        = 0.00001f;   // mild velocity diffusion
constexpr float DENSITY_DECAY    = 0.997f;     // per-frame multiplicative fade
constexpr float VELOCITY_DECAY   = 0.999f;     // bleeds off kinetic energy that would otherwise accumulate and periodically blow up the solver
constexpr float MOUSE_FORCE      = 500.0f;     // dx in [0,1] -> velocity bump
constexpr float MOUSE_DENSITY    = 1.5f;       // dye added per moving frame
constexpr float INJECTION_RADIUS = 0.03f;      // gaussian sigma, normalized
constexpr int   DIFFUSION_ITERS  = 20;
constexpr int   PRESSURE_ITERS   = 80;
}  // namespace tuning

namespace {
const dim3 BLOCK(16, 16);
inline dim3 launchGrid(int w, int h) {
    return dim3((w + BLOCK.x - 1) / BLOCK.x, (h + BLOCK.y - 1) / BLOCK.y);
}
}  // namespace

// ---------------------------------------------------------------------------
// Field lifecycle
// ---------------------------------------------------------------------------

bool createField(Field& f, int width, int height) {
    f.width  = width;
    f.height = height;

    cudaChannelFormatDesc cd = cudaCreateChannelDesc<float>();
    cudaError_t err = cudaMallocArray(&f.array, &cd, width, height,
                                      cudaArraySurfaceLoadStore);
    if (err != cudaSuccess) {
        fprintf(stderr, "cudaMallocArray failed: %s\n", cudaGetErrorString(err));
        return false;
    }

    cudaResourceDesc rd{};
    rd.resType         = cudaResourceTypeArray;
    rd.res.array.array = f.array;

    cudaTextureDesc td{};
    td.addressMode[0]   = cudaAddressModeClamp;
    td.addressMode[1]   = cudaAddressModeClamp;
    td.filterMode       = cudaFilterModeLinear;
    td.readMode         = cudaReadModeElementType;
    td.normalizedCoords = 1;

    err = cudaCreateTextureObject(&f.texObj, &rd, &td, nullptr);
    if (err != cudaSuccess) {
        fprintf(stderr, "cudaCreateTextureObject failed: %s\n",
                cudaGetErrorString(err));
        return false;
    }
    err = cudaCreateSurfaceObject(&f.surfObj, &rd);
    if (err != cudaSuccess) {
        fprintf(stderr, "cudaCreateSurfaceObject failed: %s\n",
                cudaGetErrorString(err));
        return false;
    }
    return true;
}

void destroyField(Field& f) {
    if (f.texObj)  { cudaDestroyTextureObject(f.texObj); f.texObj = 0; }
    if (f.surfObj) { cudaDestroySurfaceObject(f.surfObj); f.surfObj = 0; }
    if (f.array)   { cudaFreeArray(f.array); f.array = nullptr; }
    f.width = 0; f.height = 0;
}

void swapFields(Field& a, Field& b) {
    Field t = a; a = b; b = t;
}

void copyField(Field& dst, const Field& src) {
    CUDA_CHECK(cudaMemcpy2DArrayToArray(
        dst.array, 0, 0, src.array, 0, 0,
        src.width * sizeof(float), src.height,
        cudaMemcpyDeviceToDevice));
}

// ---------------------------------------------------------------------------
// Clear kernel (cudaMemset2D doesn't work on cudaArrays — use a surface write)
// ---------------------------------------------------------------------------

__global__ void clearKernel(cudaSurfaceObject_t surf, int w, int h, float v) {
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || y >= h) return;
    surf2Dwrite(v, surf, x * static_cast<int>(sizeof(float)), y);
}

void clearField(Field& f, float value) {
    clearKernel<<<launchGrid(f.width, f.height), BLOCK>>>(
        f.surfObj, f.width, f.height, value);
}

// ---------------------------------------------------------------------------
// SimState lifecycle
// ---------------------------------------------------------------------------

bool createSimState(SimState& s, int width, int height) {
    s.width = width; s.height = height;

    bool ok = true;
    ok &= createField(s.velX,         width, height);
    ok &= createField(s.velX_tmp,     width, height);
    ok &= createField(s.velY,         width, height);
    ok &= createField(s.velY_tmp,     width, height);
    ok &= createField(s.density,      width, height);
    ok &= createField(s.density_tmp,  width, height);
    ok &= createField(s.pressure,     width, height);
    ok &= createField(s.pressure_tmp, width, height);
    ok &= createField(s.divergence,   width, height);
    ok &= createField(s.scratch,      width, height);
    if (!ok) return false;

    clearField(s.velX);         clearField(s.velX_tmp);
    clearField(s.velY);         clearField(s.velY_tmp);
    clearField(s.density);      clearField(s.density_tmp);
    clearField(s.pressure);     clearField(s.pressure_tmp);
    clearField(s.divergence);   clearField(s.scratch);
    return true;
}

void destroySimState(SimState& s) {
    destroyField(s.velX);         destroyField(s.velX_tmp);
    destroyField(s.velY);         destroyField(s.velY_tmp);
    destroyField(s.density);      destroyField(s.density_tmp);
    destroyField(s.pressure);     destroyField(s.pressure_tmp);
    destroyField(s.divergence);   destroyField(s.scratch);
}

// ---------------------------------------------------------------------------
// Simulation kernels
// ---------------------------------------------------------------------------

// Semi-Lagrangian advection. Velocity stored as normalized-units/sec so the
// backward trace is just `u - vx*dt` (no grid-size factor).
__global__ void advectKernel(
    cudaTextureObject_t velXTex,
    cudaTextureObject_t velYTex,
    cudaTextureObject_t srcTex,
    cudaSurfaceObject_t dstSurf,
    int width, int height, float dt)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;

    float u = (x + 0.5f) / width;
    float v = (y + 0.5f) / height;

    float vx = tex2D<float>(velXTex, u, v);
    float vy = tex2D<float>(velYTex, u, v);

    float prevU = u - vx * dt;
    float prevV = v - vy * dt;

    float result = tex2D<float>(srcTex, prevU, prevV);
    surf2Dwrite(result, dstSurf, x * static_cast<int>(sizeof(float)), y);
}

// Generic Jacobi sweep — same form for diffusion and pressure.
//   result = (orig*alpha + sum_neighbors) / beta
// Diffusion: a = dt*visc*N^2, alpha = 1/a, beta = 1/a + 4, orig = pre-diffuse
// Pressure:  alpha = 1, beta = 4, orig = -h^2 * div (already baked in by
//            divergenceKernel, see Stam GDC03)
__global__ void jacobiKernel(
    cudaTextureObject_t curTex,
    cudaTextureObject_t origTex,
    cudaSurfaceObject_t dstSurf,
    int width, int height,
    float alpha, float beta)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;

    float u  = (x + 0.5f) / width;
    float v  = (y + 0.5f) / height;
    float du = 1.0f / width;
    float dv = 1.0f / height;

    float left  = tex2D<float>(curTex, u - du, v);
    float right = tex2D<float>(curTex, u + du, v);
    float bot   = tex2D<float>(curTex, u, v - dv);
    float top   = tex2D<float>(curTex, u, v + dv);
    float orig  = tex2D<float>(origTex, u, v);

    float result = (orig * alpha + left + right + bot + top) / beta;
    surf2Dwrite(result, dstSurf, x * static_cast<int>(sizeof(float)), y);
}

// Velocity divergence with Stam's -0.5*h sign convention so the pressure
// Poisson solve reduces to plain Jacobi (alpha=1, beta=4) afterward.
__global__ void divergenceKernel(
    cudaTextureObject_t velXTex,
    cudaTextureObject_t velYTex,
    cudaSurfaceObject_t divSurf,
    int width, int height)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;

    float u  = (x + 0.5f) / width;
    float v  = (y + 0.5f) / height;
    float du = 1.0f / width;
    float dv = 1.0f / height;

    float vxR = tex2D<float>(velXTex, u + du, v);
    float vxL = tex2D<float>(velXTex, u - du, v);
    float vyT = tex2D<float>(velYTex, u, v + dv);
    float vyB = tex2D<float>(velYTex, u, v - dv);

    float div = -0.5f * ((vxR - vxL) / width + (vyT - vyB) / height);
    surf2Dwrite(div, divSurf, x * static_cast<int>(sizeof(float)), y);
}

// Make velocity divergence-free: v -= grad(p). Pressure is sampled via
// texture, velocity is read-modify-written through its own surface (each
// thread touches exactly one cell so there's no race).
__global__ void subtractGradientKernel(
    cudaTextureObject_t pTex,
    cudaSurfaceObject_t velXSurf,
    cudaSurfaceObject_t velYSurf,
    int width, int height)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;

    float u  = (x + 0.5f) / width;
    float v  = (y + 0.5f) / height;
    float du = 1.0f / width;
    float dv = 1.0f / height;

    float pR = tex2D<float>(pTex, u + du, v);
    float pL = tex2D<float>(pTex, u - du, v);
    float pT = tex2D<float>(pTex, u, v + dv);
    float pB = tex2D<float>(pTex, u, v - dv);

    float vx, vy;
    surf2Dread(&vx, velXSurf, x * static_cast<int>(sizeof(float)), y);
    surf2Dread(&vy, velYSurf, x * static_cast<int>(sizeof(float)), y);

    // h = 1/N (normalized grid spacing); central diff over 2 cells: 0.5/h.
    vx -= 0.5f * (pR - pL) * static_cast<float>(width);
    vy -= 0.5f * (pT - pB) * static_cast<float>(height);

    surf2Dwrite(vx, velXSurf, x * static_cast<int>(sizeof(float)), y);
    surf2Dwrite(vy, velYSurf, x * static_cast<int>(sizeof(float)), y);
}

// Inject mouse-driven velocity and dye with a Gaussian falloff. Force is
// applied as a per-frame additive bump (matches Phase 0 prototype); dye is
// a constant amount when the mouse moves so the trail is visible.
__global__ void addForceKernel(
    cudaSurfaceObject_t velXSurf,
    cudaSurfaceObject_t velYSurf,
    cudaSurfaceObject_t densSurf,
    int width, int height,
    float mouseX, float mouseY,
    float forceX, float forceY,
    float densityAmount,
    float radius)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;

    float u = (x + 0.5f) / width;
    float v = (y + 0.5f) / height;

    float dx = u - mouseX;
    float dy = v - mouseY;
    float distSq  = dx * dx + dy * dy;
    float falloff = expf(-distSq / (radius * radius));
    if (falloff < 1e-4f) return;

    float curVX, curVY, curD;
    surf2Dread(&curVX, velXSurf, x * static_cast<int>(sizeof(float)), y);
    surf2Dread(&curVY, velYSurf, x * static_cast<int>(sizeof(float)), y);
    surf2Dread(&curD,  densSurf, x * static_cast<int>(sizeof(float)), y);

    curVX += forceX        * falloff;
    curVY += forceY        * falloff;
    curD  += densityAmount * falloff;

    surf2Dwrite(curVX, velXSurf, x * static_cast<int>(sizeof(float)), y);
    surf2Dwrite(curVY, velYSurf, x * static_cast<int>(sizeof(float)), y);
    surf2Dwrite(curD,  densSurf, x * static_cast<int>(sizeof(float)), y);
}

__global__ void dissipateKernel(
    cudaSurfaceObject_t densSurf,
    int width, int height, float decay)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;

    float d;
    surf2Dread(&d, densSurf, x * static_cast<int>(sizeof(float)), y);
    surf2Dwrite(d * decay, densSurf, x * static_cast<int>(sizeof(float)), y);
}

__global__ void decayVelocityKernel(
    cudaSurfaceObject_t velXSurf,
    cudaSurfaceObject_t velYSurf,
    int width, int height, float decay)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;

    float vx, vy;
    surf2Dread(&vx, velXSurf, x * static_cast<int>(sizeof(float)), y);
    surf2Dread(&vy, velYSurf, x * static_cast<int>(sizeof(float)), y);
    surf2Dwrite(vx * decay, velXSurf, x * static_cast<int>(sizeof(float)), y);
    surf2Dwrite(vy * decay, velYSurf, x * static_cast<int>(sizeof(float)), y);
}

__global__ void renderDensityKernel(
    cudaTextureObject_t densityTex,
    cudaSurfaceObject_t outputSurf,
    int outW, int outH)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= outW || y >= outH) return;

    // Normalized UV works for both grids; hardware bilinear filter does the
    // sim-grid → display-resolution upsample for free.
    float u = (x + 0.5f) / outW;
    float v = (y + 0.5f) / outH;
    float d = tex2D<float>(densityTex, u, v);

    float val = fminf(fmaxf(d, 0.0f), 1.0f);
    unsigned char c = static_cast<unsigned char>(val * 255.0f);
    uchar4 px = make_uchar4(c, c, c, 255);
    surf2Dwrite(px, outputSurf, x * static_cast<int>(sizeof(uchar4)), y);
}

// ---------------------------------------------------------------------------
// Step orchestration
// ---------------------------------------------------------------------------

namespace {

// Diffuse a single scalar field in place: copy current → scratch (frozen
// pre-diffusion), then run N Jacobi sweeps ping-ponging between `field`
// and `field_tmp`. End state: result lives in `field`.
void diffuseField(Field& field, Field& field_tmp, Field& scratch,
                  float visc, float dt, int width, int height, int iters) {
    if (visc <= 0.0f) return;

    copyField(scratch, field);

    float a     = dt * visc * static_cast<float>(width) * static_cast<float>(height);
    float alpha = 1.0f / a;
    float beta  = alpha + 4.0f;

    dim3 grid = launchGrid(width, height);
    for (int i = 0; i < iters; ++i) {
        jacobiKernel<<<grid, BLOCK>>>(
            field.texObj, scratch.texObj, field_tmp.surfObj,
            width, height, alpha, beta);
        swapFields(field, field_tmp);
    }
}

// Compute divergence of (velX, velY), zero pressure, run a long Jacobi solve,
// then subtract gradient. End state: (velX, velY) is divergence-free.
void projectVelocity(SimState& s) {
    dim3 grid = launchGrid(s.width, s.height);

    divergenceKernel<<<grid, BLOCK>>>(
        s.velX.texObj, s.velY.texObj, s.divergence.surfObj,
        s.width, s.height);

    clearField(s.pressure);
    clearField(s.pressure_tmp);

    for (int i = 0; i < tuning::PRESSURE_ITERS; ++i) {
        jacobiKernel<<<grid, BLOCK>>>(
            s.pressure.texObj, s.divergence.texObj, s.pressure_tmp.surfObj,
            s.width, s.height, 1.0f, 4.0f);
        swapFields(s.pressure, s.pressure_tmp);
    }

    subtractGradientKernel<<<grid, BLOCK>>>(
        s.pressure.texObj, s.velX.surfObj, s.velY.surfObj,
        s.width, s.height);
}

}  // namespace

void stepSimulation(SimState& s, const MouseInput& mouse, float dt) {
    dim3 grid = launchGrid(s.width, s.height);

    // 1. Mouse forces (velocity impulse + dye splat) — only when the cursor
    //    moves.
    if (mouse.active) {
        float fx = mouse.dx * tuning::MOUSE_FORCE;
        float fy = mouse.dy * tuning::MOUSE_FORCE;
        addForceKernel<<<grid, BLOCK>>>(
            s.velX.surfObj, s.velY.surfObj, s.density.surfObj,
            s.width, s.height,
            mouse.x, mouse.y,
            fx, fy,
            tuning::MOUSE_DENSITY,
            tuning::INJECTION_RADIUS);
    }

    // 2. Diffuse velocity.
    diffuseField(s.velX, s.velX_tmp, s.scratch,
                 tuning::VISCOSITY, dt, s.width, s.height,
                 tuning::DIFFUSION_ITERS);
    diffuseField(s.velY, s.velY_tmp, s.scratch,
                 tuning::VISCOSITY, dt, s.width, s.height,
                 tuning::DIFFUSION_ITERS);

    // 3. Project (make velocity divergence-free).
    projectVelocity(s);

    // 4. Self-advect velocity. Both kernels read the pre-advect (velX, velY)
    //    and write to the _tmp buffers — no in-place read/write, so the
    //    order of the two launches doesn't matter. Swap after both finish.
    advectKernel<<<grid, BLOCK>>>(
        s.velX.texObj, s.velY.texObj, s.velX.texObj, s.velX_tmp.surfObj,
        s.width, s.height, dt);
    advectKernel<<<grid, BLOCK>>>(
        s.velX.texObj, s.velY.texObj, s.velY.texObj, s.velY_tmp.surfObj,
        s.width, s.height, dt);
    swapFields(s.velX, s.velX_tmp);
    swapFields(s.velY, s.velY_tmp);

    // 5. Project again
    projectVelocity(s);

    // 6. Advect density along the now-clean velocity field.
    advectKernel<<<grid, BLOCK>>>(
        s.velX.texObj, s.velY.texObj, s.density.texObj, s.density_tmp.surfObj,
        s.width, s.height, dt);
    swapFields(s.density, s.density_tmp);

    // 7. Multiplicative density decay
    dissipateKernel<<<grid, BLOCK>>>(
        s.density.surfObj, s.width, s.height, tuning::DENSITY_DECAY);

    // 8. Velocity decay.
    decayVelocityKernel<<<grid, BLOCK>>>(
        s.velX.surfObj, s.velY.surfObj,
        s.width, s.height, tuning::VELOCITY_DECAY);
}

void renderDensityToOutput(const Field& density,
                           cudaSurfaceObject_t outputSurf,
                           int outputWidth, int outputHeight) {
    renderDensityKernel<<<launchGrid(outputWidth, outputHeight), BLOCK>>>(
        density.texObj, outputSurf, outputWidth, outputHeight);
}
