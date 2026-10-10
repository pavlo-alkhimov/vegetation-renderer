// cook_ground_cover: Poly Haven plant assets (glTF + JPG maps, tools/fetch-polyhaven.sh) -> one cooked file (.vgc,
// see ground_cover_file.h).
//
//   cook_ground_cover [-o out.vrp] [--size N] <asset directory>...     e.g. data/external/polyhaven/*
//
// Every glTF node with a mesh becomes a variant, moved to its own origin (Poly Haven lays variants out along x).
// LODs remove whole cards (connected components) and scale the kept ones up about their root to keep the
// coverage. Textures: diffuse + alpha -> albedo, normal + AO/roughness -> surface; mips preserve the alpha-test
// coverage so cards do not thin out with distance.
#include "../base.h"
#include "../gpu_shared.h"
#include "../ground_cover_file.h"

#define CGLTF_IMPLEMENTATION
#include "../../third_party/cgltf.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include "../../third_party/stb_image.h"

#define MAX_SPECIES  16
#define MAX_VARIANTS 256
#define ALPHA_CUTOFF 0.5f

typedef struct { TreeVertex* v; u32 n, cap; } VertexArray;
typedef struct { u32* v; u32 n, cap; } IndexArray;

static VertexArray g_vertices;
static IndexArray g_indices;
static GcFileSpecies g_species[MAX_SPECIES];
static GcFileVariant g_variants[MAX_VARIANTS];
static u32 g_species_count, g_variant_count;
static u8* g_textures[MAX_SPECIES][2];      // all mips, RGBA8

static void push_vertex(TreeVertex v)
{
    if (g_vertices.n == g_vertices.cap) { g_vertices.cap = MAX(g_vertices.cap * 2, 65536u); g_vertices.v = (TreeVertex*)realloc(g_vertices.v, g_vertices.cap * sizeof(TreeVertex)); }
    g_vertices.v[g_vertices.n++] = v;
}

static void push_index(u32 i)
{
    if (g_indices.n == g_indices.cap) { g_indices.cap = MAX(g_indices.cap * 2, 65536u); g_indices.v = (u32*)realloc(g_indices.v, g_indices.cap * sizeof(u32)); }
    g_indices.v[g_indices.n++] = i;
}

