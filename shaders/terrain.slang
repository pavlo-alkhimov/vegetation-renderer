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

// Baked ground-cover top view of habitat k, stochastically tiled (Heitz & Neyret 2018, Deliot & Heitz 2019): a
// triangle grid of ~2 m cells; each vertex picks a random offset and 90-degree rotation of the tile; the three copies
// are blended by sharpened barycentric weights. Normals are rotated back. Accumulates weight * sample.
float2 rot90(float2 p, uint r)
{
    return r == 0 ? p : r == 1 ? float2(-p.y, p.x) : r == 2 ? -p : float2(p.y, -p.x);
}

void cover_tiled(uint k, float2 uv, float2 duvx, float2 duvy, float weight, inout float4 color, inout float4 surf)
{
    float2 skewed = mul(float2x2(1.0, 0.0, -0.57735027, 1.15470054), uv * (GC_BAKE_TILE / 2.0) * 3.464);
    int2 base = int2(floor(skewed));
    float3 t = float3(frac(skewed), 0);
    t.z = 1.0 - t.x - t.y;
    float3 w;
    int2 vtx[3];
    if (t.z > 0.0) { w = float3(t.z, t.y, t.x); vtx[0] = base; vtx[1] = base + int2(0, 1); vtx[2] = base + int2(1, 0); }
    else { w = float3(-t.z, 1.0 - t.y, 1.0 - t.x); vtx[0] = base + int2(1, 1); vtx[1] = base + int2(1, 0); vtx[2] = base + int2(0, 1); }
    w = w * w * w * w;
    w /= w.x + w.y + w.z;
    SamplerState smp = g_samplers[SAMPLER_ANISO_REPEAT];
    for (uint j = 0; j < 3; j++) {
        uint h = hash_u32(uint(vtx[j].x) * 73856093u ^ uint(vtx[j].y) * 19349663u ^ (k + 1u) * 0x9e3779b9u);
        uint r = h & 3u;
        float2 off = float2(hash_f(h + 1u), hash_f(h + 2u));
        float2 uvj = rot90(uv, r) + off;
        float4 c = g_textures[TEX_COVER + 2 * k].SampleGrad(smp, uvj, rot90(duvx, r), rot90(duvy, r));
        float4 s = g_textures[TEX_COVER + 2 * k + 1].SampleGrad(smp, uvj, rot90(duvx, r), rot90(duvy, r));
        s.xy = rot90(s.xy * 2.0 - 1.0, (4u - r) & 3u) * 0.5 + 0.5;     // texture-space normal back to world xz
        color += c * (w[j] * weight);
        surf += s * (w[j] * weight);
    }
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

    // Placeholder terrain materials until the terrain layers and RVT exist (docs/12), driven by the vegetation mask.
    // Ground cover: the baked top view of the habitat mix (ground_cover.cpp), with the bare ground (thatch, litter)
    // where it has no coverage. Near the camera, under the meshes, it stays as the darker lower layer; towards grazing angles blade sides hide the ground (turbid layer: taller cover hides more). Beyond the
    // tree draw distance forests take the canopy colour; slopes show soil and rock. Noise and tiling use terrain-local
    // coordinates, so nothing moves with the render origin.
    float3 to_cam = f.cam_pos.xyz - i.rel;
    float dist = length(to_cam);
    float3 v = to_cam / dist;
    float2 mask = mask_at(f, i.grid, max(0.0, lod - 2.0));
    float variation = value_noise(i.grid / 45.0) * 0.6 + value_noise(i.grid / 9.0) * 0.4;
    float2 pm = i.grid * f.terrain.z;                                   // terrain-local metres

    float3 hab = gc_habitat(mask);
    float hsum = hab.x + hab.y + hab.z;
    float3 hw = hab / max(hsum, 1e-4);
    float2 uv = pm / GC_BAKE_TILE, duvx = ddx(uv), duvy = ddy(uv);
    float4 cover_color = float4(0, 0, 0, 0), cover_surf = float4(0, 0, 0, 0);
    for (uint k = 0; k < GC_HABITATS; k++)
        if (hw[k] > 0.03) cover_tiled(k, uv, duvx, duvy, hw[k], cover_color, cover_surf);
    float near = (f.veg_flags & VEG_COVER) != 0 ? smoothstep(GC_NEAR_DISTANCE * f.veg.y * 0.55, GC_NEAR_DISTANCE * f.veg.y * 0.95, dist) : 1.0;
    float tall = meadow_height(pm) * hw.x + 0.3 * hw.y + 0.15 * hw.z;   // m, rough mean cover height
    float cos_v = saturate(dot(n, v));
    float a = saturate(cover_color.a * saturate(hsum * 1.25) * lerp(0.85, 1.0, near));
    a = 1.0 - pow(1.0 - a, 1.0 + 2.0 * tall / max(cos_v, 0.08));
    float3 top = cover_color.rgb / max(cover_color.a, 1e-3) * lerp(0.9, 1.1, variation) * lerp(0.7, 1.0, near) *
                 lerp(float3(1, 1, 1), float3(1.2, 1.08, 0.82), meadow_dryness(pm) * hw.x);
    float2 nxz = cover_surf.xy * 2.0 - 1.0;
    float3 n_cover = normalize(n + float3(nxz.x, 0, nxz.y) * 1.5);
    float3 litter = lerp(cover_tint(f, pm) * float3(0.6, 0.55, 0.42), FOREST_FLOOR_ALBEDO, hw.z);

    float sh = sun_shadow(f, i.rel, n, i.pos.xy);
    float3 light = SUN_COLOR * saturate(dot(n, f.sun_dir.xyz)) * sh + SKY_AMBIENT * (0.6 + 0.4 * n.y);
    float3 c = lerp(litter * light, shade_foliage(f, top, n_cover, v, 1.0, 0.6, sh), a);
    float canopy = (f.veg_flags & VEG_TREES) != 0 ? smoothstep(f.veg.x * 0.7, f.veg.x, dist) : 1.0;
    c = lerp(c, CANOPY_ALBEDO * light, smoothstep(0.25, 0.6, mask.x) * canopy);
    c = lerp(c, float3(0.24, 0.19, 0.13) * light, smoothstep(0.85, 0.7, n.y));
    c = lerp(c, float3(0.32, 0.31, 0.29) * light, smoothstep(0.7, 0.55, n.y));
    if (f.debug_mode == DEBUG_LOD) c = DEBUG_TINT[i.level % 6] * 0.4 * light;
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
