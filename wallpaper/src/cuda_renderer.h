#pragma once

struct ID3D11Texture2D;
struct cudaGraphicsResource;

// Owns a CUDA-registered handle to a DX11 texture and a placeholder kernel
// that writes an animated pattern into it each frame.
//
// The DX11 texture itself lives in Renderer; this class only borrows a raw
// pointer to register it with CUDA and unregister at shutdown.
class CudaRenderer {
public:
    CudaRenderer() = default;
    ~CudaRenderer();

    CudaRenderer(const CudaRenderer&) = delete;
    CudaRenderer& operator=(const CudaRenderer&) = delete;

    bool init(ID3D11Texture2D* shared_texture, int width, int height);
    void render(float time_seconds);

private:
    cudaGraphicsResource* resource_ = nullptr;
    int width_  = 0;
    int height_ = 0;
};
