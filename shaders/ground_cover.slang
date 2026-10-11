// Ground cover near and mid range: Poly Haven scans (src/ground_cover.cpp) placed every frame by hash from the
// vegetation mask. Task shader: one cell of a layer (gpu_shared.h), GC_CELL_SLOTS candidates (species by habitat,
// variant, scale, distance fade, frustum cull). Each instance is drawn either as its complete mesh (near, up to
// GC_NEAR_DISTANCE: interaction, real parallax) or as two crossed curved stripes carrying baked side views (mid): same
// instances, so the switch changes neither density nor positions. Mesh shader: meshlets with wind, or 2 stripes per
// workgroup. The same shaders bake the top view of each habitat and the impostor / stripe views at load time
// (FrameConstants.gc_bake): flat ground, LOD 0, no wind.
#include "common.slang"

struct GcPayload {
    float4 pos[GC_CELL_SLOTS];       // xyz = base relative to the render origin, w = scale
    uint info[GC_CELL_SLOTS];        // variant | lod << 8 | yaw << 16 | tint << 24
    uint count[GC_CELL_SLOTS];       // mesh workgroups per slot (task-internal)
    uint prefix[GC_CELL_SLOTS + 1];  // mesh-instance workgroups; stripe workgroups follow
    uint stripe_slot[GC_CELL_SLOTS]; // slots drawn as stripes
    uint stripe_count;
};
groupshared GcPayload s_gc;

#define GC_STRIPES_PER_GROUP 2       // 2 x 30 vertices, 2 x 32 triangles: fits the meshlet output limits

