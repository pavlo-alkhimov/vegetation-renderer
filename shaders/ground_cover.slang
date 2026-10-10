// Ground cover near field: Poly Haven scans (src/ground_cover.cpp) placed every frame by hash from the vegetation mask.
// Task shader: one cell of a layer (gpu_shared.h), GC_CELL_SLOTS candidates (species by habitat, variant, scale,
// distance fade, frustum cull, LOD by projected height). Mesh shader: meshlets with wind. Fragment: alpha-tested
// scanned cards. The same shaders bake the top view of each habitat at load time (FrameConstants.gc_bake): a
// seamless tile on flat ground, every instance at LOD 0, no wind.
#include "common.slang"

struct GcPayload {
    float4 pos[GC_CELL_SLOTS];       // xyz = base relative to the render origin, w = scale
    uint info[GC_CELL_SLOTS];        // variant | lod << 8 | yaw << 16 | tint << 24
    uint count[GC_CELL_SLOTS];       // mesh workgroups per slot (task-internal)
    uint prefix[GC_CELL_SLOTS + 1];
};
groupshared GcPayload s_gc;

static const float GC_LOD_PX[2] = { 120.0, 40.0 };   // projected height above which LOD 0 / 1 is used

[shader("amplification")]
[numthreads(GC_CELL_SLOTS, 1, 1)]
void as_gc(uint tid : SV_GroupThreadID, uint3 gid : SV_GroupID)
{
    FrameConstants f = *pc.frame;
    GcScene ps = *pc.gc;
    bool bake = f.gc_bake.w > 0.0, imp_bake = f.gc_bake.w < 0.0;
    uint layer = gid.z;
    float cell = GC_LAYER_CELL[layer];
    int cx = f.gc_cell_x[layer] + int(gid.x), cz = f.gc_cell_z[layer] + int(gid.y);
    // Bake: cells repeat with the tile period (copies beyond the edges make the tile seamless).
    int hx = cx, hz = cz;
    if (bake) {
        int n = int(round(f.gc_bake.w / cell));
        hx = ((cx % n) + n) % n;
        hz = ((cz % n) + n) % n;
    }
    uint meshlets = 0;
    uint h = hash_u32(uint(hx) * 73856093u ^ uint(hz) * 19349663u ^ (tid + 1u) * 83492791u ^ (layer + 1u) * 0x2f6b1d3u ^
                      f.gc_bake_pass * 0x51ed270bu);
    // Stratified 8 x 4 positions inside the cell, so instances do not pile up.
    float2 local = (float2(tid & 7u, tid >> 3) + float2(hash_f(h), hash_f(h + 1u))) / float2(8.0, 4.0);
    float2 pm = (float2(cx, cz) + local) * cell;                           // terrain-local metres (bake: tile-local)
    float2 g = pm / f.terrain.z;
    // Whole cell first (uniform branch): bounding sphere from the terrain height at the cell centre; instances up to
    // ~2 m tall, terrain slope within the cell bounded by the radius.
    float2 cm = (float2(cx, cz) + 0.5) * cell;
    float2 cg = clamp(cm / f.terrain.z, 0.0, f.terrain_size.xy - 1.0);
    float3 cc = float3(f.terrain.x + cm.x, bake ? 1.0 : height_at(f, cg, 0) + 1.0, f.terrain.y + cm.y);
    bool cell_visible = gid.x < f.gc_cells[layer] && gid.y < f.gc_cells[layer] && sphere_visible(cc, cell * 1.5 + 2.0) &&
                        (bake || distance(cc, f.cam_pos.xyz) < (layer == 0 ? GC_NEAR_DISTANCE : 45.0) + cell * 1.5);
    if (!imp_bake && cell_visible && (bake || (all(g >= 0.0) && all(g <= f.terrain_size.xy - 1.0)))) {
        float3 hab = bake ? f.gc_bake.xyz : gc_habitat(mask_at(f, g, 0));
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
            float3 base = float3(f.terrain.x + pm.x, bake ? 0.0 : height_at(f, g, 0), f.terrain.y + pm.y);
            float dist = bake ? 1.0 : distance(base, f.cam_pos.xyz);
            float fade = smoothstep(sp.shape.z * 0.75, sp.shape.z, dist);   // dithered: no visible ring
            // Dense layer: fewer, larger instances with distance (coverage stays, cost falls).
            float keep = layer == 0 ? saturate(pow(6.0 / max(dist, 1e-3), 1.2)) : 1.0;
            if (hash_f(h + 3u) >= fade && hash_f(h + 8u) < keep) {
                uint variant = sp.first_variant + min(uint(hash_f(h + 4u) * float(sp.variant_count)), sp.variant_count - 1u);
                GcVariant pv = ps.variants[variant];
                float scale = lerp(sp.shape.x, sp.shape.y, hash_f(h + 5u)) * min(rsqrt(max(keep, 1e-3)), 1.5);
                // Tall and short meadow patches (the bake shows the average meadow; the terrain varies it).
                if ((uint(sp.habitat.w) & 2u) != 0 && !bake) scale *= clamp(meadow_height(pm) / 0.45, 0.5, 1.4);
                float hgt = pv.height * scale, r = pv.radius * scale + 0.05;
                if (sphere_visible(base + float3(0, hgt * 0.5, 0), sqrt(r * r + hgt * hgt * 0.25))) {
                    float px = hgt * f.veg.z / max(dist, 0.05);
                    uint lod = px > GC_LOD_PX[0] ? 0u : px > GC_LOD_PX[1] ? 1u : 2u;
                    bool shadow_view = pc.view->jitter.z > 0.0;                     // sparsest cards, tall instances only
                    if (shadow_view) lod = GC_LODS - 1;
                    if (bake) lod = 0;
                    bool draw = bake || (shadow_view ? hgt >= 0.35 : px > 8.0);     // below ~8 px the terrain carries it
                    meshlets = draw ? pv.meshlet_count[lod] : 0u;
                    s_gc.pos[tid] = float4(base, scale);
                    s_gc.info[tid] = variant | lod << 8 | (hash_u32(h + 6u) & 255u) << 16 | (hash_u32(h + 7u) & 255u) << 24;
                }
            }
        }
    }
    if (imp_bake && all(gid == 0) && tid == 0) {                      // impostor bake: one instance at the origin
        meshlets = ps.variants[f.gc_bake_pass].meshlet_count[0];
        s_gc.pos[0] = float4(0, 0, 0, 1);
        s_gc.info[0] = f.gc_bake_pass | 128u << 24;
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

// Surface of a scanned card at a fragment: albedo (tinted), shading normal (normal map in a cotangent frame from
// screen derivatives; glTF uv has v down, the OpenGL normal map +y up, hence -B), geometric normal, AO.
// Returns false where the alpha test discards.
bool gc_surface(GcVsOut i, bool front, out float3 albedo, out float3 nm, out float3 n, out float ao)
{
    GcSpecies sp = pc.gc->species[i.info & 255u];
    SamplerState smp = g_samplers[SAMPLER_ANISO_REPEAT];
    float4 a = g_textures[sp.albedo_tex].Sample(smp, i.uv);              // sRGB view: linear rgb
    albedo = float3(0, 0, 0); nm = float3(0, 1, 0); n = nm; ao = 1.0;
    if ((uint(sp.habitat.w) & 1u) != 0 && a.a < 0.5) return false;
    float4 surf = g_textures[sp.surface_tex].Sample(smp, i.uv);           // normal xy, AO, roughness
    n = normalize(i.n);
    if (!front) n = -n;
    float3 dp1 = ddx(i.rel), dp2 = ddy(i.rel);
    float2 duv1 = ddx(i.uv), duv2 = ddy(i.uv);
    float3 dp2perp = cross(dp2, n), dp1perp = cross(n, dp1);
    float3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    float3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float inv = rsqrt(max(max(dot(T, T), dot(B, B)), 1e-20));
    float2 tn = (surf.xy * 2.0 - 1.0) * 0.8;
    nm = normalize(T * inv * tn.x - B * inv * tn.y + n * sqrt(saturate(1.0 - dot(tn, tn))));
    if (any(isnan(nm))) nm = n;
    float tint = float((i.info >> 16) & 255u) / 255.0;
    albedo = a.rgb * lerp(float3(0.85, 0.9, 0.95), float3(1.1, 1.08, 0.92), tint);
    if ((uint(sp.habitat.w) & 2u) != 0) {                                  // meadow grasses: season greenness
        FrameConstants f = *pc.frame;
        float3 lw = float3(0.3, 0.6, 0.1);
        float dry = saturate(1.0 - 4.0 * (albedo.g - albedo.r) / max(albedo.g, 1e-3));   // straw: r ~ g
        float3 green = f.cover.rgb * (dot(albedo, lw) / max(dot(f.cover.rgb, lw), 1e-4));
        albedo = lerp(albedo, green, dry * f.cover.w * lerp(0.6, 1.0, tint));
    }
    ao = surf.z * lerp(0.4, 1.0, saturate(i.t * 2.5));                     // darker towards the ground
    return true;
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
    float3 albedo, nm, n;
    float ao;
    if (!gc_surface(i, front, albedo, nm, n, ao)) discard;
    if (f.debug_mode == DEBUG_LOD) albedo = DEBUG_TINT[(i.info >> 8) & 255u] * 0.4;
    if (f.debug_mode == DEBUG_NORMALS) return scene_out(f, nm * 0.5 + 0.5, i.pos, i.prev);
    float3 to_cam = normalize(f.cam_pos.xyz - i.rel);
    float3 c = shade_foliage(f, albedo, nm, to_cam, ao, 0.7, sun_shadow(f, i.rel, n, i.pos.xy));
    return scene_out(f, tonemap(apply_fog(f, c, i.rel)), i.pos, i.prev);
}

// Bake: top view of the topmost surface. colour = albedo x AO with coverage 1 (cleared to 0: ground); surface =
// upward-facing normal xz and height above the ground.
struct GcBakeOut {
    float4 color : SV_Target0;
    float4 surface : SV_Target1;
};

[shader("fragment")]
GcBakeOut fs_gc_bake(GcVsOut i, bool front : SV_IsFrontFace)
{
    float3 albedo, nm, n;
    float ao;
    if (!gc_surface(i, front, albedo, nm, n, ao)) discard;
    if (nm.y < 0.0) nm = -nm;                                             // seen from above
    GcBakeOut o;
    o.color = float4(albedo * ao, 1.0);
    o.surface = float4(nm.xz * 0.5 + 0.5, saturate(i.rel.y / 1.5), 1.0);
    return o;
}

// Impostor bake: colour as above, full normal in the instance frame (yaw 0).
[shader("fragment")]
GcBakeOut fs_gc_imp_bake(GcVsOut i, bool front : SV_IsFrontFace)
{
    float3 albedo, nm, n;
    float ao;
    if (!gc_surface(i, front, albedo, nm, n, ao)) discard;
    GcBakeOut o;
    o.color = float4(albedo * ao, 1.0);
    o.surface = float4(nm * 0.5 + 0.5, 1.0);
    return o;
}

// ---------------------------------------------------------------------------------------------------------------
// Mid field: impostors (one per instance, a camera-facing quad) placed in GC_IMP_CELL cells by the same habitat
// densities as the meshes. An impostor stands for several plants: it is widened horizontally (never vertically,
// so heights stay right) by the density it represents. Fades in where the meshes fade out and out towards
// GC_IMP_DISTANCE, where the baked top view on the terrain carries the ground cover alone.

// Hemi-octahedral mapping of the upper hemisphere (y up) to [-1, 1]^2 (Cigolle et al. 2014).
float2 hemioct_encode(float3 v)
{
    float2 p = v.xz / (abs(v.x) + abs(v.y) + abs(v.z));
    return float2(p.x + p.y, p.x - p.y);
}

float3 hemioct_decode(float2 e)
{
    float2 t = float2(e.x + e.y, e.x - e.y) * 0.5;
    return normalize(float3(t.x, 1.0 - abs(t.x) - abs(t.y), t.y));
}

// Frame basis for a view direction d (towards the viewer): right, up; consistent between bake and runtime.
void imp_basis(float3 d, out float3 right, out float3 up)
{
    float3 fwd = -d;
    float3 r = cross(float3(0, 1, 0), fwd);
    right = dot(r, r) > 1e-6 ? normalize(r) : float3(1, 0, 0);
    up = cross(fwd, right);
}

struct GcImpPayload {
    float4 pos[GC_CELL_SLOTS];          // xyz = base, w = scale
    float widen[GC_CELL_SLOTS];         // horizontal widening
    uint info[GC_CELL_SLOTS];           // variant | yaw << 16 | tint << 24
    uint count;
};
groupshared GcImpPayload s_imp;

[shader("amplification")]
[numthreads(GC_CELL_SLOTS, 1, 1)]
void as_gc_imp(uint tid : SV_GroupThreadID, uint3 gid : SV_GroupID)
{
    FrameConstants f = *pc.frame;
    GcScene ps = *pc.gc;
    if (tid == 0) s_imp.count = 0;
    GroupMemoryBarrierWithGroupSync();
    const float cell = GC_IMP_CELL;
    int cx = f.gc_imp_cell_x + int(gid.x), cz = f.gc_imp_cell_z + int(gid.y);
    float2 cm = (float2(cx, cz) + 0.5) * cell;
    float2 cg = clamp(cm / f.terrain.z, 0.0, f.terrain_size.xy - 1.0);
    float3 cc = float3(f.terrain.x + cm.x, height_at(f, cg, 0) + 1.0, f.terrain.y + cm.y);
    float cd = distance(cc, f.cam_pos.xyz);
    bool cell_visible = gid.x < f.gc_imp_cells && gid.y < f.gc_imp_cells && cd > GC_NEAR_DISTANCE * 0.7 - cell * 1.5 &&
                        cd < GC_IMP_DISTANCE + cell * 1.5 && sphere_visible(cc, cell * 1.5 + 2.0);
    uint h = hash_u32(uint(cx) * 73856093u ^ uint(cz) * 19349663u ^ (tid + 1u) * 83492791u ^ 0x1a5f00d7u);
    float2 local = (float2(tid & 7u, tid >> 3) + float2(hash_f(h), hash_f(h + 1u))) / float2(8.0, 4.0);
    float2 pm = (float2(cx, cz) + local) * cell;
    float2 g = pm / f.terrain.z;
    if (cell_visible && all(g >= 0.0) && all(g <= f.terrain_size.xy - 1.0)) {
        float3 hab = gc_habitat(mask_at(f, g, 0));
        float slot_area = cell * cell / float(GC_CELL_SLOTS);
        float total = 0.0;
        for (uint s = 0; s < ps.species_count; s++) total += dot(ps.species[s].habitat.xyz, hab) * slot_area;
        float u = hash_f(h + 2u) * max(total, 1.0), acc = 0.0;          // dense habitats: every slot, mixed by density
        int sel = -1;
        for (uint s = 0; s < ps.species_count; s++) {
            acc += dot(ps.species[s].habitat.xyz, hab) * slot_area;
            if (sel < 0 && u < acc) sel = int(s);
        }
        if (sel >= 0) {
            GcSpecies sp = ps.species[sel];
            float3 base = float3(f.terrain.x + pm.x, height_at(f, g, 0), f.terrain.y + pm.y);
            float dist = distance(base, f.cam_pos.xyz);
            float fade_in = smoothstep(sp.shape.z * 0.7, sp.shape.z, dist);        // where this species' meshes fade out
            float fade_out = smoothstep(GC_IMP_DISTANCE * 0.7, GC_IMP_DISTANCE, dist);
            float keep = saturate(30.0 / dist);
            if (hash_f(h + 3u) < fade_in && hash_f(h + 9u) >= fade_out && hash_f(h + 8u) < keep) {
                uint variant = sp.first_variant + min(uint(hash_f(h + 4u) * float(sp.variant_count)), sp.variant_count - 1u);
                GcVariant pv = ps.variants[variant];
                float scale = lerp(sp.shape.x, sp.shape.y, hash_f(h + 5u));
                if ((uint(sp.habitat.w) & 2u) != 0) scale *= clamp(meadow_height(pm) / 0.45, 0.5, 1.4);
                float widen = min(sqrt(max(total, 1.0) / keep), 4.0);
                float r = pv.bound * scale * widen;
                float px = pv.height * scale * f.veg.z / max(dist, 0.05);
                if (px > 1.5 && sphere_visible(base + float3(0, pv.height * scale * 0.5, 0), r)) {
                    uint k;
                    InterlockedAdd(s_imp.count, 1u, k);
                    s_imp.pos[k] = float4(base, scale);
                    s_imp.widen[k] = widen;
                    s_imp.info[k] = variant | (hash_u32(h + 6u) & 255u) << 16 | (hash_u32(h + 7u) & 255u) << 24;
                }
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    DispatchMesh(s_imp.count > 0 ? 1 : 0, 1, 1, s_imp);
}

struct GcImpVsOut {
    float4 pos : SV_Position;
    float3 rel : REL;
    float4 prev : PREV;
    float2 uv : UV;                                 // 0..1 across the quad
    nointerpolation float2 frame : FRAME;           // continuous frame coordinates (0 .. GC_IMP_FRAMES - 1)
    nointerpolation uint info : INFO;               // variant | yaw << 16 | tint << 24
};

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(GC_CELL_SLOTS, 1, 1)]
void ms_gc_imp(uint tid : SV_GroupThreadID, in payload GcImpPayload p,
               out indices uint3 tris[GC_CELL_SLOTS * 2], out vertices GcImpVsOut verts[GC_CELL_SLOTS * 4])
{
    FrameConstants f = *pc.frame;
    uint count = p.count;
    SetMeshOutputCounts(count * 4, count * 2);
    if (tid >= count) return;
    GcScene ps = *pc.gc;
    uint info = p.info[tid];
    GcVariant pv = ps.variants[info & 255u];
    float scale = p.pos[tid].w, widen = p.widen[tid];
    float3 centre = p.pos[tid].xyz + float3(0, pv.height * scale * 0.5, 0);
    float r = pv.bound * scale;
    float yaw = float((info >> 16) & 255u) / 256.0 * 6.2831853;
    float cy = cos(yaw), sy = sin(yaw);
    float3 d = normalize(f.cam_pos.xyz - centre);
    // View direction in the instance frame (yaw 0, as baked), kept just above the horizon.
    float3 dl = float3(d.x * cy + d.z * sy, max(d.y, 0.02), -d.x * sy + d.z * cy);
    dl = normalize(dl);
    float2 e = hemioct_encode(dl);
    float2 frame = clamp((e * 0.5 + 0.5) * GC_IMP_FRAMES - 0.5, 0.0, GC_IMP_FRAMES - 1.0);
    float3 right, up;
    imp_basis(d, right, up);
    float3 hr = right * r * widen, hu = up * r;
    float2 corner[4] = { float2(0, 0), float2(1, 0), float2(0, 1), float2(1, 1) };
    for (uint k = 0; k < 4; k++) {
        float2 c = corner[k];
        float3 wp = centre + hr * (c.x * 2.0 - 1.0) - hu * (c.y * 2.0 - 1.0);   // uv.y down = up
        GcImpVsOut o;
        o.rel = wp;
        o.pos = project(wp);
        o.prev = project_prev(wp);
        o.uv = c;
        o.frame = frame;
        o.info = info;
        verts[tid * 4 + k] = o;
    }
    tris[tid * 2] = uint3(tid * 4, tid * 4 + 1, tid * 4 + 2);
    tris[tid * 2 + 1] = uint3(tid * 4 + 1, tid * 4 + 3, tid * 4 + 2);
}

[shader("fragment")]
SceneOut fs_gc_imp(GcImpVsOut i)
{
    FrameConstants f = *pc.frame;
    uint variant = i.info & 255u;
    // Dither between the neighbouring frames by their fractional weights (TAA blends them).
    float2 fr = floor(i.frame), w = i.frame - fr;
    float rx = hash(i.pos.xy + frac(f.cam_fwd.w * 7.13) * 61.0), ry = hash(i.pos.yx * 1.37 + frac(f.cam_fwd.w * 3.71) * 47.0);
    float2 frame = min(fr + float2(rx < w.x ? 1.0 : 0.0, ry < w.y ? 1.0 : 0.0), GC_IMP_FRAMES - 1.0);
    float2 tile = float2(variant % GC_IMP_GRID, variant / GC_IMP_GRID);
    float2 auv = ((tile * GC_IMP_FRAMES + frame) * GC_IMP_FRAME + 0.5 + i.uv * (GC_IMP_FRAME - 1.0)) / GC_IMP_ATLAS;
    float2 g = float2(GC_IMP_FRAME - 1.0, GC_IMP_FRAME - 1.0) / GC_IMP_ATLAS;
    SamplerState smp = g_samplers[SAMPLER_LINEAR_CLAMP];
    float4 c = g_textures[TEX_IMPOSTOR].SampleGrad(smp, auv, ddx(i.uv) * g, ddy(i.uv) * g);
    if (c.a < 0.5) discard;
    float4 s = g_textures[TEX_IMPOSTOR + 1].SampleGrad(smp, auv, ddx(i.uv) * g, ddy(i.uv) * g);
    float yaw = float((i.info >> 16) & 255u) / 256.0 * 6.2831853;
    float cy = cos(yaw), sy = sin(yaw);
    float3 nl = normalize(s.xyz * 2.0 - 1.0);
    float3 n = float3(nl.x * cy - nl.z * sy, nl.y, nl.x * sy + nl.z * cy);
    float3 albedo = c.rgb / c.a;
    if (f.debug_mode == DEBUG_LOD) albedo = DEBUG_TINT[3] * 0.4;
    if (f.debug_mode == DEBUG_NORMALS) return scene_out(f, n * 0.5 + 0.5, i.pos, i.prev);
    float3 to_cam = normalize(f.cam_pos.xyz - i.rel);
    float3 col = shade_foliage(f, albedo, n, to_cam, 1.0, 0.7, sun_shadow(f, i.rel, n, i.pos.xy));
    return scene_out(f, tonemap(apply_fog(f, col, i.rel)), i.pos, i.prev);
}