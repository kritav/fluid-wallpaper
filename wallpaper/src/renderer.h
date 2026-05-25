#pragma once
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

class Renderer {
public:
    bool init(HWND hwnd, int width, int height);
    void render(float time_seconds);

private:
    int width_  = 0;
    int height_ = 0;
    Microsoft::WRL::ComPtr<ID3D11Device>           device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext>    context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain>         swap_chain_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader>     vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>      ps_;
    Microsoft::WRL::ComPtr<ID3D11Buffer>           cbuffer_;
};
