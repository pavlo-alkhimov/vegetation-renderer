// Interim terrain + sky shaders: CDLOD heightfield patches through the vertex pipeline (docs/12).
// The production path renders the same patches as procedural clusters into the visibility buffer (docs/05).
#include "../src/gpu_shared.h"

[[vk::push_constant]] PushConstants pc;
[[vk::binding(0, 0)]] Texture2D g_textures[];
[[vk::binding(1, 0)]] SamplerState g_samplers[];

static const float3 SUN_COLOR = float3(1.0, 0.95, 0.88) * 3.2;

float height_at(FrameConstants f, float2 g, float lod)
{
    float2 uv = (g + 0.5) / f.terrain_size.xy;
    return g_textures[f.height_tex].SampleLevel(g_samplers[0], uv, lod).x * f.terrain_size.z + f.terrain.w;
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

float4 project(FrameConstants f, float3 rel)
{
    float3 v = rel - f.cam_pos.xyz;
    float x = dot(v, f.cam_right.xyz), y = dot(v, f.cam_up.xyz), z = dot(v, f.cam_fwd.xyz);
    return float4(x * f.cam_right.w, -y * f.cam_up.w, f.cam_pos.w, z);   // reversed Z: depth = near / z
}

float3 tonemap(float3 c)
{
    return 1.0 - exp(-c * 0.9);
}

struct TerrainVsOut {
    float4 pos : SV_Position;
    float3 rel : REL;       // relative to render origin
    float2 grid : GRID;     // grid samples, absolute within the terrain
    nointerpolation uint level : LEVEL;
};

[shader("vertex")]
TerrainVsOut vs_terrain(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
    FrameConstants f = *pc.frame;
    TerrainNode n = pc.nodes[iid];
    uint vx = vid % (TERRAIN_PATCH_QUADS + 1), vz = vid / (TERRAIN_PATCH_QUADS + 1);
    float step = float(1u << n.level);
    float2 maxg = f.terrain_size.xy - 1.0;
    float2 g = float2(n.gx, n.gz) + float2(vx, vz) * step;

    // CDLOD morph: odd vertices slide onto the next coarser grid as the distance approaches the level's range.
    float2 gc = min(g, maxg);
    float3 p = float3(f.terrain.x + gc.x * f.terrain.z, height_at(f, gc, 0), f.terrain.y + gc.y * f.terrain.z);
    float4 m = f.morph[n.level];
    float k = saturate((distance(p, f.cam_pos.xyz) - m.x) * m.y);
    g -= frac(float2(vx, vz) * 0.5) * 2.0 * k * step;
    g = min(g, maxg);   // patches overlapping the east/north edge collapse onto it

    TerrainVsOut o;
    o.rel = float3(f.terrain.x + g.x * f.terrain.z, height_at(f, g, 0), f.terrain.y + g.y * f.terrain.z);
    o.pos = project(f, o.rel);
    o.grid = g;
    o.level = n.level;
    return o;
}

float hash(float2 p)
{
    p = frac(p * float2(0.1031, 0.1030));
    p += dot(p, p.yx + 33.33);
    return frac((p.x + p.y) * p.x);
}

float value_noise(float2 p)
{
    float2 i = floor(p), u = frac(p);
    u = u * u * (3.0 - 2.0 * u);
    return lerp(lerp(hash(i), hash(i + float2(1, 0)), u.x), lerp(hash(i + float2(0, 1)), hash(i + float2(1, 1)), u.x), u.y);
}

[shader("fragment")]
float4 fs_terrain(TerrainVsOut i) : SV_Target
{
    FrameConstants f = *pc.frame;
    // Normal from the height texture with a footprint-sized difference, so distant terrain does not alias.
    float2 fp = max(abs(ddx(i.grid)), abs(ddy(i.grid)));
    float lod = max(0.0, log2(max(fp.x, fp.y)));
    float e = exp2(floor(lod));
    float hx0 = height_at(f, i.grid - float2(e, 0), lod), hx1 = height_at(f, i.grid + float2(e, 0), lod);
    float hz0 = height_at(f, i.grid - float2(0, e), lod), hz1 = height_at(f, i.grid + float2(0, e), lod);
    float3 n = normalize(float3(hx0 - hx1, 2.0 * e * f.terrain.z, hz0 - hz1));

    // Placeholder materials until the terrain layers and RVT exist (docs/12): meadow / forest patches, soil and rock
    // on slopes. Noise uses absolute grid coordinates so it does not move with the render origin.
    float patches = value_noise(i.grid / 180.0) * 0.6 + value_noise(i.grid / 45.0) * 0.3 + value_noise(i.grid / 9.0) * 0.1;
    float3 albedo = lerp(float3(0.13, 0.20, 0.06), float3(0.05, 0.09, 0.035), smoothstep(0.45, 0.6, patches));
    albedo = lerp(albedo, float3(0.24, 0.19, 0.13), smoothstep(0.85, 0.7, n.y));
    albedo = lerp(albedo, float3(0.32, 0.31, 0.29), smoothstep(0.7, 0.55, n.y));

    if (f.debug_mode == DEBUG_LOD) {
        static const float3 tint[6] = { float3(1, 0.3, 0.3), float3(1, 0.7, 0.2), float3(0.9, 1, 0.3), float3(0.3, 1, 0.4), float3(0.3, 0.8, 1), float3(0.7, 0.4, 1) };
        albedo = tint[i.level % 6] * 0.4;
    }

    float3 to_cam = f.cam_pos.xyz - i.rel;
    float dist = length(to_cam);
    float3 c = albedo * (SUN_COLOR * saturate(dot(n, f.sun_dir.xyz)) + float3(0.30, 0.38, 0.52) * (0.6 + 0.4 * n.y));

    if (f.debug_mode == DEBUG_CONTOUR) {
        float h = i.rel.y - f.terrain.w;    // above height_min; lines every 10 m, thicker every 50 m
        float l10 = abs(frac(h / 10.0 + 0.5) - 0.5) / max(fwidth(h / 10.0), 1e-5);
        float l50 = abs(frac(h / 50.0 + 0.5) - 0.5) / max(fwidth(h / 50.0), 1e-5);
        c = lerp(c, float3(0.02, 0.02, 0.02), max(1.0 - saturate(l10), (1.0 - saturate(l50 * 0.5))) * 0.8);
    }
    if (f.debug_mode == DEBUG_NORMALS) c = n * 0.5 + 0.5;

    float fog = 1.0 - exp(-dist * f.sun_dir.w);
    c = lerp(c, sky(f, -to_cam / dist), fog);
    return float4(f.debug_mode == DEBUG_NORMALS ? c : tonemap(c), 1);
}

struct SkyVsOut {
    float4 pos : SV_Position;
    float2 ndc : NDC;
};

[shader("vertex")]
SkyVsOut vs_sky(uint vid : SV_VertexID)
{
    SkyVsOut o;
    o.ndc = float2((vid << 1) & 2, vid & 2) * 2.0 - 1.0;
    o.pos = float4(o.ndc, 0.0, 1.0);   // depth 0 = infinitely far (reversed Z)
    return o;
}

[shader("fragment")]
float4 fs_sky(SkyVsOut i) : SV_Target
{
    FrameConstants f = *pc.frame;
    float3 dir = normalize(f.cam_fwd.xyz + f.cam_right.xyz * (i.ndc.x / f.cam_right.w) - f.cam_up.xyz * (i.ndc.y / f.cam_up.w));
    return float4(tonemap(sky(f, dir)), 1);
}
