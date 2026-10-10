// Shared by all shader modules: bindings, terrain height function, camera projection, sky, fog, lighting, noise.
#pragma once
#include "../src/gpu_shared.h"

[[vk::push_constant]] PushConstants pc;
[[vk::binding(0, 0)]] Texture2D g_textures[];
[[vk::binding(1, 0)]] SamplerState g_samplers[];
[[vk::binding(2, 0)]] SamplerComparisonState g_shadow_sampler;
[[vk::binding(3, 0)]] Texture2DArray g_shadow_map;

static const float3 SUN_COLOR = float3(1.0, 0.95, 0.88) * 3.2;
static const float3 SKY_AMBIENT = float3(0.30, 0.38, 0.52);

// Ground and canopy albedos shared by terrain and vegetation, so the far field (terrain only) matches the near
// field (blades, trees) on average.
static const float3 MEADOW_ALBEDO = float3(0.13, 0.20, 0.06);
static const float3 FOREST_FLOOR_ALBEDO = float3(0.075, 0.08, 0.04);
static const float3 CANOPY_ALBEDO = float3(0.045, 0.085, 0.035);

// Terrain height (relative to the render origin) at grid sample coordinates g. The same bilinear function as
// terrain_height() on the CPU (docs/12: one height function).
float height_at(FrameConstants f, float2 g, float lod)
{
    float2 uv = (g + 0.5) / f.terrain_size.xy;
    return g_textures[f.height_tex].SampleLevel(g_samplers[0], uv, lod).x * f.terrain_size.z + f.terrain.w;
}

// Vegetation mask (r = forest, g = ground cover) at grid sample coordinates g; texels cover the terrain extent evenly.
float2 mask_at(FrameConstants f, float2 g, float lod)
{
    float2 uv = g / (f.terrain_size.xy - 1.0);
    return g_textures[f.mask_tex].SampleLevel(g_samplers[0], uv, lod).xy;
}

float3 sky(FrameConstants f, float3 dir)
{
    float up = saturate(dir.y);
    float3 c = lerp(float3(0.62, 0.72, 0.85), float3(0.22, 0.38, 0.72), pow(up, 0.6));
    c = lerp(c, float3(0.55, 0.55, 0.52), saturate(-dir.y * 4.0));   // below the horizon: haze
    float s = saturate(dot(dir, f.sun_dir.xyz));
    c += SUN_COLOR * (pow(s, 1200.0) * 8.0 + pow(s, 12.0) * 0.08);
    return c;
}

float4 xform(v4 m[4], float3 p)
{
    float4 q = float4(p, 1.0);
    return float4(dot(m[0], q), dot(m[1], q), dot(m[2], q), dot(m[3], q));
}

// Clip position in the current view (reversed Z: depth = near / view depth).
float4 project(float3 rel)
{
    return xform(pc.view->view_proj, rel);
}

// Clip position of a point (or of a direction, w = 0) in the previous frame, for motion vectors.
float4 project_prev(float3 rel)
{
    return xform(pc.view->prev_view_proj, rel);
}

float4 project_prev_dir(float3 dir)
{
    float4 q = float4(dir, 0.0);
    v4 m[4] = pc.view->prev_view_proj;
    return float4(dot(m[0], q), dot(m[1], q), dot(m[2], q), dot(m[3], q));
}

// Scene pass output: colour + motion (uv of this frame - uv of the previous frame, without jitter).
struct SceneOut {
    float4 color : SV_Target0;
    float2 motion : SV_Target1;
};

SceneOut scene_out(FrameConstants f, float3 c, float4 pos, float4 prev)
{
    SceneOut o;
    o.color = float4(c, 1.0);
    float2 ndc = pos.xy * f.screen.zw * 2.0 - 1.0 - pc.view->jitter.xy;
    o.motion = prev.w > 1e-6 ? (ndc - prev.xy / prev.w) * 0.5 : float2(0, 0);
    return o;
}

// Sphere against the view's planes.
bool sphere_visible(float3 c, float r)
{
    for (uint i = 0; i < 6; i++)
        if (dot(pc.view->planes[i].xyz, c) + pc.view->planes[i].w < -r) return false;
    return true;
}

float3 tonemap(float3 c)
{
    return 1.0 - exp(-c * 0.9);
}

float3 apply_fog(FrameConstants f, float3 c, float3 rel)
{
    float3 to_cam = f.cam_pos.xyz - rel;
    float dist = length(to_cam);
    float fog = 1.0 - exp(-dist * f.sun_dir.w);
    return lerp(c, sky(f, -to_cam / max(dist, 1e-3)), fog);
}

// Foliage and ground cover: wrapped diffuse, sun transmission when looking towards the sun, sky ambient. ao darkens the
// ambient fully and the sun partly (self-shadowing below shadow-map resolution); shadow = sun visibility.
float3 shade_foliage(FrameConstants f, float3 albedo, float3 n, float3 to_cam, float ao, float transmission, float shadow)
{
    float3 l = f.sun_dir.xyz;
    float ndl = dot(n, l);
    float diffuse = saturate(ndl * 0.6 + 0.4);
    float through = saturate(-ndl) * 0.35 * transmission;              // thin leaves: light through the back side
    float back = pow(saturate(dot(-to_cam, l)), 6.0) * transmission;   // looking towards the sun
    float sun_vis = lerp(0.45, 1.0, ao) * shadow;
    return albedo * (SUN_COLOR * (diffuse + through + back) * sun_vis + SKY_AMBIENT * (0.75 + 0.25 * n.y) * ao);
}

