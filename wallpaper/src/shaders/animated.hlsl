// Placeholder animated shader for Phase 1.
//
// Phase 3 swaps this for the fluid-sim density texture; for now it just
// proves the constant buffer is updating each frame and the fullscreen
// quad is rasterizing correctly.

cbuffer Globals : register(b0) {
    float  time;
    float2 resolution;
    float  _pad;
};

struct VsOut {
    float4 position : SV_Position;
};

// Fullscreen triangle (covers clip-space [-1,1]^2 with one oversized triangle).
//   id=0 -> (-1,-1)
//   id=1 -> (-1, 3)
//   id=2 -> ( 3,-1)
// Order is clockwise in Y-up NDC so the default D3D11 rasterizer state
// (CullMode = BACK, FrontCounterClockwise = FALSE) treats it as front-facing.
VsOut vs_main(uint id : SV_VertexID) {
    VsOut o;
    float2 xy = float2((id == 2) ? 3.0 : -1.0,
                       (id == 1) ? 3.0 : -1.0);
    o.position = float4(xy, 0.0, 1.0);
    return o;
}

float4 ps_main(VsOut input) : SV_Target {
    float2 uv = input.position.xy / resolution;
    float v = sin(uv.x * 10.0 + time) + cos(uv.y * 10.0 + time * 1.3);
    v = 0.5 + 0.5 * v / 2.0;
    return float4(v * 0.2, v * 0.5, v, 1.0);
}
