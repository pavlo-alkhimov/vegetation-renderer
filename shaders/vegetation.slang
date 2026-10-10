// Vegetation baseline: trees as meshlets (task shader: instance culling + LOD, mesh shader: meshlet output) and grass
// blades generated per frame (task shader: tile culling + density, mesh shader: blades). Forward shaded.
// Baseline for docs/06; each part is meant to be replaced step by step (see docs/06, "Baseline and steps").
#include "common.slang"

// ---------------------------------------------------------------------------------------------------------------
// Trees

// One payload layout for both task shaders (each task shader is built into its own SPIR-V module, see build.bat).
//   trees: a = instance index, b = type | lod << 8, chunk = chunk index
//   grass: a = tile (x | z << 16), b = blade count, c = blade width multiplier (asuint)
struct VegPayload {
    uint chunk;
    uint a[TREE_CHUNK];
    uint b[TREE_CHUNK];
    uint c[TREE_CHUNK];
    uint count[TREE_CHUNK];         // mesh workgroups per slot (task-internal)
    uint prefix[TREE_CHUNK + 1];    // mesh workgroup prefix sum
};
groupshared VegPayload s_payload;

float instance_scale(uint packed) { return 0.6 + float((packed >> 16) & 255) / 255.0 * 0.8; }
float instance_yaw(uint packed)   { return float((packed >> 8) & 255) / 256.0 * 6.2831853; }

[shader("amplification")]
[numthreads(TREE_CHUNK, 1, 1)]
void as_trees(uint tid : SV_GroupThreadID, uint gid : SV_GroupID)
{
    FrameConstants f = *pc.frame;
    VegScene s = *pc.veg;
    TreeChunk c = pc.chunks[gid];
    uint meshlets = 0;
    if (tid < c.count) {
        uint ii = c.first + tid;
        TreeInstance ins = s.instances[ii];
        uint type = ins.packed & 255;
        TreeType tt = s.types[type];
        float scale = instance_scale(ins.packed);
        float3 base = float3(c.ox + ins.x, f.terrain.w + ins.y, c.oz + ins.z);
        float3 centre = base + tt.bounds.xyz * scale;
        float r = tt.bounds.w * scale;
        float dist = distance(centre, f.cam_pos.xyz);
        if (dist - r < f.veg.x && sphere_visible(centre, r)) {
            float px = tt.bark_color.w * scale * f.veg.z / max(dist, 1.0);
            uint lod = px > f.tree_lod.x ? 0 : px > f.tree_lod.y ? 1 : px > f.tree_lod.z ? 2 : 3;
            meshlets = tt.meshlet_count[lod];
            s_payload.a[tid] = ii;
            s_payload.b[tid] = type | lod << 8;
        }
    }
    s_payload.count[tid] = meshlets;
    GroupMemoryBarrierWithGroupSync();
    if (tid == 0) {
        uint sum = 0;
        for (uint i = 0; i < TREE_CHUNK; i++) { s_payload.prefix[i] = sum; sum += s_payload.count[i]; }
        s_payload.prefix[TREE_CHUNK] = sum;
        s_payload.chunk = gid;
    }
    GroupMemoryBarrierWithGroupSync();
    DispatchMesh(s_payload.prefix[TREE_CHUNK], 1, 1, s_payload);
}