// Sun visibility (0..1) from the cascaded shadow map at a point relative to the render origin with normal n. Cascade
// by view depth with a dithered hand-over near each cascade's end (TAA smooths it), normal offset against acne,
// 4 bilinear comparison taps.
float sun_shadow(FrameConstants f, float3 rel, float3 n, float2 pixel)
{
    if (f.shadows == 0) return 1.0;
    float depth = dot(rel - f.cam_pos.xyz, f.cam_fwd.xyz);
    uint c = 0;
    while (c < SHADOW_CASCADES && depth > f.shadow_split[c]) c++;
    if (c < SHADOW_CASCADES) {
        float end = f.shadow_split[c], start = lerp(c > 0 ? f.shadow_split[c - 1] : 0.0, end, 0.85);
        if (hash(pixel + frac(f.cam_fwd.w * 13.37) * 97.0) < saturate((depth - start) / (end - start))) c++;
    }
    if (c >= SHADOW_CASCADES) return 1.0;
    float texel = f.shadow_texel[c];
    ViewConstants v = pc.view[1 + c];
    float4 q = xform(v.view_proj, rel + n * texel * 1.5);
    float2 uv = q.xy * 0.5 + 0.5;
    if (any(uv < 0.0) || any(uv > 1.0)) return 1.0;
    float ref = q.z + 2.0 * texel * length(v.view_proj[2].xyz);
    float d = 0.5 / SHADOW_SIZE, s = 0.0;
    s += g_shadow_map.SampleCmpLevelZero(g_shadow_sampler, float3(uv + float2(-d, -d), c), ref);
    s += g_shadow_map.SampleCmpLevelZero(g_shadow_sampler, float3(uv + float2(d, -d), c), ref);
    s += g_shadow_map.SampleCmpLevelZero(g_shadow_sampler, float3(uv + float2(-d, d), c), ref);
    s += g_shadow_map.SampleCmpLevelZero(g_shadow_sampler, float3(uv + float2(d, d), c), ref);
    return s * 0.25;
}
float hash(float2 p)
{
    p = frac(p * float2(0.1031, 0.1030));
    p += dot(p, p.yx + 33.33);
    return frac((p.x + p.y) * p.x);
}

uint hash_u32(uint x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float hash_f(uint x)
{
    return float(hash_u32(x) >> 8) * (1.0 / 16777216.0);
}

float value_noise(float2 p)
{
    float2 i = floor(p), u = frac(p);
    u = u * u * (3.0 - 2.0 * u);
    return lerp(lerp(hash(i), hash(i + float2(1, 0)), u.x), lerp(hash(i + float2(0, 1)), hash(i + float2(1, 1)), u.x), u.y);
}

float3 unpack_normal(uint p)   // snorm 10:10:10
{
    int3 v = int3(int(p << 22) >> 22, int(p << 12) >> 22, int(p << 2) >> 22);
    return float3(v) / 511.0;
}

// Task -> mesh shader expansion: find the payload slot owning mesh workgroup `gid`. prefix[] is non-decreasing with
// prefix[32] = total; the result is the largest i with prefix[i] <= gid, which is always a non-empty slot.
uint find_slot(uint prefix[33], uint gid)
{
    uint lo = 0;
    for (uint step = 16; step > 0; step >>= 1)
        if (prefix[lo + step] <= gid) lo += step;
    return lo;
}

// Habitat weights from the vegetation mask (r = forest, g = ground cover): meadow, forest edge, forest floor.
float3 gc_habitat(float2 mask)
{
    float forest = mask.x;
    return float3(mask.y * (1.0 - forest), 4.0 * forest * (1.0 - forest), forest * forest);
}

// Meadow grass height (m) at terrain-local metres: patches of short (grazed, mown) and tall grass. One function for
// ground-cover scale and terrain shading, so both agree.
float meadow_height(float2 pm)
{
    float n = value_noise(pm / 23.0) * 0.65 + value_noise(pm / 6.0 + 31.7) * 0.35;
    return lerp(0.15, 0.75, smoothstep(0.2, 0.7, n));
}

// 0 fresh green .. 1 dry, yellowed patches.
float meadow_dryness(float2 pm)
{
    return smoothstep(0.5, 0.85, value_noise(pm / 41.0 + 17.3));
}

float3 cover_tint(FrameConstants f, float2 pm)
{
    return f.cover.rgb * lerp(float3(1, 1, 1), float3(1.35, 1.15, 0.7), meadow_dryness(pm));
}

// Gust: a travelling wave along the wind direction (pm = terrain-local metres).
float wind_gust(FrameConstants f, float time, float2 pm, float phase)
{
    return 0.35 + 0.3 * sin(time * 1.7 + dot(pm, f.wind.xz) * 0.35 + phase * 1.5);
}

static const float3 DEBUG_TINT[6] = { float3(1, 0.3, 0.3), float3(1, 0.7, 0.2), float3(0.9, 1, 0.3), float3(0.3, 1, 0.4),
                                      float3(0.3, 0.8, 1), float3(0.7, 0.4, 1) };
