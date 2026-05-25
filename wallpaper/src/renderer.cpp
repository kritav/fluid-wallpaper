#include "renderer.h"
#include <d3dcompiler.h>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {

// Must match the cbuffer Globals layout in animated.hlsl.
// HLSL packs `float time; float2 resolution; float _pad;` into one 16-byte
// register; the C++ struct mirrors that exactly with no padding.
struct Globals {
    float time;
    float resolution[2];
    float _pad;
};

std::wstring shader_path() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring path(buf);
    auto slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) path.resize(slash);
    return path + L"\\shaders\\animated.hlsl";
}

std::string read_file(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

ComPtr<ID3DBlob> compile_shader(const std::string& src,
                                const char* entry,
                                const char* target) {
    ComPtr<ID3DBlob> code, err;
    UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    HRESULT hr = D3DCompile(src.data(), src.size(), "animated.hlsl",
                            nullptr, nullptr, entry, target,
                            flags, 0, &code, &err);
    if (FAILED(hr)) {
        if (err) {
            fprintf(stderr, "Shader compile error (%s/%s): %s\n",
                    entry, target,
                    static_cast<const char*>(err->GetBufferPointer()));
        } else {
            fprintf(stderr, "Shader compile failed (%s/%s): 0x%08lx\n",
                    entry, target, hr);
        }
        return nullptr;
    }
    return code;
}

}  // namespace

bool Renderer::init(HWND hwnd, int width, int height) {
    width_  = width;
    height_ = height;

    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferCount        = 2;
    scd.BufferDesc.Width   = width;
    scd.BufferDesc.Height  = height;
    scd.BufferDesc.Format  = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow       = hwnd;
    scd.SampleDesc.Count   = 1;
    scd.Windowed           = TRUE;
    scd.SwapEffect         = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    UINT flags = 0;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL level;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        nullptr, 0, D3D11_SDK_VERSION,
        &scd, &swap_chain_, &device_, &level, &context_);
    if (FAILED(hr)) {
        fprintf(stderr, "D3D11CreateDeviceAndSwapChain failed: 0x%08lx\n", hr);
        return false;
    }

    ComPtr<ID3D11Texture2D> back_buffer;
    swap_chain_->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    device_->CreateRenderTargetView(back_buffer.Get(), nullptr, &rtv_);

    D3D11_VIEWPORT vp{};
    vp.Width    = static_cast<FLOAT>(width);
    vp.Height   = static_cast<FLOAT>(height);
    vp.MaxDepth = 1.0f;
    context_->RSSetViewports(1, &vp);

    auto path = shader_path();
    std::string src = read_file(path);
    if (src.empty()) {
        fwprintf(stderr, L"Could not read shader at %ls\n", path.c_str());
        return false;
    }

    auto vs_blob = compile_shader(src, "vs_main", "vs_5_0");
    auto ps_blob = compile_shader(src, "ps_main", "ps_5_0");
    if (!vs_blob || !ps_blob) return false;

    device_->CreateVertexShader(vs_blob->GetBufferPointer(),
                                vs_blob->GetBufferSize(), nullptr, &vs_);
    device_->CreatePixelShader(ps_blob->GetBufferPointer(),
                               ps_blob->GetBufferSize(), nullptr, &ps_);

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth      = sizeof(Globals);
    bd.Usage          = D3D11_USAGE_DYNAMIC;
    bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    device_->CreateBuffer(&bd, nullptr, &cbuffer_);

    return true;
}

void Renderer::render(float time_seconds) {
    Globals g{};
    g.time          = time_seconds;
    g.resolution[0] = static_cast<float>(width_);
    g.resolution[1] = static_cast<float>(height_);

    D3D11_MAPPED_SUBRESOURCE map;
    if (SUCCEEDED(context_->Map(cbuffer_.Get(), 0,
                                D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        memcpy(map.pData, &g, sizeof(g));
        context_->Unmap(cbuffer_.Get(), 0);
    }

    const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    context_->ClearRenderTargetView(rtv_.Get(), clear);

    ID3D11RenderTargetView* rtvs[] = {rtv_.Get()};
    context_->OMSetRenderTargets(1, rtvs, nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->IASetInputLayout(nullptr);  // VS generates verts from SV_VertexID

    context_->VSSetShader(vs_.Get(), nullptr, 0);
    context_->PSSetShader(ps_.Get(), nullptr, 0);
    ID3D11Buffer* cbs[] = {cbuffer_.Get()};
    context_->VSSetConstantBuffers(0, 1, cbs);
    context_->PSSetConstantBuffers(0, 1, cbs);

    context_->Draw(3, 0);  // fullscreen triangle, 3 vertices

    swap_chain_->Present(1, 0);  // vsync
}
