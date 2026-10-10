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

    for (uint v = tid; v < m.vertex_count; v += 32) {
        TreeVertex tv = s.vertices[m.vertex_offset + v];
        uint mat = tv.attr & 255;
        float w = float((tv.attr >> 16) & 255) / 255.0;
        float3 lp = float3(tv.x, tv.y, tv.z) * scale;
        float3 n = unpack_normal(tv.normal);
        float3 rp = float3(lp.x * cy - lp.z * sy, lp.y, lp.x * sy + lp.z * cy);
        float3 rn = float3(n.x * cy - n.z * sy, n.y, n.x * sy + n.z * cy);
        rp.xz += f.wind.xz * sway * w * 0.25 * scale;
        TreeVsOut o;
        o.rel = base + rp;
        o.pos = project(o.rel);
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
float4 fs_trees(TreeVsOut i, bool front : SV_IsFrontFace) : SV_Target
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
    if (f.debug_mode == DEBUG_NORMALS) return float4(n * 0.5 + 0.5, 1);
    return float4(tonemap(apply_fog(f, c, i.rel)), 1);
}

// ---------------------------------------------------------------------------------------------------------------
// Grass: tiles of GRASS_TILE m on an absolute grid around the camera; one task workgroup = 8 x 4 tiles.


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
            if (dist < f.veg.y + 3.0 && sphere_visible(c + float3(0, 0.5, 0), 4.0)) {
                float density = mask_at(f, g, 0).y * f.veg.w;
                float lod = min(1.0, pow(12.0 / max(dist, 12.0), 1.6));
                uint blades = uint(float(GRASS_TILE_BLADES) * density * lod + 0.5);
                groups = (blades + GRASS_GROUP_BLADES - 1) / GRASS_GROUP_BLADES;
                s_payload.a[tid] = rx | rz << 16;
                s_payload.b[tid] = blades;
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

struct GrassVsOut {
    float4 pos : SV_Position;
    float3 rel : REL;
    float3 n : NRM;
    float3 color : COLOR;
    float t : HEIGHT;               // 0 root .. 1 tip
};

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(32, 1, 1)]
void ms_grass(uint tid : SV_GroupThreadID, uint gid : SV_GroupID, in payload VegPayload p,
              out indices uint3 tris[GRASS_GROUP_BLADES * 3], out vertices GrassVsOut verts[GRASS_GROUP_BLADES * 5])
{
    FrameConstants f = *pc.frame;
    uint slot = find_slot(p.prefix, gid);
    uint group = gid - p.prefix[slot];
    uint count = min(GRASS_GROUP_BLADES, p.b[slot] - group * GRASS_GROUP_BLADES);
    SetMeshOutputCounts(count * 5, count * 3);
    if (tid >= count) return;

    int tx = f.grass_tile_x + int(p.a[slot] & 0xffff), tz = f.grass_tile_z + int(p.a[slot] >> 16);
    uint b = group * GRASS_GROUP_BLADES + tid;
    uint h = hash_u32(uint(tx) * 73856093u ^ uint(tz) * 19349663u ^ b * 83492791u);
    float h0 = hash_f(h), h1 = hash_f(h + 1), h2 = hash_f(h + 2), h3 = hash_f(h + 3), h4 = hash_f(h + 4);
    float2 pm = (float2(tx, tz) + float2(h0, h1)) * GRASS_TILE;          // terrain-local metres
    float2 g = pm / f.terrain.z;
    float3 root = float3(f.terrain.x + pm.x, height_at(f, g, 0), f.terrain.y + pm.y);
    float density = mask_at(f, g, 0).y;
    float dist = distance(root, f.cam_pos.xyz);

    float height = (0.25 + 0.55 * h2 * h2) * lerp(0.4, 1.0, density) * smoothstep(f.veg.y, f.veg.y * 0.8, dist);
    if (h4 > density * 1.6) height = 0.0;                            // thin out towards sparse areas
    float width = 0.012 * asfloat(p.c[slot]) * (0.7 + 0.6 * h3);
    float yaw = h3 * 6.2831853;
    float3 across = float3(cos(yaw), 0, sin(yaw));
    float3 facing = float3(-across.z, 0, across.x);
    float gust = 0.35 + 0.3 * sin(f.cam_fwd.w * 1.7 + dot(pm, f.wind.xz) * 0.35 + h4 * 1.5);
    float3 lean = (facing * (h4 - 0.5) * 0.6 + float3(f.wind.x, 0, f.wind.z) * gust * f.wind.y) * height;

    // Quadratic Bézier: root, control straight above, tip displaced by lean (Tsushima-style blade).
    float3 p0 = root, p1 = root + float3(0, height, 0), p2 = root + float3(0, height * 0.85, 0) + lean;
    float3 mid = 0.25 * p0 + 0.5 * p1 + 0.25 * p2;
    float3 tangent_mid = normalize(p2 - p0 + float3(0, 1e-4, 0));
    float3 n = normalize(cross(across, tangent_mid));

    float3 base_col = lerp(float3(0.10, 0.20, 0.045), float3(0.20, 0.23, 0.07), h2) * lerp(0.85, 1.15, h0);
    float3 pos[5] = { p0 - across * width, p0 + across * width, mid - across * width * 0.7, mid + across * width * 0.7, p2 };
    float side[5] = { -1, 1, -1, 1, 0 };
    float t[5] = { 0, 0, 0.5, 0.5, 1 };
    for (uint k = 0; k < 5; k++) {
        GrassVsOut o;
        o.rel = pos[k];
        o.pos = project(pos[k]);
        o.n = normalize(n + across * side[k] * 0.5);                  // rounded blade normal
        o.color = base_col;
        o.t = t[k];
        verts[tid * 5 + k] = o;
    }
    uint v = tid * 5;
    tris[tid * 3 + 0] = uint3(v + 0, v + 1, v + 2);
    tris[tid * 3 + 1] = uint3(v + 1, v + 3, v + 2);
    tris[tid * 3 + 2] = uint3(v + 2, v + 3, v + 4);
}

[shader("fragment")]
float4 fs_grass(GrassVsOut i, bool front : SV_IsFrontFace) : SV_Target
{
    FrameConstants f = *pc.frame;
    float3 n = normalize(i.n);
    if (!front) n = -n;
    float3 to_cam = normalize(f.cam_pos.xyz - i.rel);
    float ao = lerp(0.35, 1.0, i.t);
    float3 albedo = i.color * lerp(0.7, 1.1, i.t);
    if (f.debug_mode == DEBUG_LOD) albedo = float3(0.4, 0.4, 0.1);
    if (f.debug_mode == DEBUG_NORMALS) return float4(n * 0.5 + 0.5, 1);
    float3 c = shade_foliage(f, albedo, n, to_cam, ao, 0.8);
    return float4(tonemap(apply_fog(f, c, i.rel)), 1);
}