struct TreeVsOut {
    float4 pos : SV_Position;
    float3 rel : REL;
    float4 prev : PREV;                 // previous frame's clip position
    float3 n : NRM;
    float2 uv : UV;
    float3 color : COLOR;           // albedo (leaf or bark), tinted per instance
    float ao : AO;
    nointerpolation uint info : INFO;   // material | lod << 8 | card seed << 16
    nointerpolation float4 shape : SHAPE;  // TreeType.shape (billboard silhouette)
};

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(32, 1, 1)]
void ms_trees(uint tid : SV_GroupThreadID, uint gid : SV_GroupID, in payload VegPayload p,
              out indices uint3 tris[TREE_MESHLET_TRIS], out vertices TreeVsOut verts[TREE_MESHLET_VERTS])
{
    FrameConstants f = *pc.frame;
    VegScene s = *pc.veg;
    uint slot = find_slot(p.prefix, gid);
    uint type = p.b[slot] & 255, lod = p.b[slot] >> 8;
    TreeType tt = s.types[type];
    TreeInstance ins = s.instances[p.a[slot]];
    TreeChunk c = pc.chunks[p.chunk];
    TreeMeshlet m = s.meshlets[tt.meshlet_first[lod] + gid - p.prefix[slot]];
    SetMeshOutputCounts(m.vertex_count, m.triangle_count);

    float scale = instance_scale(ins.packed), yaw = instance_yaw(ins.packed);
    float cy = cos(yaw), sy = sin(yaw);
    float3 base = float3(c.ox + ins.x, f.terrain.w + ins.y, c.oz + ins.z);
    float tint = float(ins.packed >> 24) / 255.0;
    float3 leaf = tt.leaf_color.rgb * lerp(float3(0.8, 0.85, 0.9), float3(1.2, 1.15, 0.95), tint);
    // Stateless sway: whole tree bends along the wind, phase from position (docs/06 §5.6 has the real rig).
    float phase = dot(base.xz, f.wind.xz) * 0.05 + tint * 6.0;
    float sway = f.wind.y * (0.6 * sin(f.cam_fwd.w * 0.8 + phase) + 0.4 * sin(f.cam_fwd.w * 1.9 + phase * 1.7));
    float sway_prev = f.wind.y * (0.6 * sin(f.taa.x * 0.8 + phase) + 0.4 * sin(f.taa.x * 1.9 + phase * 1.7));

    for (uint v = tid; v < m.vertex_count; v += 32) {
        TreeVertex tv = s.vertices[m.vertex_offset + v];
        uint mat = tv.attr & 255;
        float w = float((tv.attr >> 16) & 255) / 255.0;
        float3 lp = float3(tv.x, tv.y, tv.z) * scale;
        float3 n = unpack_normal(tv.normal);
        float3 rp = float3(lp.x * cy - lp.z * sy, lp.y, lp.x * sy + lp.z * cy);
        float3 rn = float3(n.x * cy - n.z * sy, n.y, n.x * sy + n.z * cy);
        float3 still = base + rp;
        TreeVsOut o;
        o.rel = still + float3(f.wind.x, 0, f.wind.z) * sway * w * 0.25 * scale;
        o.pos = project(o.rel);
        o.prev = project_prev(still + float3(f.wind.x, 0, f.wind.z) * sway_prev * w * 0.25 * scale);
        o.n = rn;
        o.uv = float2(tv.uv & 0xffff, tv.uv >> 16) / 1024.0;
        o.color = mat == MAT_BARK ? tt.bark_color.rgb : leaf;
        o.ao = float((tv.attr >> 8) & 255) / 255.0;
        o.info = mat | lod << 8 | (tv.attr >> 24) << 16;
        o.shape = tt.shape;
        verts[v] = o;
    }
    for (uint t = tid; t < m.triangle_count; t += 32) {
        uint x = s.triangles[m.triangle_offset + t];
        tris[t] = uint3(x & 255, (x >> 8) & 255, (x >> 16) & 255);
    }
}

// Alpha of a broadleaf card: five leaves around the centre plus one in the middle, rotated per card.
float leaf_alpha(float2 uv, uint seed)
{
    float a0 = float(seed) * 0.0245;
    float2 q = uv - 0.5;
    float best = 1e9;
    for (uint k = 0; k < 6; k++) {
        float a = a0 + float(k) * 1.2566;
        float2 dir = float2(cos(a), sin(a));
        float2 c = k == 5 ? float2(0, 0) : dir * 0.27;
        float2 d = q - c;
        float2 l = float2(dot(d, dir), dot(d, float2(-dir.y, dir.x)));   // leaf axis points outwards
        best = min(best, dot(l / float2(0.2, 0.1), l / float2(0.2, 0.1)));
    }
    return 1.0 - best;   // > 0 inside a leaf
}

