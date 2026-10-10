// Cooked ground-cover cover (.vrp), written by cook_ground_cover from Poly Haven glTF assets, read by the viewer.
//
// Layout: GcFileHeader, GcFileSpecies[species_count], GcFileVariant[variant_count],
// TreeVertex[vertex_count] (gpu_shared.h; uv as 2 x unorm16 mapping [-4, 4], attr = 0 | ao << 8 | sway << 16 |
// card seed << 24),
// u32 indices[index_count] (into the vertex array), then per species two RGBA8 textures of tex_size^2 with
// tex_mips levels each, mip 0 first: albedo (sRGB rgb + linear alpha, alpha-coverage preserving mips) and
// surface (r = normal x, g = normal y, OpenGL convention, b = ambient occlusion, a = roughness).
#pragma once

#define GC_FILE_MAGIC   0x31505256u   // "VRP1"
#define GC_FILE_VERSION 1u
#define GC_NAME          48            // GC_LODS (gpu_shared.h): all cards, ~50 %, ~20 % (kept cards scaled up)

typedef struct {
    u32 magic, version;
    u32 species_count, variant_count;
    u32 vertex_count, index_count;
    u32 tex_size, tex_mips;
} GcFileHeader;         // 32 B

typedef struct {
    char name[GC_NAME];  // Poly Haven asset id
    u32 first_variant, variant_count;
    u32 has_alpha;          // 0: opaque geometry
    u32 pad;
} GcFileSpecies;        // 64 B

typedef struct {
    char name[GC_NAME];  // glTF node name
    u32 species;
    f32 height;             // m, base at y = 0
    f32 radius;             // m, horizontal extent from the origin
    u32 pad;
    u32 first_index[GC_LODS], index_count[GC_LODS];
} GcFileVariant;        // 88 B

static_assert(sizeof(GcFileHeader) == 32 && sizeof(GcFileSpecies) == 64 && sizeof(GcFileVariant) == 88,
              "cover file struct size");
