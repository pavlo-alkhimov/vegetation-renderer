// Vegetation baseline (docs/06, "Baseline and steps"): the simplest complete trees + grass path, built to be
// measured and then replaced piece by piece.
//
// - Vegetation mask: RG8 texture over the terrain (r = forest density, g = grass density), procedural noise in
//   absolute map coordinates. One source for terrain colour, tree placement and grass (later: real land cover).
// - Trees: 5 species x 2 variants generated procedurally at load (trunk/branch tubes + leaf/needle cards), 4 LODs
//   (full, main branches, crown clusters, crossed billboards), split into meshlets. Instances on a jittered grid,
//   stored per 256 m cell. CPU culls cells and emits chunks of 32 instances; the task shader culls instances and
//   picks the LOD by projected height; the mesh shader outputs the LOD's meshlets.
// - Grass: nothing stored. Task shader culls 4 m tiles around the camera and sets the blade count from the mask
//   and distance; the mesh shader builds curved blades from a hash of tile and blade index.

#define TREE_SPECIES   5
#define TREE_VARIANTS  2
#define TREE_TYPES     (TREE_SPECIES * TREE_VARIANTS)
#define TREE_SPACING   5.5                  // m, jittered grid; ~330 trees/ha in full forest
#define MASK_TEXEL     4.0                  // m, approximate (exactly extent / texels)
#define MAX_TREE_CHUNKS 65535u              // minimum guaranteed maxTaskWorkGroupCount[0]
#define CHUNKS_OFFSET  (UPLOAD_NODES_OFFSET + TERRAIN_MAX_NODES * sizeof(TerrainNode))   // in the per-frame upload buffer

static_assert(CHUNKS_OFFSET + MAX_TREE_CHUNKS * sizeof(TreeChunk) <= UPLOAD_BYTES, "upload buffer too small");
static_assert(sizeof(TreeVertex) == 24 && sizeof(TreeType) == 96 && sizeof(TreeInstance) == 16, "GPU struct size");

typedef struct {
    u32 first, count;           // instances
    f32 ymin, ymax;             // above height_min, including tree heights
} TreeCell;

typedef struct {
    // CPU
    u8* mask;                   // RG8, row 0 = south
    u32 mask_w, mask_h;
    f32 extent_x, extent_z;     // m
    TreeInstance* instances;
    u32 instance_count;
    TreeCell* cells;
    u32 cells_x, cells_z;
    TreeType types[TREE_TYPES];
    u32 lod_tris[TREE_TYPES][TREE_LODS];
    // GPU
    bool enabled;               // task/mesh shaders available
    VkTex mask_tex;
    VkBuf scene, instance_buf, type_buf, meshlet_buf, vertex_buf, triangle_buf;
    VkPipeline pipe_trees, pipe_grass;
    // Per frame
    u32 chunk_count, tree_candidates;
    u32 grass_groups_x, grass_groups_z;
} Vegetation;

// ---------------------------------------------------------------------------------------------------------------
// Hashing and noise (CPU). Deterministic in absolute coordinates, so the result does not depend on the map crop.

