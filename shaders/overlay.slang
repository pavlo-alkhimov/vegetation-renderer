// Overlay quad (src/overlay.cpp): panel background, FPS graph, 5 x 7 bitmap text. Output is premultiplied alpha.
#include "common.slang"

static const float2 QUAD[6] = { float2(0, 0), float2(1, 0), float2(0, 1), float2(0, 1), float2(1, 0), float2(1, 1) };

[shader("vertex")]
float4 vs_overlay(uint vid : SV_VertexID) : SV_Position
{
    float4 r = pc.overlay->panel;
    float2 p = r.xy + QUAD[vid] * r.zw;
    return float4(p / pc.overlay->screen.xy * 2.0 - 1.0, 0.5, 1.0);
}

float3 fps_color(float fps)
{
    if (fps >= 57.0) return float3(0.10, 0.55, 0.08);
    if (fps >= 28.5) return float3(0.75, 0.50, 0.04);
    return float3(0.75, 0.06, 0.04);
}

[shader("fragment")]
float4 fs_overlay(float4 pos : SV_Position) : SV_Target
{
    float2 p = pos.xy;
    float4 c = float4(0, 0, 0, 0.7);

    float4 g = pc.overlay->graph;
    float4 params = pc.overlay->params;
    float2 q = p - g.xy;
    if (all(q >= 0.0) && all(q < g.zw)) {
        c = float4(0, 0, 0, 0.85);
        uint col = min(uint(q.x / g.z * OVERLAY_GRAPH_COLS), OVERLAY_GRAPH_COLS - 1u);
        float fps = pc.overlay->fps[col];
        float px_fps = params.y / g.w;                    // FPS per pixel row
        float y = (g.w - q.y) * px_fps;                   // FPS at this pixel's height
        if (fps > 0.0 && y <= fps) c = float4(fps_color(fps), 1.0);
        if (abs(y - params.z) < px_fps * 0.75 || abs(y - params.w) < px_fps * 0.75)
            c = float4(lerp(c.rgb, float3(0.6, 0.6, 0.6), 0.6), 1.0);
    }

    float4 t = pc.overlay->text;
    float2 cell = floor((p - t.xy) / t.zw);
    if (all(cell >= 0.0) && cell.x < OVERLAY_TEXT_COLS && cell.y < OVERLAY_TEXT_LINES) {
        uint i = uint(cell.y) * OVERLAY_TEXT_COLS + uint(cell.x);
        uint ch = (pc.overlay->chars[i >> 2] >> ((i & 3u) * 8u)) & 0xffu;
        bool highlight = (ch & 0x80u) != 0;
        ch &= 0x7fu;
        if (ch >= 97u && ch <= 122u) ch -= 32u;           // lowercase -> uppercase
        int2 texel = int2(floor((p - t.xy - cell * t.zw) / params.x));
        if (ch > 32u && all(texel >= 0) && texel.x < 5 && texel.y < 7) {
            uint bit = uint(texel.y * 5 + texel.x);
            if (((pc.overlay->font[(ch - 32u) * 2u + (bit >> 5)] >> (bit & 31u)) & 1u) != 0)
                c = float4(highlight ? float3(1.0, 0.65, 0.15) : float3(0.85, 0.85, 0.85), 1.0);
        }
    }
    return float4(c.rgb * c.a, c.a);
}
