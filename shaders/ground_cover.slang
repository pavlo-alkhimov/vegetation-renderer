// Near-field ground cover: Poly Haven plant assets (src/ground_cover.cpp) placed every frame by hash from the vegetation
// mask. Task shader: one cell of a layer (gpu_shared.h), GC_CELL_SLOTS candidates (species by habitat, variant, scale, distance
// fade, frustum cull, LOD by projected height). Mesh shader: meshlets with wind. Fragment: alpha-tested scanned cards.
#include "common.slang"

struct GcPayload {
    float4 pos[GC_CELL_SLOTS];       // xyz = base relative to the render origin, w = scale
    uint info[GC_CELL_SLOTS];        // variant | lod << 8 | yaw << 16 | tint << 24
    uint count[GC_CELL_SLOTS];       // mesh workgroups per slot (task-internal)
    uint prefix[GC_CELL_SLOTS + 1];
};
groupshared GcPayload s_gc;

static const float GC_LOD_PX[2] = { 120.0, 40.0 };   // projected height above which LOD 0 / 1 is used

// Habitat weights from the vegetation mask (r = forest, g = ground cover): meadow, forest edge, forest floor.
float3 gc_habitat(float2 mask)
{
    float forest = mask.x;
    return float3(mask.y * (1.0 - forest), 4.0 * forest * (1.0 - forest), forest * forest);
}

[shader("amplification")]
[numthreads(GC_CELL_SLOTS, 1, 1)]
void as_gc(uint tid : SV_GroupThreadID, uint3 gid : SV_GroupID)
{
    FrameConstants f = *pc.frame;
    GcScene ps = *pc.gc;
    uint layer = gid.z;
    float cell = GC_LAYER_CELL[layer];
    int cx = f.gc_cell_x[layer] + int(gid.x), cz = f.gc_cell_z[layer] + int(gid.y);
    uint meshlets = 0;
    uint h = hash_u32(uint(cx) * 73856093u ^ uint(cz) * 19349663u ^ (tid + 1u) * 83492791u ^ (layer + 1u) * 0x2f6b1d3u);
    // Stratified 8 x 4 positions inside the cell, so cover do not pile up.
    float2 local = (float2(tid & 7u, tid >> 3) + float2(hash_f(h), hash_f(h + 1u))) / float2(8.0, 4.0);
    float2 pm = (float2(cx, cz) + local) * cell;                           // terrain-local metres
    float2 g = pm / f.terrain.z;
    // Whole cell first (uniform branch): bounding sphere from the terrain height at the cell centre; cover up to
    // ~2 m tall, terrain slope within the cell bounded by the radius.
    float2 cm = (float2(cx, cz) + 0.5) * cell;
    float2 cg = clamp(cm / f.terrain.z, 0.0, f.terrain_size.xy - 1.0);
    float3 cc = float3(f.terrain.x + cm.x, height_at(f, cg, 0) + 1.0, f.terrain.y + cm.y);
    bool cell_visible = gid.x < f.gc_cells[layer] && gid.y < f.gc_cells[layer] && sphere_visible(cc, cell * 1.5 + 2.0) &&
                        distance(cc, f.cam_pos.xyz) < (layer == 0 ? GC_NEAR_DISTANCE : 45.0) + cell * 1.5;
    if (cell_visible && all(g >= 0.0) && all(g <= f.terrain_size.xy - 1.0)) {
        float3 hab = gc_habitat(mask_at(f, g, 0));
        float slot_area = cell * cell / float(GC_CELL_SLOTS);            // m² per candidate
        float u = hash_f(h + 2u), acc = 0.0;
        int sel = -1;
        for (uint s = 0; s < ps.species_count; s++) {
            GcSpecies sp = ps.species[s];
            if ((sp.shape.z <= GC_NEAR_DISTANCE) != (layer == 0)) continue;
            acc += dot(sp.habitat.xyz, hab) * slot_area;
            if (sel < 0 && u < acc) sel = int(s);
        }
        if (sel >= 0) {
            GcSpecies sp = ps.species[sel];
            float3 base = float3(f.terrain.x + pm.x, height_at(f, g, 0), f.terrain.y + pm.y);
            float dist = distance(base, f.cam_pos.xyz);
            float fade = smoothstep(sp.shape.z * 0.75, sp.shape.z, dist);   // dithered: no visible ring
            // Dense layer: fewer, larger cover with distance (coverage stays, cost falls).
            float keep = layer == 0 ? saturate(pow(6.0 / max(dist, 1e-3), 1.2)) : 1.0;
            if (hash_f(h + 3u) >= fade && hash_f(h + 8u) < keep) {
                uint variant = sp.first_variant + min(uint(hash_f(h + 4u) * float(sp.variant_count)), sp.variant_count - 1u);
                GcVariant pv = ps.variants[variant];
                float scale = lerp(sp.shape.x, sp.shape.y, hash_f(h + 5u)) * min(rsqrt(max(keep, 1e-3)), 1.5);
                if ((uint(sp.habitat.w) & 2u) != 0) scale *= clamp(meadow_height(pm) / 0.45, 0.5, 1.4);   // tall and short meadow patches
                float hgt = pv.height * scale, r = pv.radius * scale + 0.05;
                if (sphere_visible(base + float3(0, hgt * 0.5, 0), sqrt(r * r + hgt * hgt * 0.25))) {
                    float px = hgt * f.veg.z / max(dist, 0.05);
                    uint lod = px > GC_LOD_PX[0] ? 0u : px > GC_LOD_PX[1] ? 1u : 2u;
                    bool shadow_view = pc.view->jitter.z > 0.0;                     // sparsest cards, tall cover only
                    if (shadow_view) lod = GC_LODS - 1;
                    bool draw = shadow_view ? hgt >= 0.35 : px > 8.0;               // below ~8 px the terrain carries it
                    meshlets = draw ? pv.meshlet_count[lod] : 0u;
                    s_gc.pos[tid] = float4(base, scale);
                    s_gc.info[tid] = variant | lod << 8 | (hash_u32(h + 6u) & 255u) << 16 | (hash_u32(h + 7u) & 255u) << 24;
                }
            }
        }
    }
    s_gc.count[tid] = meshlets;
    GroupMemoryBarrierWithGroupSync();
    if (tid == 0) {
        uint sum = 0;
        for (uint i = 0; i < GC_CELL_SLOTS; i++) { s_gc.prefix[i] = sum; sum += s_gc.count[i]; }
        s_gc.prefix[GC_CELL_SLOTS] = sum;
    }
    GroupMemoryBarrierWithGroupSync();
    DispatchMesh(s_gc.prefix[GC_CELL_SLOTS], 1, 1, s_gc);
}

