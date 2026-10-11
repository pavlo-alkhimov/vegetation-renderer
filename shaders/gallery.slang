// Plant gallery (src/gallery.cpp): the dimmed last world frame as background, and per cell a gray stage under the
// plant with a scale grid (lines every 10 cm, stronger every 50 cm).
#include "common.slang"

[shader("vertex")]
float4 vs_fullscreen(uint vid : SV_VertexID) : SV_Position
{
    float2 ndc = float2((vid << 1) & 2, vid & 2) * 2.0 - 1.0;
    return float4(ndc, 0.0, 1.0);
}

[shader("fragment")]
SceneOut fs_frozen(float4 pos : SV_Position)
{
    SceneOut o;
    o.color = float4(g_textures[TEX_FROZEN].Load(int3(int2(pos.xy), 0)).rgb * 0.3, 1.0);
    o.motion = float2(0, 0);
    return o;
}

struct StageVsOut {
    float4 pos : SV_Position;
    float3 rel : REL;
    float4 prev : PREV;
};

static const float2 STAGE_QUAD[6] = { float2(-1, -1), float2(1, -1), float2(-1, 1), float2(-1, 1), float2(1, -1), float2(1, 1) };

[shader("vertex")]
StageVsOut vs_stage(uint vid : SV_VertexID)
{
    FrameConstants f = *pc.frame;
    StageVsOut o;
    float2 q = STAGE_QUAD[vid] * f.veg.w;                                 // veg.w = stage half size (m)
    o.rel = float3(q.x, -0.002, q.y);
    o.pos = project(o.rel);
    o.prev = project_prev(o.rel);
    return o;
}

[shader("fragment")]
SceneOut fs_stage(StageVsOut i)
{
    FrameConstants f = *pc.frame;
    float2 p = i.rel.xz;
    float2 w10 = abs(frac(p / 0.1 + 0.5) - 0.5) / max(fwidth(p / 0.1), 1e-5);
    float2 w50 = abs(frac(p / 0.5 + 0.5) - 0.5) / max(fwidth(p / 0.5), 1e-5);
    float line = max(1.0 - saturate(min(w10.x, w10.y)), (1.0 - saturate(min(w50.x, w50.y) * 0.5)) * 1.5);
    float3 albedo = float3(0.22, 0.22, 0.22) * (1.0 - 0.35 * saturate(line));
    float3 n = float3(0, 1, 0);
    if (f.debug_mode == DEBUG_NORMALS) return scene_out(f, n * 0.5 + 0.5, i.pos, i.prev);
    if (f.debug_mode == DEBUG_UNLIT || f.debug_mode == DEBUG_ALPHA) return scene_out(f, albedo, i.pos, i.prev);
    if (f.debug_mode == DEBUG_UV) return scene_out(f, float3(frac(p), 0), i.pos, i.prev);
    float3 c = albedo * (SUN_COLOR * saturate(dot(n, f.sun_dir.xyz)) + SKY_AMBIENT);
    return scene_out(f, tonemap(c), i.pos, i.prev);
}
