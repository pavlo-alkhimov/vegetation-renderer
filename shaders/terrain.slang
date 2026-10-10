// Interim terrain + sky shaders: CDLOD heightfield patches through the vertex pipeline (docs/12).
// The production path renders the same patches as procedural clusters into the visibility buffer (docs/05).
#include "common.slang"

struct TerrainVsOut {
    float4 pos : SV_Position;
    float3 rel : REL;       // relative to render origin
    float2 grid : GRID;     // grid samples, absolute within the terrain
    float4 prev : PREV;     // previous frame's clip position (motion vectors)
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
    o.pos = project(o.rel);
    o.prev = project_prev(o.rel);
    o.grid = g;
    o.level = n.level;
    return o;
}

[shader("fragment")]
SceneOut fs_terrain(TerrainVsOut i)
{
    FrameConstants f = *pc.frame;
    // Normal from the height texture with a footprint-sized difference, so distant terrain does not alias.
    float2 fp = max(abs(ddx(i.grid)), abs(ddy(i.grid)));
    float lod = max(0.0, log2(max(fp.x, fp.y)));
    float e = exp2(floor(lod));
    float hx0 = height_at(f, i.grid - float2(e, 0), lod), hx1 = height_at(f, i.grid + float2(e, 0), lod);
    float hz0 = height_at(f, i.grid - float2(0, e), lod), hz1 = height_at(f, i.grid + float2(0, e), lod);
    float3 n = normalize(float3(hx0 - hx1, 2.0 * e * f.terrain.z, hz0 - hz1));

    // Placeholder materials until the terrain layers and RVT exist (docs/12), driven by the vegetation mask: meadow,
    // forest floor where trees are drawn, canopy colour beyond the tree draw distance (or with trees off), soil and
    // rock on slopes. Noise uses absolute grid coordinates so it does not move with the render origin.
    float3 to_cam = f.cam_pos.xyz - i.rel;
    float dist = length(to_cam);
    float2 mask = mask_at(f, i.grid, max(0.0, lod - 2.0));
    float variation = value_noise(i.grid / 45.0) * 0.6 + value_noise(i.grid / 9.0) * 0.4;

    // Meadow as a turbid grass layer (docs/06 §5.5): from steep views soil and thatch show between the blades, at
    // grazing angles only blade sides; taller and denser grass covers more. Same height, dryness and colour
    // functions as the blades and plant assets, so the far field matches the near field. Inside the blade radius
    // the blades are drawn and the terrain shows the ground between them.
    float2 pm = i.grid * f.terrain.z;                                   // terrain-local metres
    float3 grass_col = cover_tint(f, pm) * lerp(0.85, 1.12, variation);
    float3 thatch = grass_col * float3(0.6, 0.55, 0.42);
    float cos_v = saturate(dot(n, to_cam / dist));
    float cover = 1.0 - exp(-mask.y * (0.4 + 5.0 * meadow_height(pm)) / max(cos_v, 0.05));
    float3 layer = lerp(thatch, grass_col * (1.0 + 0.25 * pow(1.0 - cos_v, 3.0)), cover);
    float blades = (f.veg_flags & VEG_COVER) != 0 ? 1.0 - smoothstep(GC_NEAR_DISTANCE * 0.6, GC_NEAR_DISTANCE, dist) : 0.0;
    float3 meadow = lerp(layer, thatch * 0.85, blades);
    float canopy = (f.veg_flags & VEG_TREES) != 0 ? smoothstep(f.veg.x * 0.7, f.veg.x, dist) : 1.0;
    float3 forest = lerp(FOREST_FLOOR_ALBEDO, CANOPY_ALBEDO, canopy);
    float3 albedo = lerp(meadow, forest, smoothstep(0.25, 0.6, mask.x));
    albedo = lerp(albedo, float3(0.24, 0.19, 0.13), smoothstep(0.85, 0.7, n.y));
    albedo = lerp(albedo, float3(0.32, 0.31, 0.29), smoothstep(0.7, 0.55, n.y));

    if (f.debug_mode == DEBUG_LOD) albedo = DEBUG_TINT[i.level % 6] * 0.4;

    float3 c = albedo * (SUN_COLOR * saturate(dot(n, f.sun_dir.xyz)) * sun_shadow(f, i.rel, n, i.pos.xy) + SKY_AMBIENT * (0.6 + 0.4 * n.y));

    if (f.debug_mode == DEBUG_CONTOUR) {
        float h = i.rel.y - f.terrain.w;    // above height_min; lines every 10 m, thicker every 50 m
        float l10 = abs(frac(h / 10.0 + 0.5) - 0.5) / max(fwidth(h / 10.0), 1e-5);
        float l50 = abs(frac(h / 50.0 + 0.5) - 0.5) / max(fwidth(h / 50.0), 1e-5);
        c = lerp(c, float3(0.02, 0.02, 0.02), max(1.0 - saturate(l10), (1.0 - saturate(l50 * 0.5))) * 0.8);
    }
    if (f.debug_mode == DEBUG_NORMALS) c = n * 0.5 + 0.5;

    float fog = 1.0 - exp(-dist * f.sun_dir.w);
    c = lerp(c, sky(f, -to_cam / dist), fog);
    return scene_out(f, f.debug_mode == DEBUG_NORMALS ? c : tonemap(c), i.pos, i.prev);
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
SceneOut fs_sky(SkyVsOut i)
{
    FrameConstants f = *pc.frame;
    float2 ndc = i.ndc - pc.view->jitter.xy;
    float3 dir = normalize(f.cam_fwd.xyz + f.cam_right.xyz * (ndc.x / f.cam_right.w) - f.cam_up.xyz * (ndc.y / f.cam_up.w));
    return scene_out(f, tonemap(sky(f, dir)), i.pos, project_prev_dir(dir));
}