struct GcVsOut {
    float4 pos : SV_Position;
    float3 rel : REL;
    float4 prev : PREV;                     // previous frame's clip position
    float3 n : NRM;
    float2 uv : UV;
    float t : HEIGHT;                       // 0 at the base .. 1 at the instance's top
    nointerpolation uint info : INFO;       // species | lod << 8 | tint << 16
};

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(32, 1, 1)]
void ms_gc(uint tid : SV_GroupThreadID, uint gid : SV_GroupID, in payload GcPayload p,
               out indices uint3 tris[TREE_MESHLET_TRIS], out vertices GcVsOut verts[TREE_MESHLET_VERTS])
{
    FrameConstants f = *pc.frame;
    GcScene ps = *pc.gc;
    uint slot = find_slot(p.prefix, gid);
    uint info = p.info[slot];
    uint variant = info & 255u, lod = (info >> 8) & 255u;
    GcVariant pv = ps.variants[variant];
    GcSpecies sp = ps.species[pv.species];
    TreeMeshlet m = ps.meshlets[pv.meshlet_first[lod] + gid - p.prefix[slot]];
    SetMeshOutputCounts(m.vertex_count, m.triangle_count);

    float3 base = p.pos[slot].xyz;
    float scale = p.pos[slot].w;
    float yaw = float((info >> 16) & 255u) / 256.0 * 6.2831853;
    float cy = cos(yaw), sy = sin(yaw);
    float hgt = pv.height * scale;
    float2 pm = base.xz - f.terrain.xy;                                 // terrain-local metres (for the gust phase)
    float phase = float((info >> 24) & 255u) / 255.0;
    float2 bend_dir = f.wind.xz * f.wind.y * sp.shape.w * 0.35 * hgt;
    float2 bend = bend_dir * wind_gust(f, f.cam_fwd.w, pm, phase), bend_prev = bend_dir * wind_gust(f, f.taa.x, pm, phase);

    for (uint v = tid; v < m.vertex_count; v += 32) {
        TreeVertex tv = ps.vertices[m.vertex_offset + v];
        float w = float((tv.attr >> 16) & 255u) / 255.0;
        uint seed = tv.attr >> 24;
        float3 lp = float3(tv.x, tv.y, tv.z) * scale;
        float3 n = unpack_normal(tv.normal);
        float3 rp = float3(lp.x * cy - lp.z * sy, lp.y, lp.x * sy + lp.z * cy);
        float3 rn = float3(n.x * cy - n.z * sy, n.y, n.x * sy + n.z * cy);
        float flutter_amp = 0.04 * hgt * f.wind.y * sp.shape.w;
        float2 side = float2(-f.wind.z, f.wind.x);
        float2 off = (bend + flutter_amp * sin(f.cam_fwd.w * 5.3 + float(seed) * 0.37) * side) * w;
        float2 off_prev = (bend_prev + flutter_amp * sin(f.taa.x * 5.3 + float(seed) * 0.37) * side) * w;
        float denom = max(2.0 * max(rp.y, 0.02), 1e-3);              // keep the stem length roughly
        GcVsOut o;
        o.rel = base + rp + float3(off.x, -dot(off, off) / denom * w, off.y);
        o.pos = project(o.rel);
        o.prev = project_prev(base + rp + float3(off_prev.x, -dot(off_prev, off_prev) / denom * w, off_prev.y));
        o.n = rn;
        o.uv = float2(tv.uv & 0xffffu, tv.uv >> 16) * (8.0 / 65535.0) - 4.0;
        o.t = saturate(lp.y / max(hgt, 1e-3));
        o.info = pv.species | lod << 8 | ((info >> 24) & 255u) << 16;
        verts[v] = o;
    }
    for (uint t = tid; t < m.triangle_count; t += 32) {
        uint x = ps.triangles[m.triangle_offset + t];
        tris[t] = uint3(x & 255u, (x >> 8) & 255u, (x >> 16) & 255u);
    }
}