static u32 hash_u32(u32 x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

static u32 hash3(i64 a, i64 b, u32 c)
{
    return hash_u32((u32)a * 73856093u ^ (u32)b * 19349663u ^ hash_u32(c + 0x9e3779b9u));
}

static f32 hash_unit(u32 h) { return (h >> 8) * (1.0f / 16777216.0f); }

static f32 smoothstep(f32 a, f32 b, f32 x)
{
    f32 t = CLAMP((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static f32 value_noise(f64 x, f64 z, u32 seed)
{
    f64 fx = floor(x), fz = floor(z);
    i64 ix = (i64)fx, iz = (i64)fz;
    f32 ux = (f32)(x - fx), uz = (f32)(z - fz);
    ux = ux * ux * (3 - 2 * ux);
    uz = uz * uz * (3 - 2 * uz);
    f32 a = hash_unit(hash3(ix, iz, seed)), b = hash_unit(hash3(ix + 1, iz, seed));
    f32 c = hash_unit(hash3(ix, iz + 1, seed)), d = hash_unit(hash3(ix + 1, iz + 1, seed));
    return (a + (b - a) * ux) * (1 - uz) + (c + (d - c) * ux) * uz;
}

static f32 fbm(f64 x, f64 z, u32 seed, u32 octaves)
{
    f32 sum = 0, amp = 0.5f, norm = 0;
    for (u32 o = 0; o < octaves; o++) {
        sum += value_noise(x, z, seed + o * 101) * amp;
        norm += amp;
        x = x * 2.03 + 17.1;
        z = z * 2.03 - 9.7;
        amp *= 0.5f;
    }
    return sum / norm;
}

// ---------------------------------------------------------------------------------------------------------------
// Vegetation mask

static void veg_mask_rows(Vegetation* vg, const Terrain* t, u32 row0, u32 row1)
{
    for (u32 j = row0; j < row1; j++)
        for (u32 i = 0; i < vg->mask_w; i++) {
            f64 x = (i + 0.5) * vg->extent_x / vg->mask_w, z = (j + 0.5) * vg->extent_z / vg->mask_h;
            f64 e = t->hdr.origin_e + x, n = t->hdr.origin_n + z;
            f32 forest = smoothstep(0.47f, 0.54f, fbm(e / 1100.0, n / 1100.0, 11, 6));
            forest *= smoothstep(0.26f, 0.33f, fbm(e / 160.0, n / 160.0, 23, 3));      // small clearings
            f32 meadow = 0.55f + 0.45f * fbm(e / 60.0, n / 60.0, 31, 3);
            f32 grass = (1.0f - forest) * meadow + forest * 0.06f;
            u8* o = vg->mask + 2 * ((size_t)j * vg->mask_w + i);
            o[0] = (u8)lrintf(forest * 255.0f);
            o[1] = (u8)lrintf(CLAMP(grass, 0.0f, 1.0f) * 255.0f);
        }
}

static void veg_build_mask(Vegetation* vg, const Terrain* t)
{
    vg->extent_x = (t->hdr.width - 1) * t->hdr.spacing;
    vg->extent_z = (t->hdr.height - 1) * t->hdr.spacing;
    vg->mask_w = MAX(2u, (u32)ceil(vg->extent_x / MASK_TEXEL));
    vg->mask_h = MAX(2u, (u32)ceil(vg->extent_z / MASK_TEXEL));
    vg->mask = (u8*)malloc((size_t)vg->mask_w * vg->mask_h * 2);
    u32 nthreads = MAX(1u, MIN(32u, std::thread::hardware_concurrency()));
    std::thread threads[32];
    for (u32 k = 0; k < nthreads; k++)
        threads[k] = std::thread(veg_mask_rows, vg, t, vg->mask_h * k / nthreads, vg->mask_h * (k + 1) / nthreads);
    for (u32 k = 0; k < nthreads; k++) threads[k].join();
}

// Bilinear mask channel (0 forest, 1 grass) at terrain-local metres; same texel mapping as mask_at() in the shaders.
static f32 veg_mask_sample(const Vegetation* vg, f64 x, f64 z, u32 channel)
{
    f64 fx = CLAMP(x / vg->extent_x * vg->mask_w - 0.5, 0.0, vg->mask_w - 1.001);
    f64 fz = CLAMP(z / vg->extent_z * vg->mask_h - 0.5, 0.0, vg->mask_h - 1.001);
    u32 x0 = (u32)fx, z0 = (u32)fz, x1 = MIN(x0 + 1, vg->mask_w - 1), z1 = MIN(z0 + 1, vg->mask_h - 1);
    f32 ax = (f32)(fx - x0), az = (f32)(fz - z0);
    const u8* m = vg->mask;
    f32 a = m[2 * ((size_t)z0 * vg->mask_w + x0) + channel], b = m[2 * ((size_t)z0 * vg->mask_w + x1) + channel];
    f32 c = m[2 * ((size_t)z1 * vg->mask_w + x0) + channel], d = m[2 * ((size_t)z1 * vg->mask_w + x1) + channel];
    return ((a + (b - a) * ax) * (1 - az) + (c + (d - c) * ax) * az) / 255.0f;
}

// ---------------------------------------------------------------------------------------------------------------
// Procedural trees

typedef enum { CROWN_CONE, CROWN_UMBRELLA, CROWN_EGG, CROWN_WIDE, CROWN_NARROW } CrownShape;
typedef enum { SPRUCE, PINE, BEECH, OAK, BIRCH } Species;

typedef struct {
    const char* name;
    CrownShape shape;
    bool conifer;
    f32 height, trunk_radius;   // m
    f32 crown_base;             // fraction of height
    f32 crown_radius;           // m
    u32 branches;               // conifer: per whorl; broadleaf: primary branches
    f32 angle_low, angle_high;  // branch angle from vertical (degrees) at crown base / top
    f32 droop;                  // downward bend per branch segment
    f32 leaf_size;              // card size (m)
    f32 leaf_spacing;           // card spacing along twigs (m)
    f32 leaf[3], bark[3];       // albedo
} SpeciesDesc;

static const SpeciesDesc SPECIES[TREE_SPECIES] = {
    {"spruce", CROWN_CONE,     true,  28, 0.30, 0.18, 3.0,  5, 105, 60, 0.10, 0.34, 0.17, {0.035f, 0.075f, 0.040f}, {0.20f, 0.13f, 0.09f}},
    {"pine",   CROWN_UMBRELLA, true,  24, 0.27, 0.62, 3.2,  4,  80, 45, 0.02, 0.40, 0.15, {0.070f, 0.110f, 0.050f}, {0.33f, 0.19f, 0.11f}},
    {"beech",  CROWN_EGG,      false, 26, 0.32, 0.35, 5.0, 26,  62, 28, 0.00, 0.60, 0.12, {0.090f, 0.180f, 0.045f}, {0.36f, 0.36f, 0.34f}},
    {"oak",    CROWN_WIDE,     false, 22, 0.42, 0.30, 6.5, 22,  75, 45, 0.00, 0.55, 0.12, {0.075f, 0.150f, 0.045f}, {0.17f, 0.15f, 0.12f}},
    {"birch",  CROWN_NARROW,   false, 20, 0.17, 0.35, 3.0, 28,  50, 28, 0.06, 0.42, 0.11, {0.130f, 0.230f, 0.065f}, {0.62f, 0.60f, 0.56f}},
};

// Crown radius as a fraction of the maximum, hn = 0 at the crown base .. 1 at the top.
static f32 crown_envelope(CrownShape s, f32 hn)
{
    hn = CLAMP(hn, 0.0f, 1.0f);
    f32 t;
    switch (s) {
    case CROWN_CONE:     return MAX(0.04f, 1.0f - hn);
    case CROWN_UMBRELLA: return MAX(0.05f, sqrtf(sinf(3.14159f * hn)));
    case CROWN_EGG:      t = (hn - 0.4f) / (hn < 0.4f ? 0.45f : 0.6f); return MAX(0.05f, sqrtf(MAX(0.0f, 1 - t * t)));
    case CROWN_WIDE:     t = (hn - 0.45f) / (hn < 0.45f ? 0.5f : 0.55f); return MAX(0.05f, powf(MAX(0.0f, 1 - t * t), 0.6f));
    case CROWN_NARROW:   t = (hn - 0.45f) / (hn < 0.45f ? 0.5f : 0.55f); return MAX(0.05f, sqrtf(MAX(0.0f, 1 - t * t)) * 0.95f);
    }
    return 1.0f;
}

typedef struct {
    TreeVertex* v;
    u32 vn, vcap;
    u32* t;                     // 3 indices per triangle
    u32 tn, tcap;
} MeshBuild;

static u32 pack_snorm10(v3 n)
{
    i32 x = (i32)lrintf(CLAMP(n.x, -1.0f, 1.0f) * 511.0f);
    i32 y = (i32)lrintf(CLAMP(n.y, -1.0f, 1.0f) * 511.0f);
    i32 z = (i32)lrintf(CLAMP(n.z, -1.0f, 1.0f) * 511.0f);
    return ((u32)x & 1023u) | ((u32)y & 1023u) << 10 | ((u32)z & 1023u) << 20;
}

static u32 mb_vertex(MeshBuild* mb, v3 p, v3 n, f32 u, f32 v, u32 mat, f32 ao, f32 sway, u32 seed)
{
    if (mb->vn == mb->vcap) { mb->vcap = mb->vcap ? mb->vcap * 2 : 4096; mb->v = (TreeVertex*)realloc(mb->v, mb->vcap * sizeof(TreeVertex)); }
    TreeVertex* o = &mb->v[mb->vn];
    o->x = p.x; o->y = p.y; o->z = p.z;
    o->normal = pack_snorm10(v3_norm(n));
    u32 uu = (u32)lrintf(CLAMP(u, 0.0f, 63.9f) * 1024.0f), vv = (u32)lrintf(CLAMP(v, 0.0f, 63.9f) * 1024.0f);
    o->uv = uu | vv << 16;
    o->attr = mat | (u32)lrintf(CLAMP(ao, 0.0f, 1.0f) * 255.0f) << 8 | (u32)lrintf(CLAMP(sway, 0.0f, 1.0f) * 255.0f) << 16 | (seed & 255u) << 24;
    return mb->vn++;
}

static void mb_tri(MeshBuild* mb, u32 a, u32 b, u32 c)
{
    if (mb->tn + 3 > mb->tcap) { mb->tcap = mb->tcap ? mb->tcap * 2 : 12288; mb->t = (u32*)realloc(mb->t, mb->tcap * sizeof(u32)); }
    mb->t[mb->tn++] = a; mb->t[mb->tn++] = b; mb->t[mb->tn++] = c;
}

// Generalised cylinder along a polyline (parallel-transport frames), no caps.
static void mb_tube(MeshBuild* mb, const v3* pts, const f32* radius, u32 n, u32 sides, f32 ao0, f32 ao1, f32 sway0, f32 sway1)
{
    v3 d = v3_norm(v3_sub(pts[1], pts[0]));
    v3 ref = fabsf(d.y) < 0.9f ? v3_make(0, 1, 0) : v3_make(1, 0, 0);
    v3 nx = v3_norm(v3_cross(d, ref));
    f32 length = 0, circumference = 6.2831853f * MAX(radius[0], 0.02f);
    u32 first = mb->vn;
    for (u32 k = 0; k < n; k++) {
        if (k > 0) length += sqrtf(v3_dot(v3_sub(pts[k], pts[k - 1]), v3_sub(pts[k], pts[k - 1])));
        v3 dk = k == 0 ? d : k == n - 1 ? v3_norm(v3_sub(pts[k], pts[k - 1])) : v3_norm(v3_sub(pts[k + 1], pts[k - 1]));
        nx = v3_norm(v3_sub(nx, v3_scale(dk, v3_dot(nx, dk))));
        v3 ny = v3_cross(dk, nx);
        f32 t = (f32)k / (f32)(n - 1);
        for (u32 s = 0; s <= sides; s++) {
            f32 a = 6.2831853f * s / sides;
            v3 dir = v3_add(v3_scale(nx, cosf(a)), v3_scale(ny, sinf(a)));
            mb_vertex(mb, v3_add(pts[k], v3_scale(dir, radius[k])), dir, (f32)s / sides, length / circumference, MAT_BARK,
                      ao0 + (ao1 - ao0) * t, sway0 + (sway1 - sway0) * t, 0);
        }
    }
    for (u32 k = 0; k + 1 < n; k++)
        for (u32 s = 0; s < sides; s++) {
            u32 a = first + k * (sides + 1) + s, b = a + 1, c = a + sides + 1, e = c + 1;
            mb_tri(mb, a, c, b);
            mb_tri(mb, b, c, e);
        }
}

// Quad o, o + du, o + du + dv, o + dv with uv (0,0) .. (1,1), u along du.
static void mb_card(MeshBuild* mb, v3 o, v3 du, v3 dv, v3 n, u32 mat, f32 ao, f32 sway, u32 seed)
{
    u32 a = mb_vertex(mb, o, n, 0, 0, mat, ao, sway, seed);
    u32 b = mb_vertex(mb, v3_add(o, du), n, 1, 0, mat, ao, sway, seed);
    u32 c = mb_vertex(mb, v3_add(v3_add(o, du), dv), n, 1, 1, mat, ao, sway, seed);
    u32 d = mb_vertex(mb, v3_add(o, dv), n, 0, 1, mat, ao, sway, seed);
    mb_tri(mb, a, b, c);
    mb_tri(mb, a, c, d);
}

static f32 rnd(u32 seed, u32 k) { return hash_unit(hash_u32(seed * 0x9e3779b1u + k * 0x85ebca77u + 0x165667b1u)); }

static v3 rnd_unit(u32 seed, u32 k)
{
    f32 z = rnd(seed, k) * 2 - 1, a = rnd(seed, k + 1) * 6.2831853f, r = sqrtf(MAX(0.0f, 1 - z * z));
    return v3_make(r * cosf(a), z, r * sinf(a));
}

static v3 polyline_at(const v3* pts, u32 n, f32 t, v3* dir)
{
    f32 f = CLAMP(t, 0.0f, 1.0f) * (n - 1);
    u32 i = MIN((u32)f, n - 2);
    f32 a = f - i;
    *dir = v3_norm(v3_sub(pts[i + 1], pts[i]));
    return v3_add(pts[i], v3_scale(v3_sub(pts[i + 1], pts[i]), a));
}

typedef struct {
    f32 height, radius, crown_base, trunk_top;
} TreeDims;

static f32 trunk_radius_at(const SpeciesDesc* sp, const TreeDims* td, f32 y)
{
    return sp->trunk_radius * (1.0f - 0.85f * CLAMP(y / td->trunk_top, 0.0f, 1.0f)) + 0.015f;
}

static f32 sway_at(const TreeDims* td, f32 y) { return CLAMP(powf(MAX(y, 0.0f) / td->height, 1.5f), 0.0f, 1.0f); }

// Foliage AO: dark inside and low in the crown, bright at the outer shell (stand-in for self-shadowing).
static f32 foliage_ao(const SpeciesDesc* sp, const TreeDims* td, v3 p)
{
    f32 hn = (p.y - td->crown_base) / (td->height - td->crown_base);
    f32 reach = td->radius * crown_envelope(sp->shape, hn);
    f32 radial = sqrtf(p.x * p.x + p.z * p.z) / MAX(reach, 0.3f);
    return CLAMP(0.25f + 0.75f * CLAMP(radial, 0.0f, 1.0f) * (0.55f + 0.45f * CLAMP(hn, 0.0f, 1.0f)), 0.2f, 1.0f);
}

// Card facing n, centred at c, square of side s.
static void leaf_card(MeshBuild* mb, v3 c, v3 n, f32 s, u32 mat, f32 ao, f32 sway, u32 seed)
{
    v3 ref = fabsf(n.y) < 0.95f ? v3_make(0, 1, 0) : v3_make(1, 0, 0);
    v3 du = v3_scale(v3_norm(v3_cross(ref, n)), s);
    v3 dv = v3_scale(v3_norm(v3_cross(n, du)), s);
    mb_card(mb, v3_sub(c, v3_scale(v3_add(du, dv), 0.5f)), du, dv, n, mat, ao, sway, seed);
}

// Needle spray along direction d from base p, lying in the plane with normal pn.
static void needle_card(MeshBuild* mb, v3 p, v3 d, v3 pn, f32 len, f32 width, f32 ao, f32 sway, u32 seed)
{
    v3 across = v3_norm(v3_cross(pn, d));
    mb_card(mb, v3_sub(p, v3_scale(across, width * 0.5f)), v3_scale(d, len), v3_scale(across, width), pn, MAT_NEEDLE, ao, sway, seed);
}

static void tree_branch_conifer(MeshBuild* mb, const SpeciesDesc* sp, const TreeDims* td, u32 lod, u32 bseed, f32 y, f32 az)
{
    f32 hn = (y - td->crown_base) / (td->height - td->crown_base);
    f32 el = (sp->angle_low + (sp->angle_high - sp->angle_low) * hn) * (0.92f + 0.16f * rnd(bseed, 1)) * 3.14159f / 180.0f;
    v3 h = v3_make(cosf(az), 0, sinf(az));
    v3 dir = v3_add(v3_scale(h, sinf(el)), v3_make(0, cosf(el), 0));
    f32 reach = td->radius * crown_envelope(sp->shape, hn) * (0.85f + 0.3f * rnd(bseed, 2));
    f32 length = MAX(0.3f, reach / MAX(sinf(el), 0.35f));
    v3 pts[4];
    f32 rad[4];
    f32 r0 = MAX(0.012f, trunk_radius_at(sp, td, y) * 0.3f);
    pts[0] = v3_add(v3_make(0, y, 0), v3_scale(h, trunk_radius_at(sp, td, y) * 0.7f));
    for (u32 k = 1; k < 4; k++) {
        pts[k] = v3_add(pts[k - 1], v3_scale(dir, length / 3));
        dir = v3_norm(v3_sub(dir, v3_make(0, sp->droop, 0)));
    }
    for (u32 k = 0; k < 4; k++) rad[k] = r0 + (0.005f - r0) * k / 3.0f;
    f32 ao_in = foliage_ao(sp, td, pts[0]) * 0.8f, ao_out = foliage_ao(sp, td, pts[3]);
    mb_tube(mb, pts, rad, 4, lod == 0 ? 4 : 3, ao_in, ao_out, sway_at(td, y), MIN(1.0f, sway_at(td, y) + 0.3f));

    // Sprays. Pine: tufts on the outer half only, tilted at random.
    bool pine = sp->shape == CROWN_UMBRELLA;
    f32 spacing = sp->leaf_spacing * (lod == 0 ? 1.0f : 2.2f), size = lod == 0 ? 1.0f : 1.8f;
    u32 k = 0;
    for (f32 s = spacing * 0.5f; s < length; s += spacing, k++) {
        f32 t = s / length;
        if (pine && t < 0.45f) continue;
        v3 d;
        v3 p = polyline_at(pts, 4, t, &d);
        v3 pn = v3_norm(v3_sub(v3_make(0, 1, 0), v3_scale(d, d.y)));
        if (pine) pn = v3_norm(v3_add(pn, v3_scale(rnd_unit(bseed, 10 + k * 3), 0.7f)));
        f32 len = sp->leaf_size * 1.6f * size * (1.0f - 0.35f * t), width = sp->leaf_size * size;
        f32 ao = foliage_ao(sp, td, p), sway = MIN(1.0f, sway_at(td, y) + 0.3f * t);
        u32 seed = hash_u32(bseed + k);
        if (lod == 0) {
            needle_card(mb, p, d, pn, len, width, ao, sway, seed);
            v3 across = v3_norm(v3_cross(pn, d));
            for (i32 side = -1; side <= 1; side += 2) {
                v3 sd = v3_norm(v3_add(v3_scale(d, 0.64f), v3_scale(across, 0.77f * side)));
                needle_card(mb, p, sd, pn, len * 0.7f, width * 0.8f, ao, sway, seed + side);
            }
        } else {
            needle_card(mb, p, d, pn, len * 1.2f, width * 1.8f, ao, sway, seed);
        }
    }
}

static void broadleaf_leaves(MeshBuild* mb, const SpeciesDesc* sp, const TreeDims* td, const v3* pts, u32 n, f32 t0,
                             f32 spacing, f32 size, u32 bseed)
{
    f32 length = 0;
    for (u32 k = 1; k < n; k++) length += sqrtf(v3_dot(v3_sub(pts[k], pts[k - 1]), v3_sub(pts[k], pts[k - 1])));
    u32 k = 0;
    for (f32 s = t0 * length; s < length; s += spacing, k++) {
        v3 d;
        v3 p = polyline_at(pts, n, s / length, &d);
        p = v3_add(p, v3_scale(rnd_unit(bseed, 100 + k * 5), 0.12f));
        v3 out = v3_norm(v3_make(p.x, 0.0f, p.z));
        v3 nrm = v3_norm(v3_add(v3_add(v3_scale(rnd_unit(bseed, 200 + k * 5), 0.7f), out), v3_make(0, 0.45f, 0)));
        f32 sz = size * (0.85f + 0.3f * rnd(bseed, 300 + k));
        leaf_card(mb, p, nrm, sz, MAT_LEAF, foliage_ao(sp, td, p), MIN(1.0f, sway_at(td, p.y) + 0.25f), hash_u32(bseed + k * 7));
    }
}

static void tree_branch_broadleaf(MeshBuild* mb, const SpeciesDesc* sp, const TreeDims* td, u32 lod, u32 bseed, f32 hn, f32 az)
{
    f32 y = td->crown_base + hn * (td->trunk_top - td->crown_base) * 0.95f;
    f32 el = (sp->angle_low + (sp->angle_high - sp->angle_low) * hn) * (0.9f + 0.2f * rnd(bseed, 1)) * 3.14159f / 180.0f;
    v3 h = v3_make(cosf(az), 0, sinf(az));
    v3 dir = v3_add(v3_scale(h, sinf(el)), v3_make(0, cosf(el), 0));
    f32 reach = td->radius * crown_envelope(sp->shape, hn + 0.1f) * (0.85f + 0.3f * rnd(bseed, 2));
    f32 length = CLAMP(reach / MAX(sinf(el), 0.35f), 0.8f, td->height);
    v3 pts[5];
    f32 rad[5];
    f32 r0 = trunk_radius_at(sp, td, y) * 0.45f;
    pts[0] = v3_make(0, y, 0);
    for (u32 k = 1; k < 5; k++) {
        pts[k] = v3_add(pts[k - 1], v3_scale(dir, length / 4));
        dir = v3_norm(v3_add(dir, v3_make(0, 0.06f - sp->droop, 0)));
    }
    for (u32 k = 0; k < 5; k++) rad[k] = r0 + (0.01f - r0) * k / 4.0f;
    mb_tube(mb, pts, rad, 5, lod == 0 ? 5 : 3, 0.4f, foliage_ao(sp, td, pts[4]), sway_at(td, y), MIN(1.0f, sway_at(td, y) + 0.25f));

    if (lod == 1) {
        broadleaf_leaves(mb, sp, td, pts, 5, 0.3f, sp->leaf_spacing * 2.0f, sp->leaf_size * 2.6f, bseed);
        return;
    }
    broadleaf_leaves(mb, sp, td, pts, 5, 0.65f, sp->leaf_spacing, sp->leaf_size, bseed ^ 0x55u);
    const u32 twigs = 7;
    for (u32 c = 0; c < twigs; c++) {
        u32 cseed = hash_u32(bseed + 1000 + c);
        f32 t = 0.25f + 0.7f * (c + rnd(cseed, 1)) / twigs;
        v3 d;
        v3 p = polyline_at(pts, 5, t, &d);
        v3 perp = v3_norm(v3_cross(d, rnd_unit(cseed, 2)));
        v3 sd = v3_norm(v3_add(v3_add(v3_scale(d, 0.7f), v3_scale(perp, 0.7f)), v3_make(0, 0.1f - sp->droop * 4.0f, 0)));
        f32 slen = length * (0.35f + 0.15f * rnd(cseed, 3)) * (1.0f - 0.4f * t);
        v3 sp3[3] = {p, v3_add(p, v3_scale(sd, slen * 0.5f)), v3_add(p, v3_scale(v3_norm(v3_add(sd, v3_make(0, -sp->droop * 3.0f, 0))), slen))};
        f32 sr0 = MAX(0.008f, rad[0] * 0.45f * (1.0f - t));
        f32 srad[3] = {sr0, sr0 * 0.6f, 0.005f};
        mb_tube(mb, sp3, srad, 3, 3, foliage_ao(sp, td, p), foliage_ao(sp, td, sp3[2]), sway_at(td, p.y), MIN(1.0f, sway_at(td, p.y) + 0.3f));
        broadleaf_leaves(mb, sp, td, sp3, 3, 0.2f, sp->leaf_spacing, sp->leaf_size, cseed);
    }
}

static void tree_build(MeshBuild* mb, const SpeciesDesc* sp, u32 seed, u32 lod, TreeDims* td)
{
    td->height = sp->height * (0.92f + 0.16f * rnd(seed, 1));
    td->radius = sp->crown_radius * (0.9f + 0.2f * rnd(seed, 2));
    td->crown_base = td->height * sp->crown_base;
    td->trunk_top = sp->conifer ? td->height : td->height * 0.88f;

    if (lod == 3) {   // three crossed quads; the silhouette comes from billboard_alpha()
        f32 w = td->radius * 1.05f;
        for (u32 k = 0; k < 3; k++) {
            f32 a = k * 3.14159f / 3.0f;
            v3 across = v3_make(cosf(a), 0, sinf(a));
            v3 n = v3_make(-sinf(a), 0, cosf(a));
            mb_card(mb, v3_scale(across, -w), v3_scale(across, 2 * w), v3_make(0, td->height, 0), n, MAT_BILLBOARD, 1.0f, 0.5f, hash_u32(seed + k));
        }
        return;
    }

    // Trunk, slightly leaning and bent.
    const u32 tn = 8;
    v3 pts[tn];
    f32 rad[tn];
    f32 lean_x = (rnd(seed, 3) - 0.5f) * 0.6f, lean_z = (rnd(seed, 4) - 0.5f) * 0.6f;
    f32 top = lod == 2 ? td->crown_base + (td->height - td->crown_base) * 0.6f : td->trunk_top;
    for (u32 k = 0; k < tn; k++) {
        f32 t = (f32)k / (tn - 1), y = t * top;
        pts[k] = v3_make(lean_x * t * t, y, lean_z * t * t);
        rad[k] = trunk_radius_at(sp, td, y) * (k == 0 ? 1.3f : 1.0f);
    }
    static const u32 trunk_sides[3] = {10, 6, 5};
    mb_tube(mb, pts, rad, tn, trunk_sides[lod], 0.7f, 0.45f, 0.0f, sway_at(td, top));

    if (lod == 2) {   // crown as a shell of blob cards
        u32 count = sp->conifer ? 70 : 55;
        for (u32 c = 0; c < count; c++) {
            f32 hn = (c + rnd(seed, 10 + c)) / count;
            f32 y = td->crown_base + hn * (td->height - td->crown_base);
            f32 rr = td->radius * crown_envelope(sp->shape, hn) * (0.55f + 0.35f * rnd(seed, 200 + c));
            f32 az = c * 2.39996f + rnd(seed, 400 + c);
            v3 p = v3_make(cosf(az) * rr, y, sinf(az) * rr);
            v3 out = v3_norm(v3_make(p.x, (y - (td->crown_base + td->height) * 0.5f) * 0.4f, p.z));
            v3 n = v3_norm(v3_add(out, v3_scale(rnd_unit(seed, 600 + c * 2), 0.5f)));
            f32 s = sp->conifer ? (td->height - td->crown_base) * 0.22f : td->radius * 0.85f;
            leaf_card(mb, p, n, s, MAT_CLUSTER, foliage_ao(sp, td, p), sway_at(td, y), hash_u32(seed + c));
        }
        return;
    }

    if (sp->conifer) {
        f32 step = lod == 0 ? 0.55f : 1.1f;
        u32 whorl = 0;
        for (f32 y = td->crown_base; y < td->height - 0.4f; y += step * (0.85f + 0.3f * rnd(seed, 700 + whorl)), whorl++) {
            u32 count = sp->branches + (rnd(seed, 900 + whorl) < 0.5f ? 1 : 0);
            f32 az0 = rnd(seed, 1100 + whorl) * 6.2831853f;
            for (u32 b = 0; b < count; b++) {
                u32 bseed = hash_u32(seed ^ (whorl * 64 + b) * 0x27d4eb2fu);
                f32 az = az0 + b * 6.2831853f / count + (rnd(bseed, 0) - 0.5f) * 0.5f;
                tree_branch_conifer(mb, sp, td, lod, bseed, y, az);
            }
        }
        // Leader: a short spray at the top.
        needle_card(mb, v3_make(0, td->height - 0.6f, 0), v3_make(0, 1, 0), v3_make(1, 0, 0), 0.8f, 0.4f, 1.0f, 1.0f, seed);
    } else {
        for (u32 i = 0; i < sp->branches; i++) {
            u32 bseed = hash_u32(seed ^ i * 0x27d4eb2fu);
            f32 hn = powf((i + 0.5f + (rnd(bseed, 0) - 0.5f) * 0.6f) / sp->branches, 0.85f);
            f32 az = i * 2.39996f + (rnd(bseed, 5) - 0.5f) * 0.6f;
            tree_branch_broadleaf(mb, sp, td, lod, bseed, CLAMP(hn, 0.0f, 1.0f), az);
        }
    }
}

typedef struct {
    TreeVertex* v;
    u32 vn, vcap;
    u32* t;                     // packed local triangles
    u32 tn, tcap;
    TreeMeshlet* m;
    u32 mn, mcap;
} MeshletArrays;

// Greedy meshlets in generation order (branches and cards are generated locally, so locality is decent).
static void meshletize(MeshletArrays* out, const MeshBuild* mb, u32* first, u32* count)
{
    *first = out->mn;
    i32* remap = (i32*)malloc(MAX(mb->vn, 1u) * sizeof(i32));
    for (u32 i = 0; i < mb->vn; i++) remap[i] = -1;
    u32 local[TREE_MESHLET_VERTS], lv = 0;
    u32 tris[TREE_MESHLET_TRIS], lt = 0;
    auto flush = [&]() {
        if (!lt) return;
        if (out->mn == out->mcap) { out->mcap = out->mcap ? out->mcap * 2 : 1024; out->m = (TreeMeshlet*)realloc(out->m, out->mcap * sizeof(TreeMeshlet)); }
        if (out->vn + lv > out->vcap) { out->vcap = MAX(out->vcap * 2, out->vn + lv + 65536); out->v = (TreeVertex*)realloc(out->v, out->vcap * sizeof(TreeVertex)); }
        if (out->tn + lt > out->tcap) { out->tcap = MAX(out->tcap * 2, out->tn + lt + 65536); out->t = (u32*)realloc(out->t, out->tcap * sizeof(u32)); }
        out->m[out->mn++] = {out->vn, out->tn, lv, lt};
        for (u32 k = 0; k < lv; k++) { out->v[out->vn++] = mb->v[local[k]]; remap[local[k]] = -1; }
        for (u32 k = 0; k < lt; k++) out->t[out->tn++] = tris[k];
        lv = lt = 0;
    };
    for (u32 i = 0; i < mb->tn; i += 3) {
        u32 idx[3] = {mb->t[i], mb->t[i + 1], mb->t[i + 2]};
        u32 fresh = (remap[idx[0]] < 0) + (remap[idx[1]] < 0 && idx[1] != idx[0]) + (remap[idx[2]] < 0 && idx[2] != idx[0] && idx[2] != idx[1]);
        if (lv + fresh > TREE_MESHLET_VERTS || lt == TREE_MESHLET_TRIS) flush();
        u32 packed = 0;
        for (u32 k = 0; k < 3; k++) {
            if (remap[idx[k]] < 0) { remap[idx[k]] = (i32)lv; local[lv++] = idx[k]; }
            packed |= (u32)remap[idx[k]] << (8 * k);
        }
        tris[lt++] = packed;
    }
    flush();
    free(remap);
    *count = out->mn - *first;
}

// ---------------------------------------------------------------------------------------------------------------
// Placement

static void veg_place(Vegetation* vg, const Terrain* t)
{
    const TerrainFileHeader* h = &t->hdr;
    vg->cells_x = (u32)ceil(vg->extent_x / TREE_CELL);
    vg->cells_z = (u32)ceil(vg->extent_z / TREE_CELL);
    vg->cells = (TreeCell*)calloc((size_t)vg->cells_x * vg->cells_z, sizeof(TreeCell));
    u32 cap = 1u << 20;
    vg->instances = (TreeInstance*)malloc(cap * sizeof(TreeInstance));
    vg->instance_count = 0;
    f32 max_height[TREE_TYPES];
    for (u32 k = 0; k < TREE_TYPES; k++) max_height[k] = vg->types[k].bark_color.w;
    const f64 s = TREE_SPACING;
    for (u32 cz = 0; cz < vg->cells_z; cz++)
        for (u32 cx = 0; cx < vg->cells_x; cx++) {
            TreeCell* cell = &vg->cells[cz * vg->cells_x + cx];
            cell->first = vg->instance_count;
            cell->ymin = 1e30f; cell->ymax = -1e30f;
            f64 x0 = cx * TREE_CELL, z0 = cz * TREE_CELL;
            i64 i0 = (i64)ceil(x0 / s), i1 = (i64)ceil(MIN(x0 + TREE_CELL, (f64)vg->extent_x) / s);
            i64 j0 = (i64)ceil(z0 / s), j1 = (i64)ceil(MIN(z0 + TREE_CELL, (f64)vg->extent_z) / s);
            for (i64 j = j0; j < j1; j++)
                for (i64 i = i0; i < i1; i++) {
                    // Hash by absolute grid index, so placement does not depend on the map crop.
                    i64 gi = i + (i64)floor(h->origin_e / s), gj = j + (i64)floor(h->origin_n / s);
                    u32 r = hash3(gi, gj, 0x51a7u);
                    f64 x = (i + (hash_unit(r) - 0.5) * 0.9) * s, z = (j + (hash_unit(hash_u32(r + 1)) - 0.5) * 0.9) * s;
                    if (x < 0 || z < 0 || x > vg->extent_x || z > vg->extent_z) continue;
                    f32 forest = veg_mask_sample(vg, x, z, 0);
                    bool in_forest = hash_unit(hash_u32(r + 2)) < forest * 0.98f;
                    bool solitary = !in_forest && hash_unit(hash_u32(r + 3)) < 0.0035f * (1.0f - forest);
                    if (!in_forest && !solitary) continue;

                    f32 stand = fbm((h->origin_e + x) / 500.0, (h->origin_n + z) / 500.0, 41, 3);
                    f32 pick = hash_unit(hash_u32(r + 4));
                    Species sp;
                    if (solitary) sp = pick < 0.45f ? OAK : pick < 0.75f ? BIRCH : BEECH;
                    else if (forest < 0.8f && pick < 0.45f) sp = pick < 0.25f ? BIRCH : OAK;     // forest edge
                    else if (stand > 0.56f) sp = pick < 0.8f ? SPRUCE : BEECH;
                    else if (stand < 0.40f) sp = pick < 0.8f ? PINE : BIRCH;
                    else sp = pick < 0.55f ? BEECH : pick < 0.7f ? OAK : SPRUCE;
                    u32 type = (u32)sp * TREE_VARIANTS + (hash_u32(r + 5) & 1u);
                    f32 scale = solitary ? 1.0f + 0.3f * hash_unit(hash_u32(r + 6)) : 0.8f + 0.4f * hash_unit(hash_u32(r + 6));
                    if (!solitary && forest < 0.8f) scale *= 0.85f;
                    u32 scale_byte = (u32)lrintf(CLAMP((scale - 0.6f) / 0.8f, 0.0f, 1.0f) * 255.0f);
                    f32 y = terrain_height(t, x, z) - 0.15f - h->height_min;

                    if (vg->instance_count == cap) { cap *= 2; vg->instances = (TreeInstance*)realloc(vg->instances, cap * sizeof(TreeInstance)); }
                    TreeInstance* ins = &vg->instances[vg->instance_count++];
                    ins->x = (f32)(x - x0); ins->y = y; ins->z = (f32)(z - z0);
                    ins->packed = type | (hash_u32(r + 7) & 255u) << 8 | scale_byte << 16 | (hash_u32(r + 8) & 255u) << 24;
                    cell->ymin = MIN(cell->ymin, y);
                    cell->ymax = MAX(cell->ymax, y + max_height[type] * (0.6f + scale_byte / 255.0f * 0.8f));
                }
            cell->count = vg->instance_count - cell->first;
        }
}

// ---------------------------------------------------------------------------------------------------------------
// Load, upload, pipelines

// Task shader in its own module, mesh + fragment shaders in another (see PipelineDesc.task_module). shadow: depth-only
// pipeline with the fs_<name>_shadow fragment shader (alpha test).
static VkPipeline veg_pipeline(Vk* vk, const char* shader_dir, const char* name, bool shadow = false)
{
    char path[1024], ts[32], ms[32], fs[32];
    snprintf(path, sizeof(path), "%sveg_%s_task.spv", shader_dir, name);
    VkShaderModule task = vk_load_shader(vk, path);
    snprintf(path, sizeof(path), "%sveg_%s.spv", shader_dir, name);
    VkShaderModule mesh = vk_load_shader(vk, path);
    snprintf(ts, sizeof(ts), "as_%s", name);
    snprintf(ms, sizeof(ms), "ms_%s", name);
    snprintf(fs, sizeof(fs), shadow ? "fs_%s_shadow" : "fs_%s", name);
    PipelineDesc d = {mesh, NULL, fs, VK_COMPARE_OP_GREATER, true, VK_CULL_MODE_NONE, VK_POLYGON_MODE_FILL, ts, ms, task};
    d.depth_only = shadow;
    VkPipeline p = vk_create_pipeline(vk, &d);
    vkDestroyShaderModule(vk->device, task, NULL);
    vkDestroyShaderModule(vk->device, mesh, NULL);
    return p;
}

// shader_dir: directory of the SPIR-V modules, with trailing separator.
static void veg_init(Vegetation* vg, const Terrain* t, Vk* vk, const char* shader_dir)
{
    u64 t0 = SDL_GetPerformanceCounter();
    f64 freq = (f64)SDL_GetPerformanceFrequency();
    veg_build_mask(vg, t);
    vg->mask_tex = vk_texture_2d(vk, VK_FORMAT_R8G8_UNORM, 2, vg->mask_w, vg->mask_h, vg->mask, true);
    vk_bind_texture(vk, TEX_MASK, vg->mask_tex.view);
    u64 t1 = SDL_GetPerformanceCounter();
    printf("vegetation mask: %u x %u (%.1f m), %.0f ms\n", vg->mask_w, vg->mask_h, vg->extent_x / vg->mask_w, (t1 - t0) * 1000.0 / freq);

    vg->enabled = vk->mesh_shaders;
    if (!vg->enabled) return;

    MeshBuild mb = {};
    MeshletArrays ma = {};
    for (u32 type = 0; type < TREE_TYPES; type++) {
        const SpeciesDesc* sp = &SPECIES[type / TREE_VARIANTS];
        u32 seed = hash_u32(0x7a3e5eedu + type * 977u);
        TreeType* tt = &vg->types[type];
        TreeDims td = {};
        for (u32 lod = 0; lod < TREE_LODS; lod++) {
            mb.vn = mb.tn = 0;
            tree_build(&mb, sp, seed, lod, &td);
            meshletize(&ma, &mb, &tt->meshlet_first[lod], &tt->meshlet_count[lod]);
            vg->lod_tris[type][lod] = mb.tn / 3;
        }
        tt->leaf_color = {sp->leaf[0], sp->leaf[1], sp->leaf[2], 0};
        tt->bark_color = {sp->bark[0], sp->bark[1], sp->bark[2], td.height};
        f32 r = sqrtf(td.height * td.height * 0.25f + td.radius * td.radius) * 1.1f;
        tt->bounds = {0, td.height * 0.5f, 0, r};
        tt->shape = {sp->crown_base, sp->conifer ? 1.0f : 0.0f, td.radius / td.height, sp->trunk_radius / td.radius};
    }
    free(mb.v);
    free(mb.t);
    for (u32 i = 0; i < ma.mn; i++) {   // the mesh shader trusts these limits
        const TreeMeshlet* m = &ma.m[i];
        ASSERT(m->vertex_count <= TREE_MESHLET_VERTS && m->triangle_count <= TREE_MESHLET_TRIS && m->triangle_count > 0);
        for (u32 k = 0; k < m->triangle_count; k++) {
            u32 x = ma.t[m->triangle_offset + k];
            ASSERT((x & 255) < m->vertex_count && ((x >> 8) & 255) < m->vertex_count && ((x >> 16) & 255) < m->vertex_count);
        }
    }
    u64 t2 = SDL_GetPerformanceCounter();
    for (u32 sp = 0; sp < TREE_SPECIES; sp++) {
        u32 type = sp * TREE_VARIANTS;
        printf("  %-6s triangles per LOD: %6u %6u %5u %2u   meshlets: %4u %4u %3u %u\n", SPECIES[sp].name,
               vg->lod_tris[type][0], vg->lod_tris[type][1], vg->lod_tris[type][2], vg->lod_tris[type][3],
               vg->types[type].meshlet_count[0], vg->types[type].meshlet_count[1], vg->types[type].meshlet_count[2], vg->types[type].meshlet_count[3]);
    }

    veg_place(vg, t);
    u64 t3 = SDL_GetPerformanceCounter();
    printf("trees: %u types, %u meshlets (%.1f MB), %u instances (%.1f MB), generate %.0f ms, place %.0f ms\n",
           TREE_TYPES, ma.mn, (ma.vn * sizeof(TreeVertex) + ma.tn * 4 + ma.mn * sizeof(TreeMeshlet)) / 1048576.0,
           vg->instance_count, vg->instance_count * sizeof(TreeInstance) / 1048576.0, (t2 - t1) * 1000.0 / freq, (t3 - t2) * 1000.0 / freq);

    VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    vg->instance_buf = vk_buffer_static(vk, (VkDeviceSize)vg->instance_count * sizeof(TreeInstance), usage, vg->instances);
    vg->type_buf = vk_buffer_static(vk, sizeof(vg->types), usage, vg->types);
    vg->meshlet_buf = vk_buffer_static(vk, (VkDeviceSize)ma.mn * sizeof(TreeMeshlet), usage, ma.m);
    vg->vertex_buf = vk_buffer_static(vk, (VkDeviceSize)ma.vn * sizeof(TreeVertex), usage, ma.v);
    vg->triangle_buf = vk_buffer_static(vk, (VkDeviceSize)ma.tn * sizeof(u32), usage, ma.t);
    VegScene scene = {vg->instance_buf.address, vg->type_buf.address, vg->meshlet_buf.address, vg->vertex_buf.address, vg->triangle_buf.address};
    vg->scene = vk_buffer_static(vk, sizeof(scene), usage, &scene);
    free(ma.v);
    free(ma.t);
    free(ma.m);

    vg->pipe_trees = veg_pipeline(vk, shader_dir, "trees");
    vg->pipe_grass = veg_pipeline(vk, shader_dir, "grass");
}

// ---------------------------------------------------------------------------------------------------------------
// Per frame

// Visible cells -> chunks of ≤ TREE_CHUNK instances. cam_local: camera in terrain-local metres.
static void veg_select_trees(Vegetation* vg, const TerrainSelect* sel, const f64 origin[3], const f64 cam_local[3], f32 draw_dist,
                             TreeChunk* out)
{
    vg->chunk_count = vg->tree_candidates = 0;
    const f32 pad = 12.0f;      // crown radius beyond the cell edge
    i32 cx0 = MAX(0, (i32)floor((cam_local[0] - draw_dist) / TREE_CELL)), cx1 = MIN((i32)vg->cells_x - 1, (i32)floor((cam_local[0] + draw_dist) / TREE_CELL));
    i32 cz0 = MAX(0, (i32)floor((cam_local[2] - draw_dist) / TREE_CELL)), cz1 = MIN((i32)vg->cells_z - 1, (i32)floor((cam_local[2] + draw_dist) / TREE_CELL));
    for (i32 cz = cz0; cz <= cz1; cz++)
        for (i32 cx = cx0; cx <= cx1; cx++) {
            const TreeCell* cell = &vg->cells[cz * vg->cells_x + cx];
            if (!cell->count) continue;
            f32 ox = (f32)(cx * TREE_CELL - origin[0]), oz = (f32)(cz * TREE_CELL - origin[2]);
            v3 lo = v3_make(ox - pad, cell->ymin + sel->origin_terrain.y, oz - pad);
            v3 hi = v3_make(ox + (f32)TREE_CELL + pad, cell->ymax + sel->origin_terrain.y, oz + (f32)TREE_CELL + pad);
            f32 dx = MAX(MAX(lo.x - sel->cam.x, 0.0f), sel->cam.x - hi.x), dz = MAX(MAX(lo.z - sel->cam.z, 0.0f), sel->cam.z - hi.z);
            if (dx * dx + dz * dz > draw_dist * draw_dist || !aabb_in_frustum(sel, lo, hi)) continue;
            for (u32 k = 0; k < cell->count && vg->chunk_count < MAX_TREE_CHUNKS; k += TREE_CHUNK)
                out[vg->chunk_count++] = {cell->first + k, MIN((u32)TREE_CHUNK, cell->count - k), ox, oz};
            vg->tree_candidates += cell->count;
        }
}
