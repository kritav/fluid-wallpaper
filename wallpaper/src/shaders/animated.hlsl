// Phase 2: shader is now a thin pass-through that samples the texture
// CUDA writes into each frame. The animated pattern lives in the kernel
// (cuda_renderer.cu); Phase 3 will replace that kernel with the fluid sim's
// density field.

Texture2D    input_texture  : register(t0);
SamplerState linear_sampler : register(s0);

struct VsOut {
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

// Fullscreen triangle (clockwise in Y-up NDC so the default rasterizer
// state treats it as front-facing).
//   id=0 -> pos (-1,-1)  uv (0, 1)   bottom-left
//   id=1 -> pos (-1, 3)  uv (0,-1)   off-screen
//   id=2 -> pos ( 3,-1)  uv (2, 1)   off-screen
// D3D texture origin is top-left, NDC y is up, so uv.y = 0.5 - 0.5*pos.y.
VsOut vs_main(uint id : SV_VertexID) {
    VsOut o;
    float2 xy = float2((id == 2) ? 3.0 : -1.0,
                       (id == 1) ? 3.0 : -1.0);
    o.position = float4(xy, 0.0, 1.0);
    o.uv       = float2(xy.x * 0.5 + 0.5, 0.5 - xy.y * 0.5);
    return o;
}

float4 ps_main(VsOut input) : SV_Target {
    return input_texture.Sample(linear_sampler, input.uv);
}