[shader("fragment")]
void fs_gc_shadow(GcVsOut i)
{
    GcSpecies sp = pc.gc->species[i.info & 255u];
    if ((uint(sp.habitat.w) & 1u) != 0 && g_textures[sp.albedo_tex].Sample(g_samplers[SAMPLER_ANISO_REPEAT], i.uv).a < 0.5) discard;
}

[shader("fragment")]
SceneOut fs_gc(GcVsOut i, bool front : SV_IsFrontFace)
{
    FrameConstants f = *pc.frame;
    GcSpecies sp = pc.gc->species[i.info & 255u];
    SamplerState smp = g_samplers[SAMPLER_ANISO_REPEAT];
    float4 albedo = g_textures[sp.albedo_tex].Sample(smp, i.uv);         // sRGB view: linear rgb
    if ((uint(sp.habitat.w) & 1u) != 0 && albedo.a < 0.5) discard;
    float4 surf = g_textures[sp.surface_tex].Sample(smp, i.uv);          // normal xy, AO, roughness

    // Normal map in a cotangent frame from screen derivatives (no stored tangents). glTF uv has v down, the
    // OpenGL normal map has +y up, hence -B.
    float3 n = normalize(i.n);
    if (!front) n = -n;
    float3 dp1 = ddx(i.rel), dp2 = ddy(i.rel);
    float2 duv1 = ddx(i.uv), duv2 = ddy(i.uv);
    float3 dp2perp = cross(dp2, n), dp1perp = cross(n, dp1);
    float3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    float3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float inv = rsqrt(max(max(dot(T, T), dot(B, B)), 1e-20));
    float2 tn = (surf.xy * 2.0 - 1.0) * 0.8;
    float3 nm = normalize(T * inv * tn.x - B * inv * tn.y + n * sqrt(saturate(1.0 - dot(tn, tn))));
    if (any(isnan(nm))) nm = n;

    float tint = float((i.info >> 16) & 255u) / 255.0;
    float3 c_albedo = albedo.rgb * lerp(float3(0.85, 0.9, 0.95), float3(1.1, 1.08, 0.92), tint);
    float ao = surf.z * lerp(0.4, 1.0, saturate(i.t * 2.5));            // darker towards the ground
    if (f.debug_mode == DEBUG_LOD) c_albedo = DEBUG_TINT[(i.info >> 8) & 255u] * 0.4;
    if (f.debug_mode == DEBUG_NORMALS) return scene_out(f, nm * 0.5 + 0.5, i.pos, i.prev);
    float3 to_cam = normalize(f.cam_pos.xyz - i.rel);
    float3 c = shade_foliage(f, c_albedo, nm, to_cam, ao, 0.7, sun_shadow(f, i.rel, n, i.pos.xy));
    return scene_out(f, tonemap(apply_fog(f, c, i.rel)), i.pos, i.prev);
}