[shader("amplification")]
[numthreads(GC_CELL_SLOTS, 1, 1)]
void as_gc(uint tid : SV_GroupThreadID, uint3 gid : SV_GroupID)
{
    FrameConstants f = *pc.frame;
    GcScene ps = *pc.gc;
    bool bake = f.gc_bake.w > 0.0, imp_bake = f.gc_bake.w < 0.0;
    bool shadow_view = pc.view->jitter.z > 0.0;
    if (tid == 0) s_gc.stripe_count = 0;
    GroupMemoryBarrierWithGroupSync();
    uint layer = gid.z;
    float cell = GC_LAYER_CELL[layer];
    float ds = f.veg.y;                                                     // ground-cover distance factor
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
                        (bake || distance(cc, f.cam_pos.xyz) < (layer == 0 ? GC_MID_DISTANCE : 45.0) * ds + cell * 1.5);
    if (!imp_bake && cell_visible && (bake || (all(g >= 0.0) && all(g <= f.terrain_size.xy - 1.0)))) {
        float3 hab = bake ? f.gc_bake.xyz : gc_habitat(mask_at(f, g, 0));
        float slot_area = cell * cell / float(GC_CELL_SLOTS);            // m² per candidate
        float u = hash_f(h + 2u), acc = 0.0;
        int sel = -1;
        for (uint s = 0; s < ps.species_count; s++) {
            GcSpecies sp = ps.species[s];
            if ((sp.shape.z <= GC_TALL_REACH) != (layer == 0)) continue;
            acc += dot(sp.habitat.xyz, hab) * slot_area;
            if (sel < 0 && u < acc) sel = int(s);
        }
        if (sel >= 0) {
            GcSpecies sp = ps.species[sel];
            float3 base = float3(f.terrain.x + pm.x, bake ? 0.0 : height_at(f, g, 0), f.terrain.y + pm.y);
            float dist = bake ? 1.0 : distance(base, f.cam_pos.xyz);
            float reach = sp.shape.z * ds;                                    // mid-range end x distance factor
            float fade = smoothstep(reach * 0.75, reach, dist);               // dithered: no visible ring
            // Full density through the first 40 % of the mid range, then fewer, larger instances (coverage kept).
            float keep = bake ? 1.0 : saturate(pow(0.4 * reach / max(dist, 1e-3), 1.2));
            if (hash_f(h + 3u) >= fade && hash_f(h + 8u) < keep) {
                uint variant = sp.first_variant + min(uint(hash_f(h + 4u) * float(sp.variant_count)), sp.variant_count - 1u);
                GcVariant pv = ps.variants[variant];
                float scale = lerp(sp.shape.x, sp.shape.y, hash_f(h + 5u)) * min(rsqrt(max(keep, 1e-3)), 1.5);
                // Tall and short meadow patches (the bake shows the average meadow; the terrain varies it).
                if ((uint(sp.habitat.w) & 2u) != 0 && !bake) scale *= clamp(meadow_height(pm) / 0.45, 0.5, 1.4);
                float hgt = pv.height * scale, r = pv.bound * scale + 0.05;
                if (sphere_visible(base + float3(0, hgt * 0.5, 0), r)) {
                    float px = hgt * f.veg.z / max(dist, 0.05);
                    float near = GC_NEAR_DISTANCE * ds;
                    bool stripe = !bake && hash_f(h + 10u) < smoothstep(near * 0.83, near * 1.17, dist);
                    bool draw = bake || (shadow_view ? hgt >= 0.35 : px > 8.0);     // below ~8 px the terrain carries it
                    uint lod = shadow_view ? GC_LODS - 1 : 0u;                      // shadows: sparsest cards
                    uint info = variant | lod << 8 | (hash_u32(h + 6u) & 255u) << 16 | (hash_u32(h + 7u) & 255u) << 24;
                    if (draw) {
                        s_gc.pos[tid] = float4(base, scale);
                        s_gc.info[tid] = info;
                        if (stripe) {
                            uint k;
                            InterlockedAdd(s_gc.stripe_count, 1u, k);
                            s_gc.stripe_slot[k] = tid;
                        } else {
                            meshlets = pv.meshlet_count[lod];
                        }
                    }
                }
            }
        }
    }
    if (imp_bake && all(gid == 0) && tid == 0) {                      // view bake / gallery: one instance at the origin
        uint rep = f.gallery != 0 ? f.gallery_rep : 0u;                  // mesh LOD 0-2, 3 stripe (4: impostor pipeline)
        s_gc.pos[0] = float4(0, 0, 0, 1);
        s_gc.info[0] = f.gc_bake_pass | min(rep, 2u) << 8 | 128u << 24;
        if (rep == 3u) { s_gc.stripe_slot[0] = 0; s_gc.stripe_count = 1; }
        else if (rep < 3u) meshlets = ps.variants[f.gc_bake_pass].meshlet_count[rep];
    }
    s_gc.count[tid] = meshlets;
    GroupMemoryBarrierWithGroupSync();
    if (tid == 0) {
        uint sum = 0;
        for (uint i = 0; i < GC_CELL_SLOTS; i++) { s_gc.prefix[i] = sum; sum += s_gc.count[i]; }
        s_gc.prefix[GC_CELL_SLOTS] = sum;
    }
    GroupMemoryBarrierWithGroupSync();
    DispatchMesh(s_gc.prefix[GC_CELL_SLOTS] + (s_gc.stripe_count + GC_STRIPES_PER_GROUP - 1) / GC_STRIPES_PER_GROUP, 1, 1, s_gc);
}

struct GcVsOut {
    float4 pos : SV_Position;
    float3 rel : REL;
    float4 prev : PREV;                     // previous frame's clip position
    float3 n : NRM;                         // geometric normal
    float2 uv : UV;                         // mesh: species atlas; stripe: stripe atlas
    float t : HEIGHT;                       // 0 at the base .. 1 at the instance's top
    nointerpolation uint info : INFO;       // species | lod << 8 | tint << 16 | yaw << 24 (lod GC_LOD_STRIPE: stripe)
};

// Frame basis for a view direction d (towards the viewer): right, up; shared by impostors and stripes, bake and runtime.
void imp_basis(float3 d, out float3 right, out float3 up)
{
    float3 fwd = -d;
    float3 r = cross(float3(0, 1, 0), fwd);
    right = dot(r, r) > 1e-6 ? normalize(r) : float3(1, 0, 0);
    up = cross(fwd, right);
}