// Alpha of a conifer spray card: twig along u, needles as a fishbone pattern, tapering towards the tip. Needles
// fade to coverage when they get smaller than a pixel.
float needle_alpha(float2 uv)
{
    float across = abs(uv.y - 0.5) * 2.0;
    float width = sin(3.14159 * min(uv.x * 1.15, 1.0));
    if (across > width) return -1.0;
    float stripes = frac(uv.x * 28.0 + across * 5.0);
    float fw = fwidth(uv.x * 28.0);
    float needle = lerp(stripes < 0.55 ? 1.0 : 0.0, 0.8, saturate(fw * 1.5));
    return max(needle, across < 0.08 ? 1.0 : 0.0) - 0.5;
}

float cluster_alpha(float2 uv, uint seed)
{
    float2 q = uv - 0.5;
    float r2 = dot(q, q) * 4.0;
    float n = value_noise(uv * 7.0 + float(seed) * 3.17) * 0.65 + value_noise(uv * 19.0 + float(seed)) * 0.35;
    return n + 0.35 - r2 * 0.75 - 0.5;
}

// Whole-tree silhouette on a billboard quad: uv.x across the crown width, uv.y up the height.
float billboard_alpha(float2 uv, float4 shape, uint seed)
{
    float x = abs(uv.x - 0.5) * 2.0;    // 0 centre .. 1 crown radius
    float cb = shape.x;
    float crown;
    if (shape.y > 0.5) {
        float t = saturate((uv.y - cb) / (1.0 - cb));
        crown = uv.y < cb ? -1.0 : (1.0 - t) * 1.05 - x;                       // cone
    } else {
        float t = (uv.y - cb) / (1.0 - cb) * 2.0 - 1.0;
        crown = 1.0 - sqrt(x * x + t * t);                                       // ellipse
    }
    float n = value_noise(uv * float2(10.0, 14.0) + float(seed)) - 0.5;
    float trunk = shape.w - x;              // trunk half-width in units of the crown radius
    return max(crown + n * 0.3, uv.y < cb + 0.05 ? trunk : -1.0);
}

[shader("fragment")]
SceneOut fs_trees(TreeVsOut i, bool front : SV_IsFrontFace)
{
    FrameConstants f = *pc.frame;
    uint mat = i.info & 255, lod = (i.info >> 8) & 255, seed = i.info >> 16;
    float alpha = 1.0;
    if (mat == MAT_LEAF) alpha = leaf_alpha(i.uv, seed);
    else if (mat == MAT_NEEDLE) alpha = needle_alpha(i.uv);
    else if (mat == MAT_CLUSTER) alpha = cluster_alpha(i.uv, seed);
    else if (mat == MAT_BILLBOARD) alpha = billboard_alpha(i.uv, i.shape, seed);
    if (alpha < 0.0) discard;

    float3 to_cam = normalize(f.cam_pos.xyz - i.rel);
    float3 n = normalize(i.n);
    float3 albedo = i.color;
    float c_ao = i.ao;
    if (mat == MAT_BARK) {
        float grain = value_noise(i.uv * float2(6.0, 40.0)) * 0.5 + value_noise(i.uv * float2(20.0, 120.0)) * 0.5;
        albedo *= lerp(0.6, 1.15, grain);
    } else {
        if (!front) n = -n;                         // two-sided cards
        if (mat == MAT_BILLBOARD) {
            // Rounded fake normal across the quad, upward towards the crown top.
            float3 t = normalize(cross(float3(0, 1, 0), n));
            n = normalize(n * 0.6 + t * (i.uv.x - 0.5) * 1.6 + float3(0, 1, 0) * (i.uv.y - 0.6));
            c_ao = lerp(0.45, 1.0, i.uv.y);
            albedo *= 0.85;
        }
        albedo *= lerp(0.85, 1.15, hash_f(seed * 7919u + 13u));
    }
    if (f.debug_mode == DEBUG_LOD) albedo = DEBUG_TINT[lod] * 0.4;
    float3 c = mat == MAT_BARK ? albedo * (SUN_COLOR * saturate(dot(n, f.sun_dir.xyz)) * lerp(0.3, 1.0, c_ao) +
                                           SKY_AMBIENT * (0.6 + 0.4 * n.y) * c_ao)
                               : shade_foliage(f, albedo, n, to_cam, c_ao, 0.6);
    if (f.debug_mode == DEBUG_NORMALS) return scene_out(f, n * 0.5 + 0.5, i.pos, i.prev);
    return scene_out(f, tonemap(apply_fog(f, c, i.rel)), i.pos, i.prev);
}