static u32 hash_u32(u32 x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

static u32 pack_snorm10(v3 n)
{
    i32 x = (i32)lrintf(CLAMP(n.x, -1.0f, 1.0f) * 511.0f);
    i32 y = (i32)lrintf(CLAMP(n.y, -1.0f, 1.0f) * 511.0f);
    i32 z = (i32)lrintf(CLAMP(n.z, -1.0f, 1.0f) * 511.0f);
    return ((u32)x & 1023u) | ((u32)y & 1023u) << 10 | ((u32)z & 1023u) << 20;
}

static u32 uf_find(u32* parent, u32 x)
{
    while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
    return x;
}

// ---------------------------------------------------------------------------------------------------------------
// Geometry

typedef struct {
    v3* p; v3* n; f32* uv;      // uv: 2 per vertex
    u32 vn;
    u32* idx; u32 in;
} RawMesh;

static void read_node_mesh(const cgltf_node* node, RawMesh* m)
{
    f32 w[16];
    cgltf_node_transform_world(node, w);
    v3 origin = v3_make(w[12], 0, w[14]);      // variants are laid out in x/z; keep the authored ground height
    for (cgltf_size pi = 0; pi < node->mesh->primitives_count; pi++) {
        const cgltf_primitive* prim = &node->mesh->primitives[pi];
        if (prim->type != cgltf_primitive_type_triangles) continue;
        const cgltf_accessor *pos = NULL, *nrm = NULL, *tex = NULL;
        for (cgltf_size a = 0; a < prim->attributes_count; a++) {
            const cgltf_attribute* at = &prim->attributes[a];
            if (at->type == cgltf_attribute_type_position) pos = at->data;
            else if (at->type == cgltf_attribute_type_normal) nrm = at->data;
            else if (at->type == cgltf_attribute_type_texcoord && at->index == 0) tex = at->data;
        }
        if (!pos || !nrm || !tex || !prim->indices) FATAL("%s: primitive without position/normal/uv/indices", node->name);
        u32 base = m->vn, count = (u32)pos->count;
        m->p = (v3*)realloc(m->p, (m->vn + count) * sizeof(v3));
        m->n = (v3*)realloc(m->n, (m->vn + count) * sizeof(v3));
        m->uv = (f32*)realloc(m->uv, (m->vn + count) * 2 * sizeof(f32));
        for (u32 i = 0; i < count; i++) {
            f32 p[3], n[3], t[2];
            cgltf_accessor_read_float(pos, i, p, 3);
            cgltf_accessor_read_float(nrm, i, n, 3);
            cgltf_accessor_read_float(tex, i, t, 2);
            v3 wp = v3_make(w[0] * p[0] + w[4] * p[1] + w[8] * p[2] + w[12], w[1] * p[0] + w[5] * p[1] + w[9] * p[2] + w[13],
                            w[2] * p[0] + w[6] * p[1] + w[10] * p[2] + w[14]);
            v3 wn = v3_make(w[0] * n[0] + w[4] * n[1] + w[8] * n[2], w[1] * n[0] + w[5] * n[1] + w[9] * n[2], w[2] * n[0] + w[6] * n[1] + w[10] * n[2]);
            m->p[base + i] = v3_sub(wp, origin);
            m->n[base + i] = v3_norm(wn);
            m->uv[2 * (base + i)] = t[0];
            m->uv[2 * (base + i) + 1] = t[1];
        }
        m->vn += count;
        u32 ic = (u32)prim->indices->count;
        m->idx = (u32*)realloc(m->idx, (m->in + ic) * sizeof(u32));
        for (u32 i = 0; i < ic; i++) m->idx[m->in + i] = base + (u32)cgltf_accessor_read_index(prim->indices, i);
        m->in += ic;
    }
}

static void add_variant(u32 species, const char* name, const RawMesh* m, bool info)
{
    if (g_variant_count == MAX_VARIANTS) FATAL("too many variants");
    GcFileVariant* var = &g_variants[g_variant_count++];
    memset(var, 0, sizeof(*var));
    snprintf(var->name, sizeof(var->name), "%s", name);
    var->species = species;

    f32 ymin = 1e30f, ymax = -1e30f, radius = 0, umin = 1e30f, umax = -1e30f, vmin = 1e30f, vmax = -1e30f;
    for (u32 i = 0; i < m->vn; i++) {
        ymin = MIN(ymin, m->p[i].y); ymax = MAX(ymax, m->p[i].y);
        radius = MAX(radius, sqrtf(m->p[i].x * m->p[i].x + m->p[i].z * m->p[i].z));
        umin = MIN(umin, m->uv[2 * i]); umax = MAX(umax, m->uv[2 * i]);
        vmin = MIN(vmin, m->uv[2 * i + 1]); vmax = MAX(vmax, m->uv[2 * i + 1]);
    }
    var->height = ymax;
    var->radius = radius;

    // Cards = connected components; area and root (lowest vertex) per card.
    u32* parent = (u32*)malloc(m->vn * sizeof(u32));
    for (u32 i = 0; i < m->vn; i++) parent[i] = i;
    for (u32 t = 0; t < m->in; t += 3) {
        u32 a = uf_find(parent, m->idx[t]);
        for (u32 k = 1; k < 3; k++) { u32 b = uf_find(parent, m->idx[t + k]); if (a != b) parent[b] = a; }
    }
    u32* comp = (u32*)malloc(m->vn * sizeof(u32));     // vertex -> component index
    u32* root_of = (u32*)malloc(m->vn * sizeof(u32));  // representative -> component index
    u32 comps = 0;
    for (u32 i = 0; i < m->vn; i++) root_of[i] = ~0u;
    for (u32 i = 0; i < m->vn; i++) {
        u32 r = uf_find(parent, i);
        if (root_of[r] == ~0u) root_of[r] = comps++;
        comp[i] = root_of[r];
    }
    f32* area = (f32*)calloc(comps, sizeof(f32));
    v3* root = (v3*)malloc(comps * sizeof(v3));
    for (u32 c = 0; c < comps; c++) root[c] = v3_make(0, 1e30f, 0);
    for (u32 i = 0; i < m->vn; i++) if (m->p[i].y < root[comp[i]].y) root[comp[i]] = m->p[i];
    f32 total_area = 0;
    for (u32 t = 0; t < m->in; t += 3) {
        v3 a = m->p[m->idx[t]], b = m->p[m->idx[t + 1]], c = m->p[m->idx[t + 2]];
        f32 ar = 0.5f * sqrtf(v3_dot(v3_cross(v3_sub(b, a), v3_sub(c, a)), v3_cross(v3_sub(b, a), v3_sub(c, a))));
        area[comp[m->idx[t]]] += ar;
        total_area += ar;
    }
    u32 largest = 0;
    for (u32 c = 1; c < comps; c++) if (area[c] > area[largest]) largest = c;

    static const f32 KEEP[GC_LODS] = {1.0f, 0.4f, 0.12f};
    u32 tris[GC_LODS] = {};
    for (u32 lod = 0; lod < GC_LODS; lod++) {
        bool* keep = (bool*)malloc(comps * sizeof(bool));
        f32 kept_area = 0;
        for (u32 c = 0; c < comps; c++) {
            keep[c] = c == largest || (hash_u32(c * 2654435761u + 0x51ed27u) >> 8) * (1.0f / 16777216.0f) < KEEP[lod];
            if (keep[c]) kept_area += area[c];
        }
        f32 s = MIN(sqrtf(total_area / MAX(kept_area, 1e-9f)), 1.6f);   // uniform scale about each card's root
        u32 base = g_vertices.n;
        for (u32 i = 0; i < m->vn; i++) {
            v3 p = m->p[i];
            if (keep[comp[i]] && s != 1.0f) p = v3_add(root[comp[i]], v3_scale(v3_sub(p, root[comp[i]]), s));
            f32 sway = powf(CLAMP(p.y / MAX(ymax, 1e-3f), 0.0f, 1.0f), 1.5f);
            TreeVertex v;
            v.x = p.x; v.y = p.y; v.z = p.z;
            v.normal = pack_snorm10(m->n[i]);
            u32 uu = (u32)lrintf(CLAMP((m->uv[2 * i] + 4.0f) / 8.0f, 0.0f, 1.0f) * 65535.0f);    // some assets tile
            u32 vv = (u32)lrintf(CLAMP((m->uv[2 * i + 1] + 4.0f) / 8.0f, 0.0f, 1.0f) * 65535.0f);
            v.uv = uu | vv << 16;
            v.attr = 255u << 8 | (u32)lrintf(sway * 255.0f) << 16 | (hash_u32(comp[i] + 7u) & 255u) << 24;
            push_vertex(v);
        }
        // Each LOD gets its own vertex copy (positions differ); unreferenced vertices are dropped by the viewer's
        // meshletizer.
        var->first_index[lod] = g_indices.n;
        for (u32 t = 0; t < m->in; t += 3) {
            if (!keep[comp[m->idx[t]]]) continue;
            for (u32 k = 0; k < 3; k++) push_index(base + m->idx[t + k]);
            tris[lod]++;
        }
        var->index_count[lod] = g_indices.n - var->first_index[lod];
        free(keep);
    }
    if (info)
        printf("  %-36s h %.2f r %.2f ybase %+.3f  cards %4u  tris %6u %6u %6u  uv [%.2f %.2f]x[%.2f %.2f]\n", name, ymax, radius,
               ymin, comps, tris[0], tris[1], tris[2], umin, umax, vmin, vmax);
    free(parent); free(comp); free(root_of); free(area); free(root);
}

// ---------------------------------------------------------------------------------------------------------------
// Textures

static f32 g_srgb_to_linear[256];

static u8 linear_to_srgb(f32 c)
{
    c = CLAMP(c, 0.0f, 1.0f);
    f32 s = c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
    return (u8)lrintf(s * 255.0f);
}

static u8* load_image(const char* path, int channels, u32 size, bool required)
{
    int w, h, n;
    u8* p = stbi_load(path, &w, &h, &n, channels);
    if (!p) {
        if (required) FATAL("cannot read %s: %s", path, stbi_failure_reason());
        return NULL;
    }
    if ((u32)w == size && (u32)h == size) return p;
    u8* r = (u8*)malloc((size_t)size * size * channels);           // nearest resample (sizes are powers of two)
    for (u32 y = 0; y < size; y++)
        for (u32 x = 0; x < size; x++)
            memcpy(r + ((size_t)y * size + x) * channels, p + ((size_t)(y * h / size) * w + x * w / size) * channels, channels);
    stbi_image_free(p);
    return r;
}

static f32 coverage(const u8* rgba, u32 size, f32 scale)
{
    u64 n = 0;
    for (size_t i = 0; i < (size_t)size * size; i++) n += rgba[4 * i + 3] * scale >= ALPHA_CUTOFF * 255.0f;
    return (f32)n / ((f32)size * size);
}

// Builds the mip chain in place after mip 0. albedo: alpha-weighted sRGB-correct colour, alpha scaled per mip to
// keep the alpha-test coverage of mip 0 (Castaño, "Computing alpha mipmaps").
static void build_mips(u8* tex, u32 size, u32 mips, bool albedo)
{
    f32 cov0 = albedo ? coverage(tex, size, 1.0f) : 0;
    u8* src = tex;
    for (u32 level = 1; level < mips; level++) {
        u32 s = size >> level;
        u8* dst = src + (size_t)(s * 2) * (s * 2) * 4;
        for (u32 y = 0; y < s; y++)
            for (u32 x = 0; x < s; x++) {
                f32 acc[4] = {}, wsum = 0;
                for (u32 k = 0; k < 4; k++) {
                    const u8* p = src + ((size_t)(2 * y + (k >> 1)) * (s * 2) + 2 * x + (k & 1)) * 4;
                    if (albedo) {
                        f32 a = p[3] / 255.0f + 1e-4f;
                        for (u32 c = 0; c < 3; c++) acc[c] += g_srgb_to_linear[p[c]] * a;
                        acc[3] += p[3];
                        wsum += a;
                    } else {
                        for (u32 c = 0; c < 4; c++) acc[c] += p[c];
                    }
                }
                u8* o = dst + ((size_t)y * s + x) * 4;
                if (albedo) {
                    for (u32 c = 0; c < 3; c++) o[c] = linear_to_srgb(acc[c] / wsum);
                    o[3] = (u8)lrintf(acc[3] / 4.0f);
                } else {
                    for (u32 c = 0; c < 4; c++) o[c] = (u8)lrintf(acc[c] / 4.0f);
                }
            }
        if (albedo && cov0 > 0 && cov0 < 1) {
            f32 lo = 0.0f, hi = 8.0f;
            for (u32 it = 0; it < 16; it++) { f32 mid = 0.5f * (lo + hi); if (coverage(dst, s, mid) < cov0) lo = mid; else hi = mid; }
            for (size_t i = 0; i < (size_t)s * s; i++) dst[4 * i + 3] = (u8)MIN(255.0f, dst[4 * i + 3] * hi + 0.5f);
        }
        src = dst;
    }
}

static size_t mip_chain_bytes(u32 size, u32 mips)
{
    size_t n = 0;
    for (u32 l = 0; l < mips; l++) n += (size_t)(size >> l) * (size >> l) * 4;
    return n;
}

static void cook_textures(u32 sp, const char* dir, const char* asset, const char* res, u32 size, u32 mips)
{
    char path[1024];
    size_t bytes = mip_chain_bytes(size, mips);
    u8* albedo = (u8*)malloc(bytes);
    u8* surface = (u8*)malloc(bytes);
    snprintf(path, sizeof(path), "%s/textures/%s_diff_%s.jpg", dir, asset, res);
    u8* diff = load_image(path, 3, size, true);
    snprintf(path, sizeof(path), "%s/textures/%s_alpha_%s.jpg", dir, asset, res);
    u8* alpha = load_image(path, 1, size, false);
    snprintf(path, sizeof(path), "%s/textures/%s_nor_gl_%s.jpg", dir, asset, res);
    u8* nor = load_image(path, 3, size, true);
    snprintf(path, sizeof(path), "%s/textures/%s_arm_%s.jpg", dir, asset, res);
    u8* arm = load_image(path, 3, size, true);
    for (size_t i = 0; i < (size_t)size * size; i++) {
        albedo[4 * i + 0] = diff[3 * i + 0];
        albedo[4 * i + 1] = diff[3 * i + 1];
        albedo[4 * i + 2] = diff[3 * i + 2];
        albedo[4 * i + 3] = alpha ? alpha[i] : 255;
        surface[4 * i + 0] = nor[3 * i + 0];
        surface[4 * i + 1] = nor[3 * i + 1];
        surface[4 * i + 2] = arm[3 * i + 0];   // AO
        surface[4 * i + 3] = arm[3 * i + 1];   // roughness
    }
    build_mips(albedo, size, mips, true);
    build_mips(surface, size, mips, false);
    g_textures[sp][0] = albedo;
    g_textures[sp][1] = surface;
    g_species[sp].has_alpha = alpha != NULL;
    stbi_image_free(diff); stbi_image_free(nor); stbi_image_free(arm);
    if (alpha) stbi_image_free(alpha);
}

// ---------------------------------------------------------------------------------------------------------------

static const char* path_basename(const char* p)
{
    const char* b = p;
    for (const char* c = p; *c; c++) if (*c == '/' || *c == '\\') b = c + 1;
    return b;
}

int main(int argc, char** argv)
{
    const char* out_path = "data/cooked/ground_cover.vgc";
    u32 size = 2048;
    bool info = true;
    const char* dirs[MAX_SPECIES];
    u32 dir_count = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out_path = argv[++i];
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) size = (u32)atoi(argv[++i]);
        else if (argv[i][0] == '-') FATAL("usage: cook_ground_cover [-o out.vrp] [--size N] <asset directory>...");
        else if (dir_count < MAX_SPECIES) dirs[dir_count++] = argv[i];
    }
    if (!dir_count) FATAL("usage: cook_ground_cover [-o out.vrp] [--size N] <asset directory>...");
    if (size & (size - 1)) FATAL("--size must be a power of two");
    u32 mips = 0;
    while ((size >> mips) >= 1) mips++;
    for (u32 i = 0; i < 256; i++) { f32 c = i / 255.0f; g_srgb_to_linear[i] = c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }

    for (u32 d = 0; d < dir_count; d++) {
        char dir[1024];
        snprintf(dir, sizeof(dir), "%s", dirs[d]);
        size_t dl = strlen(dir);
        while (dl && (dir[dl - 1] == '/' || dir[dl - 1] == '\\')) dir[--dl] = 0;
        const char* asset = path_basename(dir);
        const char* res = NULL;
        char gltf_path[1024];
        static const char* RES[] = {"2k", "4k", "1k", "8k"};
        for (u32 r = 0; r < ARRAY_COUNT(RES) && !res; r++) {
            snprintf(gltf_path, sizeof(gltf_path), "%s/%s_%s.gltf", dir, asset, RES[r]);
            FILE* f = fopen(gltf_path, "rb");
            if (f) { fclose(f); res = RES[r]; }
        }
        if (!res) { fprintf(stderr, "skipping %s: no %s_<res>.gltf\n", dir, asset); continue; }

        cgltf_options opt = {};
        cgltf_data* data = NULL;
        if (cgltf_parse_file(&opt, gltf_path, &data) != cgltf_result_success) FATAL("cannot parse %s", gltf_path);
        if (cgltf_load_buffers(&opt, data, gltf_path) != cgltf_result_success) FATAL("cannot load buffers of %s", gltf_path);

        u32 sp = g_species_count++;
        GcFileSpecies* s = &g_species[sp];
        snprintf(s->name, sizeof(s->name), "%s", asset);
        s->first_variant = g_variant_count;
        printf("%s (%s)\n", asset, res);
        for (cgltf_size n = 0; n < data->nodes_count; n++) {
            const cgltf_node* node = &data->nodes[n];
            if (!node->mesh) continue;
            const char* name = node->name ? node->name : "unnamed";
            const char* lod = strstr(name, "_LOD");
            if (lod && strcmp(lod, "_LOD0")) continue;          // authored LODs: take LOD0 only
            RawMesh m = {};
            read_node_mesh(node, &m);
            if (m.in) add_variant(sp, name, &m, info);
            free(m.p); free(m.n); free(m.uv); free(m.idx);
        }
        s->variant_count = g_variant_count - s->first_variant;
        cgltf_free(data);
        cook_textures(sp, dir, asset, res, size, mips);
        printf("  %u variants, alpha %s\n", s->variant_count, s->has_alpha ? "yes" : "no (opaque)");
    }

    FILE* f = fopen(out_path, "wb");
    if (!f) FATAL("cannot write %s", out_path);
    GcFileHeader h = {GC_FILE_MAGIC, GC_FILE_VERSION, g_species_count, g_variant_count, g_vertices.n, g_indices.n, size, mips};
    fwrite(&h, sizeof(h), 1, f);
    fwrite(g_species, sizeof(GcFileSpecies), g_species_count, f);
    fwrite(g_variants, sizeof(GcFileVariant), g_variant_count, f);
    fwrite(g_vertices.v, sizeof(TreeVertex), g_vertices.n, f);
    fwrite(g_indices.v, sizeof(u32), g_indices.n, f);
    size_t tex_bytes = mip_chain_bytes(size, mips);
    for (u32 sp = 0; sp < g_species_count; sp++) for (u32 k = 0; k < 2; k++) fwrite(g_textures[sp][k], 1, tex_bytes, f);
    fclose(f);
    printf("wrote %s: %u species, %u variants, %u vertices, %u triangles, textures %u x %u x %u layers (%.0f MB)\n", out_path,
           g_species_count, g_variant_count, g_vertices.n, g_indices.n / 3, size, size, g_species_count * 2,
           tex_bytes * g_species_count * 2 / 1048576.0);
    return 0;
}