// One stripe vertex: strip j (0: side view from +z, 1: from +x), column c (0..4 across), row r (0 top .. 2 bottom).
// The strip spans the baked frame (the plant's width and height, as the tight side-view bake), bulges horizontally into an arc
// (15 % of its width, opposite signs for the two strips) and bends with the plant's wind sway.
GcVsOut stripe_vertex(FrameConstants f, GcVariant pv, GcSpecies sp, float3 base, float scale, uint info, uint j, uint c, uint r)
{
    float yaw = float((info >> 16) & 255u) / 256.0 * 6.2831853;
    float cy = cos(yaw), sy = sin(yaw);
    float3 dview = j == 0 ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 right, up;
    imp_basis(dview, right, up);
    float half_w = (pv.radius * 1.04 + 0.005) * scale, half_h = (pv.height * 0.52 + 0.005) * scale, hgt = pv.height * scale;
    float u = float(c) / 4.0, v = float(r) / 2.0;
    float bulge = (j == 0 ? 1.0 : -1.0) * 0.15 * 2.0 * half_w * (1.0 - (2.0 * u - 1.0) * (2.0 * u - 1.0));
    float3 lp = right * ((2.0 * u - 1.0) * half_w) + float3(0, hgt * 0.5 + (1.0 - 2.0 * v) * half_h, 0) + dview * bulge;
    float3 rp = float3(lp.x * cy - lp.z * sy, lp.y, lp.x * sy + lp.z * cy);
    float3 rd = float3(dview.x * cy - dview.z * sy, 0, dview.x * sy + dview.z * cy);
    float w = pow(saturate(lp.y / max(hgt, 1e-3)), 1.5);
    float2 pm = base.xz - f.terrain.xy;
    float phase = float((info >> 24) & 255u) / 255.0;
    float2 bend_dir = f.wind.xz * f.wind.y * sp.shape.w * 0.35 * hgt * w;
    float2 off = bend_dir * wind_gust(f, f.cam_fwd.w, pm, phase), off_prev = bend_dir * wind_gust(f, f.taa.x, pm, phase);
    GcVsOut o;
    o.rel = base + rp + float3(off.x, 0, off.y);
    o.pos = project(o.rel);
    o.prev = project_prev(base + rp + float3(off_prev.x, 0, off_prev.y));
    o.n = rd;
    uint frame = (info & 255u) * 2u + j, cols = GC_STRIPE_ATLAS_W / GC_STRIPE_FRAME;
    float2 tile = float2(frame % cols, frame / cols);
    o.uv = (tile * GC_STRIPE_FRAME + 0.5 + float2(u, v) * (GC_STRIPE_FRAME - 1.0)) / float2(GC_STRIPE_ATLAS_W, GC_STRIPE_ATLAS_H);
    o.t = saturate(lp.y / max(hgt, 1e-3));
    o.info = pv.species | GC_LOD_STRIPE << 8 | ((info >> 24) & 255u) << 16 | ((info >> 16) & 255u) << 24;
    return o;
}

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(32, 1, 1)]
void ms_gc(uint tid : SV_GroupThreadID, uint gid : SV_GroupID, in payload GcPayload p,
           out indices uint3 tris[TREE_MESHLET_TRIS], out vertices GcVsOut verts[TREE_MESHLET_VERTS])
{
    FrameConstants f = *pc.frame;
    GcScene ps = *pc.gc;
    uint mesh_groups = p.prefix[GC_CELL_SLOTS];
    if (gid >= mesh_groups) {
        // Stripes: GC_STRIPES_PER_GROUP instances, each 2 strips x 5 x 3 vertices, 2 x 4 x 2 x 2 triangles.
        uint first = (gid - mesh_groups) * GC_STRIPES_PER_GROUP;
        uint n = min(GC_STRIPES_PER_GROUP, p.stripe_count - first);
        SetMeshOutputCounts(n * 30, n * 32);
        for (uint vi = tid; vi < n * 30; vi += 32) {
            uint s = vi / 30, k = vi % 30, j = k / 15, rc = k % 15;
            uint slot = p.stripe_slot[first + s];
            uint info = p.info[slot];
            GcVariant pv = ps.variants[info & 255u];
            verts[vi] = stripe_vertex(f, pv, ps.species[pv.species], p.pos[slot].xyz, p.pos[slot].w, info, j, rc % 5, rc / 5);
        }
        for (uint ti = tid; ti < n * 32; ti += 32) {
            uint s = ti / 32, k = ti % 32, j = k / 16, q = k % 16, row = q / 8, col = (q % 8) / 2;
            uint a = s * 30 + j * 15 + row * 5 + col;
            tris[ti] = (q & 1u) == 0 ? uint3(a, a + 1, a + 5) : uint3(a + 1, a + 6, a + 5);
        }
        return;
    }
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

bool is_stripe(GcVsOut i) { return ((i.info >> 8) & 255u) == GC_LOD_STRIPE; }

// Surface at a fragment: albedo (tinted, season greenness), shading normal, geometric normal, AO. Returns false
// where the alpha test discards. Mesh: species textures, normal map in a cotangent frame from screen derivatives
// (glTF uv has v down, the OpenGL normal map +y up, hence -B). Stripe: the baked side view (colour premultiplied,
// AO and greenness already in it; normal in the instance frame, turned by the yaw; mirrored behind the strip).
bool gc_surface(GcVsOut i, bool front, out float3 albedo, out float3 nm, out float3 n, out float ao)
{
    GcSpecies sp = pc.gc->species[i.info & 255u];
    albedo = float3(0, 0, 0); nm = float3(0, 1, 0); n = nm; ao = 1.0;
    float tint = float((i.info >> 16) & 255u) / 255.0;
    if (is_stripe(i)) {
        SamplerState sl = g_samplers[SAMPLER_LINEAR_CLAMP];
        float4 c = g_textures[TEX_STRIPE].Sample(sl, i.uv);
        if (c.a < 0.5) return false;
        float3 nl = normalize(g_textures[TEX_STRIPE + 1].Sample(sl, i.uv).xyz * 2.0 - 1.0);
        float yaw = float(i.info >> 24) / 256.0 * 6.2831853;
        float cy = cos(yaw), sy = sin(yaw);
        nm = float3(nl.x * cy - nl.z * sy, nl.y, nl.x * sy + nl.z * cy);
        n = normalize(i.n);
        if (!front) { nm -= 2.0 * dot(nm, n) * n; n = -n; }
        albedo = c.rgb / c.a * lerp(float3(0.92, 0.95, 0.97), float3(1.05, 1.04, 0.96), tint);
        ao = lerp(0.6, 1.0, saturate(i.t * 2.5));
        return true;
    }
    SamplerState smp = g_samplers[SAMPLER_ANISO_REPEAT];
    float4 a = g_textures[sp.albedo_tex].Sample(smp, i.uv);              // sRGB view: linear rgb
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
    if (is_stripe(i)) {
        if (g_textures[TEX_STRIPE].Sample(g_samplers[SAMPLER_LINEAR_CLAMP], i.uv).a < 0.5) discard;
        return;
    }
    GcSpecies sp = pc.gc->species[i.info & 255u];
    if ((uint(sp.habitat.w) & 1u) != 0 && g_textures[sp.albedo_tex].Sample(g_samplers[SAMPLER_ANISO_REPEAT], i.uv).a < 0.5) discard;
}

[shader("fragment")]
SceneOut fs_gc(GcVsOut i, bool front : SV_IsFrontFace)
{
    FrameConstants f = *pc.frame;
    float3 albedo, nm, n;
    float ao;
    bool covered = gc_surface(i, front, albedo, nm, n, ao);
    if (f.debug_mode == DEBUG_OVERDRAW) return overdraw_out();
    if (f.debug_mode == DEBUG_ALPHA) return scene_out(f, covered ? float3(1, 1, 1) : float3(0.05, 0.05, 0.05), i.pos, i.prev);
    if (!covered) discard;
    if (f.debug_mode == DEBUG_UV) return scene_out(f, float3(frac(i.uv), 0), i.pos, i.prev);
    if (f.debug_mode == DEBUG_UNLIT) return scene_out(f, albedo * ao, i.pos, i.prev);
    if (f.debug_mode == DEBUG_LOD) albedo = DEBUG_TINT[is_stripe(i) ? 3u : (i.info >> 8) & 255u] * 0.4;
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
    if (f.gc_bake.w < 0.0) {                                            // gallery: one impostor at the origin
        if (f.gallery != 0 && f.gallery_rep == 4u && all(gid == 0) && tid == 0) {
            s_imp.pos[0] = float4(0, 0, 0, 1);
            s_imp.widen[0] = 1.0;
            s_imp.info[0] = f.gc_bake_pass | 128u << 24;
            s_imp.count = 1;
        }
        GroupMemoryBarrierWithGroupSync();
        DispatchMesh(s_imp.count, 1, 1, s_imp);
        return;
    }
    const float ds = f.veg.y, cell = GC_IMP_CELL * ds;                   // the mid field scales with the distance factor
    int cx = f.gc_imp_cell_x + int(gid.x), cz = f.gc_imp_cell_z + int(gid.y);
    float2 cm = (float2(cx, cz) + 0.5) * cell;
    float2 cg = clamp(cm / f.terrain.z, 0.0, f.terrain_size.xy - 1.0);
    float3 cc = float3(f.terrain.x + cm.x, height_at(f, cg, 0) + 1.0, f.terrain.y + cm.y);
    float cd = distance(cc, f.cam_pos.xyz);
    bool cell_visible = gid.x < f.gc_imp_cells && gid.y < f.gc_imp_cells && cd > GC_MID_DISTANCE * 0.5 * ds - cell * 1.5 &&
                        cd < GC_IMP_DISTANCE * ds + cell * 1.5 && sphere_visible(cc, cell * 1.5 + 2.0);
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
            float reach = sp.shape.z * ds;
            float fade_in = smoothstep(reach * 0.7, reach, dist);                  // where this species' meshes fade out
            float fade_out = smoothstep(GC_IMP_DISTANCE * ds * 0.7, GC_IMP_DISTANCE * ds, dist);
            float keep = saturate(30.0 * ds / dist);
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
    uint fi = variant * GC_IMP_FRAMES * GC_IMP_FRAMES + uint(frame.y) * GC_IMP_FRAMES + uint(frame.x), cols = GC_IMP_ATLAS / GC_IMP_FRAME;
    float2 tile = float2(fi % cols, fi / cols);                             // frames in reading order (gc_bake_views)
    float2 auv = (tile * GC_IMP_FRAME + 0.5 + i.uv * (GC_IMP_FRAME - 1.0)) / GC_IMP_ATLAS;
    float2 g = float2(GC_IMP_FRAME - 1.0, GC_IMP_FRAME - 1.0) / GC_IMP_ATLAS;
    SamplerState smp = g_samplers[SAMPLER_LINEAR_CLAMP];
    float4 c = g_textures[TEX_IMPOSTOR].SampleGrad(smp, auv, ddx(i.uv) * g, ddy(i.uv) * g);
    if (f.debug_mode == DEBUG_OVERDRAW) return overdraw_out();
    if (f.debug_mode == DEBUG_ALPHA) return scene_out(f, c.a >= 0.5 ? float3(1, 1, 1) : float3(0.05, 0.05, 0.05), i.pos, i.prev);
    if (c.a < 0.5) discard;
    if (f.debug_mode == DEBUG_UV) return scene_out(f, float3(i.uv, 0), i.pos, i.prev);
    if (f.debug_mode == DEBUG_UNLIT) return scene_out(f, c.rgb / c.a, i.pos, i.prev);
    float4 s = g_textures[TEX_IMPOSTOR + 1].SampleGrad(smp, auv, ddx(i.uv) * g, ddy(i.uv) * g);
    float yaw = float((i.info >> 16) & 255u) / 256.0 * 6.2831853;
    float cy = cos(yaw), sy = sin(yaw);
    float3 nl = normalize(s.xyz * 2.0 - 1.0);
    float3 n = float3(nl.x * cy - nl.z * sy, nl.y, nl.x * sy + nl.z * cy);
    float3 albedo = c.rgb / c.a;
    if (f.debug_mode == DEBUG_LOD) albedo = DEBUG_TINT[4] * 0.4;     // impostors: blue, as in the wireframe
    if (f.debug_mode == DEBUG_NORMALS) return scene_out(f, n * 0.5 + 0.5, i.pos, i.prev);
    float3 to_cam = normalize(f.cam_pos.xyz - i.rel);
    float3 col = shade_foliage(f, albedo, n, to_cam, 1.0, 0.7, sun_shadow(f, i.rel, n, i.pos.xy));
    return scene_out(f, tonemap(apply_fog(f, col, i.rel)), i.pos, i.prev);
}

// Wireframe (N key): every edge, no alpha test; meshes red (LOD 0), stripes green, impostors blue.
[shader("fragment")]
SceneOut fs_gc_wire(GcVsOut i)
{
    return scene_out(*pc.frame, DEBUG_TINT[is_stripe(i) ? 3u : (i.info >> 8) & 255u], i.pos, i.prev);
}

[shader("fragment")]
SceneOut fs_gc_imp_wire(GcImpVsOut i)
{
    return scene_out(*pc.frame, DEBUG_TINT[4], i.pos, i.prev);
}