// ---------------------------------------------------------------------------------------------------------------
// Grass blades (Tsushima-style): tiles of GRASS_TILE m on an absolute grid around the camera, one task workgroup =
// 8 x 4 tiles. Blades group into clumps (shared height, lean, colour), take their height from the meadow height
// field and their colour from the plant assets; density falls with distance (wider blades), the terrain takes over
// beyond the blade radius. Inside the plant radius the assets carry the volume and the blades thin out.

#define GRASS_CLUMP     0.8         // m, clump cell
#define GRASS_MAX_SEGS  4

[shader("amplification")]
[numthreads(TREE_CHUNK, 1, 1)]
void as_grass(uint tid : SV_GroupThreadID, uint3 gid : SV_GroupID)
{
    FrameConstants f = *pc.frame;
    uint rx = gid.x * 8 + (tid & 7), rz = gid.y * 4 + (tid >> 3);
    uint groups = 0;
    if (rx < f.grass_tiles && rz < f.grass_tiles) {
        int tx = f.grass_tile_x + int(rx), tz = f.grass_tile_z + int(rz);
        float2 centre_m = (float2(tx, tz) + 0.5) * GRASS_TILE;        // terrain-local metres
        float2 g = centre_m / f.terrain.z;
        float2 extent = (f.terrain_size.xy - 1.0);
        if (all(g >= 0.0) && all(g <= extent)) {
            float3 c = float3(f.terrain.x + centre_m.x, height_at(f, g, 0), f.terrain.y + centre_m.y);
            float dist = distance(c, f.cam_pos.xyz);
            if (dist < f.veg.y + 3.0 && sphere_visible(c + float3(0, 0.4, 0), 3.2)) {
                float density = mask_at(f, g, 0).y * f.veg.w;
                density *= lerp(f.grass.w, 1.0, smoothstep(PLANT_NEAR_DISTANCE - 6.0, PLANT_NEAR_DISTANCE, dist));
                float lod = min(1.0, pow(12.0 / max(dist, 12.0), 1.6));
                uint blades = uint(float(GRASS_TILE_BLADES) * density * lod + 0.5);
                uint segs = dist < 8.0 ? 4u : dist < 25.0 ? 3u : dist < 60.0 ? 2u : 1u;
                groups = (blades + GRASS_GROUP_BLADES - 1) / GRASS_GROUP_BLADES;
                s_payload.a[tid] = rx | rz << 16;
                s_payload.b[tid] = blades | segs << 24;
                s_payload.c[tid] = asuint(min(4.0, sqrt(1.0 / max(lod, 1e-3))));  // wider blades at lower density
            }
        }
    }
    s_payload.count[tid] = groups;
    GroupMemoryBarrierWithGroupSync();
    if (tid == 0) {
        uint sum = 0;
        for (uint i = 0; i < TREE_CHUNK; i++) { s_payload.prefix[i] = sum; sum += s_payload.count[i]; }
        s_payload.prefix[TREE_CHUNK] = sum;
    }
    GroupMemoryBarrierWithGroupSync();
    DispatchMesh(s_payload.prefix[TREE_CHUNK], 1, 1, s_payload);
}

// Nearest clump centre on a jittered GRASS_CLUMP grid; id = the clump's hash.
float2 grass_clump(float2 pm, out uint id)
{
    float2 cell = floor(pm / GRASS_CLUMP);
    float best = 1e9;
    float2 centre = pm;
    id = 0;
    for (int j = -1; j <= 1; j++)
        for (int i = -1; i <= 1; i++) {
            float2 c = cell + float2(i, j);
            uint h = hash_u32(uint(int(c.x)) * 73856093u ^ uint(int(c.y)) * 19349663u ^ 0xc1u);
            float2 p = (c + float2(hash_f(h), hash_f(h + 1u))) * GRASS_CLUMP;
            float d = dot(p - pm, p - pm);
            if (d < best) { best = d; centre = p; id = h; }
        }
    return centre;
}

struct GrassVsOut {
    float4 pos : SV_Position;
    float3 rel : REL;
    float4 prev : PREV;             // previous frame's clip position
    float3 n : NRM;
    float3 color : COLOR;
    float t : HEIGHT;               // 0 root .. 1 tip
};

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(32, 1, 1)]
void ms_grass(uint tid : SV_GroupThreadID, uint gid : SV_GroupID, in payload VegPayload p,
              out indices uint3 tris[GRASS_GROUP_BLADES * (2 * GRASS_MAX_SEGS - 1)],
              out vertices GrassVsOut verts[GRASS_GROUP_BLADES * (2 * GRASS_MAX_SEGS + 1)])
{
    FrameConstants f = *pc.frame;
    uint slot = find_slot(p.prefix, gid);
    uint group = gid - p.prefix[slot];
    uint total = p.b[slot] & 0xffffffu, segs = p.b[slot] >> 24;
    uint nv = 2 * segs + 1, nt = 2 * segs - 1;
    uint count = min(GRASS_GROUP_BLADES, total - group * GRASS_GROUP_BLADES);
    SetMeshOutputCounts(count * nv, count * nt);
    if (tid >= count) return;

    int tx = f.grass_tile_x + int(p.a[slot] & 0xffff), tz = f.grass_tile_z + int(p.a[slot] >> 16);
    uint b = group * GRASS_GROUP_BLADES + tid;
    uint h = hash_u32(uint(tx) * 73856093u ^ uint(tz) * 19349663u ^ b * 83492791u);
    float h0 = hash_f(h), h1 = hash_f(h + 1), h2 = hash_f(h + 2), h3 = hash_f(h + 3), h4 = hash_f(h + 4), h5 = hash_f(h + 5);
    float2 pm = (float2(tx, tz) + float2(h0, h1)) * GRASS_TILE;          // terrain-local metres
    uint cid;
    float2 cc = grass_clump(pm, cid);
    pm = lerp(pm, cc, 0.35 * h5);                                       // blades gather towards the clump centre
    float2 g = pm / f.terrain.z;
    float3 root = float3(f.terrain.x + pm.x, height_at(f, g, 0) - 0.02, f.terrain.y + pm.y);
    float density = mask_at(f, g, 0).y;
    float dist = distance(root, f.cam_pos.xyz);

    float clump_h = lerp(0.7, 1.25, hash_f(cid + 2u));
    float height = meadow_height(pm) * lerp(0.55, 1.15, h2) * clump_h * lerp(0.5, 1.0, density);
    height *= smoothstep(f.veg.y, f.veg.y * 0.8, dist);
    if (h4 > density * 1.6) height = 0.0;                            // thin out towards sparse areas
    float width = (0.0035 + 0.003 * h3) * asfloat(p.c[slot]);

    // Facing: random, biased to point away from the clump centre; widen blades seen edge-on (they would vanish).
    float2 out_dir = pm - cc;
    float out_len = length(out_dir);
    out_dir = out_len > 1e-4 ? out_dir / out_len : float2(1, 0);
    float yaw = h3 * 6.2831853;
    float3 across = float3(cos(yaw), 0, sin(yaw));
    float3 facing = float3(-across.z, 0, across.x);
    float2 view = normalize(root.xz - f.cam_pos.xz + 1e-4);
    width *= 1.0 + 1.0 * (1.0 - abs(dot(facing.xz, view))) * saturate(dist / 10.0);
    width = max(width, 0.6 * dist / f.veg.z);                           // at least ~1.2 px wide (no AA yet)
    float gust = wind_gust(f, f.cam_fwd.w, pm, hash_f(cid + 4u)), gust_prev = wind_gust(f, f.taa.x, pm, hash_f(cid + 4u));
    float3 lean_still = (float3(out_dir.x, 0, out_dir.y) * (0.25 + 0.35 * hash_f(cid + 5u)) + facing * (h4 - 0.5) * 0.4) * height;
    float3 wind_dir = float3(f.wind.x, 0, f.wind.z) * f.wind.y * height;

    // Quadratic Bézier: root, control straight above, tip displaced by lean.
    float3 p0 = root, p1 = root + float3(0, height, 0), p2 = root + float3(0, height * 0.85, 0) + lean_still + wind_dir * gust;
    float3 p2_prev = root + float3(0, height * 0.85, 0) + lean_still + wind_dir * gust_prev;
    float3 color = grass_tint(f, pm) * lerp(0.8, 1.15, h0) * lerp(float3(0.9, 0.9, 0.9), float3(1.1, 1.08, 0.95), hash_f(cid + 3u));
    uint v0 = tid * nv;
    for (uint k = 0; k <= segs; k++) {
        float t = float(k) / float(segs);
        float3 pos = (1 - t) * (1 - t) * p0 + 2 * (1 - t) * t * p1 + t * t * p2;
        float3 pos_prev = (1 - t) * (1 - t) * p0 + 2 * (1 - t) * t * p1 + t * t * p2_prev;
        float3 tangent = normalize(2 * (1 - t) * (p1 - p0) + 2 * t * (p2 - p1) + float3(0, 1e-4, 0));
        float3 n = normalize(cross(across, tangent));
        float w = width * (1.0 - 0.85 * t * t);
        for (uint s = 0; s < (k < segs ? 2u : 1u); s++) {
            float side = k < segs ? (s == 0 ? -1.0 : 1.0) : 0.0;
            GrassVsOut o;
            o.rel = pos + across * w * side;
            o.pos = project(o.rel);
            o.prev = project_prev(pos_prev + across * w * side);
            o.n = normalize(n + across * side * 0.6);                    // rounded blade normal
            o.color = color;
            o.t = t;
            verts[v0 + 2 * k + s] = o;
        }
    }
    uint t0 = tid * nt;
    for (uint k = 0; k + 1 < segs; k++) {
        uint a = v0 + 2 * k;
        tris[t0 + 2 * k] = uint3(a, a + 1, a + 2);
        tris[t0 + 2 * k + 1] = uint3(a + 1, a + 3, a + 2);
    }
    uint a = v0 + 2 * (segs - 1);
    tris[t0 + nt - 1] = uint3(a, a + 1, a + 2);
}

[shader("fragment")]
SceneOut fs_grass(GrassVsOut i, bool front : SV_IsFrontFace)
{
    FrameConstants f = *pc.frame;
    float3 n = normalize(i.n);
    if (!front) n = -n;
    float3 to_cam = normalize(f.cam_pos.xyz - i.rel);
    float ao = lerp(0.3, 1.0, i.t);
    float3 albedo = i.color * lerp(0.5, 1.05, i.t);
    if (f.debug_mode == DEBUG_LOD) albedo = float3(0.4, 0.4, 0.1);
    if (f.debug_mode == DEBUG_NORMALS) return scene_out(f, n * 0.5 + 0.5, i.pos, i.prev);
    float3 c = shade_foliage(f, albedo, n, to_cam, ao, 0.8);
    return scene_out(f, tonemap(apply_fog(f, c, i.rel)), i.pos, i.prev);
}