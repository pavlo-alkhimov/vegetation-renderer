// GPU-visible structs, included by C++ and Slang (docs/01: one definition of every GPU struct).
// Only 4-byte scalars and 16-byte vectors, so std430 and scalar layout agree.
#pragma once

#ifdef __SLANG__
typedef uint     u32;
typedef int      i32;
typedef float    f32;
typedef uint64_t u64;
typedef float4   v4;
#define GPU_PTR(T) T*
#else
typedef struct { f32 x, y, z, w; } v4;
#define GPU_PTR(T) u64
#endif

// Terrain patch: TERRAIN_PATCH_QUADS^2 quads, i.e. 128 triangles (docs/12).
#define TERRAIN_PATCH_QUADS 8
#define TERRAIN_MAX_LEVELS  16

#define DEBUG_SHADED  0
#define DEBUG_LOD     1
#define DEBUG_CONTOUR 2
#define DEBUG_NORMALS 3

struct TerrainNode {
    u32 gx, gz;             // grid sample of the patch's min corner (x east, z north)
    u32 level;              // vertex step = 1 << level samples
    u32 pad;
};

// Bindless texture slots.
#define TEX_HEIGHT 0
#define TEX_MASK   1        // RG8: r = forest (canopy) density, g = grass density; covers the terrain extent
#define TEX_PLANTS 2        // per plant species s: TEX_PLANTS + 2s albedo (sRGB + alpha), + 2s + 1 surface (plants_file.h)
#define TEX_SCENE   64      // frame targets (recreated with the swapchain): scene colour (tonemapped, RGBA16F),
#define TEX_MOTION  65      // motion (uv of this frame - uv of the previous frame, RG16F),
#define TEX_DEPTH   66      // depth (D32, reversed Z),
#define TEX_HISTORY 67      // TAA history, 2 images (ping-pong): TEX_HISTORY + 0/1

// Samplers (bindless sampler array).
#define SAMPLER_LINEAR_CLAMP 0
#define SAMPLER_ANISO_REPEAT 1  // trilinear + anisotropic, wrapping (plant atlases)
#define SAMPLER_COUNT        2

// Vegetation baseline (docs/06 §5, "baseline" section): trees as meshlets via task + mesh shaders, grass blades
// generated per frame in mesh shaders.
#define TREE_LODS            4
#define TREE_MESHLET_VERTS   64
#define TREE_MESHLET_TRIS    124
#define TREE_CHUNK           32     // instances per task workgroup
#define TREE_CELL            256.0  // placement cell = render-origin cell (docs/02)
#define GRASS_TILE           4.0    // m
#define GRASS_GROUP_BLADES   16     // blades per mesh workgroup
#define GRASS_TILE_BLADES    3200   // blades per tile at full density and full LOD (200 per m²)

#define MAT_BARK      0
#define MAT_LEAF      1             // broadleaf card: a few procedural leaves
#define MAT_NEEDLE    2             // conifer spray card
#define MAT_CLUSTER   3             // LOD2 crown card: blob of foliage
#define MAT_BILLBOARD 4             // LOD3 crossed quads: whole-tree silhouette

#define VEG_TREES 1u                // FrameConstants.veg_flags
#define VEG_GRASS 2u

struct TreeVertex {                 // 24 B, model space (metres, y up, origin at the stem base)
    f32 x, y, z;
    u32 normal;                     // snorm 10:10:10
    u32 uv;                         // 2 x u16, value / 1024
    u32 attr;                       // material 0-7 | ao 8-15 | sway weight 16-23 | card seed 24-31
};

struct TreeMeshlet {                // ≤ TREE_MESHLET_VERTS vertices, ≤ TREE_MESHLET_TRIS triangles
    u32 vertex_offset;              // into the vertex array
    u32 triangle_offset;            // into the triangle array (u32 = 3 x u8 local indices)
    u32 vertex_count;
    u32 triangle_count;
};

struct TreeType {                   // one species variant
    u32 meshlet_first[TREE_LODS];
    u32 meshlet_count[TREE_LODS];
    v4 leaf_color;                  // rgb albedo, w unused
    v4 bark_color;                  // rgb albedo, w = height (m)
    v4 bounds;                      // xyz = sphere centre (model space), w = radius
    v4 shape;                       // x = crown base / height, y = 1 conifer, z = crown radius / height,
                                    // w = trunk radius / crown radius
};

struct TreeInstance {               // 16 B
    f32 x, y, z;                    // x, z relative to the cell corner; y above the terrain's height_min
    u32 packed;                     // type 0-7 | yaw 8-15 | scale 16-23 (0.6 + v / 255 * 0.8) | tint 24-31
};

struct TreeChunk {                  // ≤ TREE_CHUNK consecutive instances of one cell; one task workgroup
    u32 first;
    u32 count;
    f32 ox, oz;                     // cell corner relative to the render origin
};

// Near-field ground cover: Poly Haven plant assets (plants_file.h) as meshlets, placed every frame from the
// vegetation mask by hash (nothing stored). Two layers in one dispatch (SV_GroupID.z): dense small cells near the
// camera for species with a short draw distance (grasses, low forbs), coarse cells further out for the tall ones.
// One task workgroup per cell, PLANT_CELL_SLOTS candidates.
#define PLANT_LODS          3
#define PLANT_MAX_SPECIES   16
#define PLANT_CELL_SLOTS    32
#define PLANT_LAYERS        2
#define PLANT_NEAR_DISTANCE 16.0    // m; species drawn up to here belong to layer 0
static const float PLANT_LAYER_CELL[PLANT_LAYERS] = {0.6f, 2.0f};    // m

struct PlantVariant {               // 48 B
    u32 meshlet_first[PLANT_LODS];
    u32 meshlet_count[PLANT_LODS];
    u32 species;
    f32 height;                     // m at scale 1, base at y = 0
    f32 radius;                     // m, horizontal
    u32 pad[3];
};

struct PlantSpecies {               // 48 B
    u32 first_variant, variant_count;
    u32 albedo_tex, surface_tex;    // bindless indices
    v4 habitat;                     // plants per m²: x meadow, y forest edge, z forest floor;
                                    // w = flags as float: + 1 alpha-tested cards, + 2 follows the meadow height
    v4 shape;                       // x, y = scale range; z = draw distance (m); w = wind response (0 rigid .. 1)
};

struct PlantScene {                 // static, written once at load
    GPU_PTR(PlantVariant) variants;
    GPU_PTR(PlantSpecies) species;
    GPU_PTR(TreeMeshlet) meshlets;
    GPU_PTR(TreeVertex) vertices;   // uv = 2 x unorm16 mapping [-4, 4]
    GPU_PTR(u32) triangles;
    u32 species_count;
    u32 pad;
};

struct VegScene {                   // static, written once at load
    GPU_PTR(TreeInstance) instances;
    GPU_PTR(TreeType) types;
    GPU_PTR(TreeMeshlet) meshlets;
    GPU_PTR(TreeVertex) vertices;
    GPU_PTR(u32) triangles;
};

// Camera-space convention: x east, y up, z north (left-handed), all positions relative to the render origin
// (docs/02). Projection: reversed-Z, infinite far plane.

// One per rendered view (main camera; later shadow cascades). Culling and projection use the view; LOD, fog and
// shading use the main camera in FrameConstants, so every view sees the same geometry.
#define MAX_VIEWS 8
struct ViewConstants {
    v4 view_proj[4];        // rows: clip = (dot(r0, p), dot(r1, p), dot(r2, p), dot(r3, p)), p = (rel, 1); jittered
    v4 prev_view_proj[4];   // previous frame, expressed relative to the current render origin
    v4 planes[6];           // inward: xyz unit normal, w = d; inside if dot(n, p) + d >= 0; unused = (0, 0, 0, 1)
    v4 jitter;              // xy = sub-pixel jitter (NDC) contained in view_proj, zw = previous frame's
};

struct FrameConstants {
    v4 cam_pos;             // xyz, w = near plane (m)
    v4 cam_right;           // xyz, w = proj_x = 1 / (tan(fov_y / 2) * aspect)
    v4 cam_up;              // xyz, w = proj_y = 1 / tan(fov_y / 2)
    v4 cam_fwd;             // xyz, w = time (s)
    v4 sun_dir;             // xyz towards the sun, w = fog density (1/m)
    v4 terrain;             // x, z of sample (0,0) relative to render origin; z = spacing (m); w = height_min - origin.y
    v4 terrain_size;        // x = width, y = height (samples), z = height_scale * 65535 (m per unorm unit), w = unused
    v4 morph[TERRAIN_MAX_LEVELS];   // per level: x = morph start (m), y = 1 / (end - start)
    v4 veg;                 // x = tree draw distance (m), y = grass radius (m), z = pixels per unit of size/distance,
                            // w = grass density scale
    v4 tree_lod;            // projected tree height (px) above which LOD 0 / 1 / 2 is used; w = unused
    v4 wind;                // xz = wind direction (unit), y = strength, w = unused
    v4 grass;               // rgb = meadow grass albedo (linear; mean of the plant assets), w = blade density inside
                            // the plant radius (the plant assets carry the volume there)
    v4 screen;              // xy = render size (px), zw = 1 / size
    v4 taa;                 // x = previous frame's time (s, for motion vectors of wind), y = weight of the current
                            // frame in the TAA blend (1 = no history), z, w = unused
    u32 height_tex;         // bindless index
    u32 debug_mode;         // DEBUG_*
    u32 mask_tex;           // bindless index
    u32 veg_flags;          // VEG_*
    i32 grass_tile_x;       // terrain-local tile index of the grass dispatch grid's (0,0)
    i32 grass_tile_z;
    u32 grass_tiles;        // grass dispatch grid side, tiles
    u32 history_tex;        // bindless index of the TAA history read this frame
    i32 plant_cell_x[PLANT_LAYERS]; // per layer: terrain-local cell index of the dispatch grid's (0,0)
    i32 plant_cell_z[PLANT_LAYERS];
    u32 plant_cells[PLANT_LAYERS];  // per layer: grid side in cells (the dispatch covers the largest)
    u32 pad2, pad3;
};

// Overlay (top right): FPS, FPS graph, key list. One quad; text and graph are evaluated in the fragment shader.
#define OVERLAY_GRAPH_COLS 160
#define OVERLAY_TEXT_COLS  28
#define OVERLAY_TEXT_LINES 24
#define OVERLAY_GLYPHS     96       // ASCII 32..127, 5 x 7 bits: bit (row * 5 + col), row 0 = top, col 0 = left

struct OverlayData {
    v4 screen;                      // xy = framebuffer size (px)
    v4 panel;                       // x, y, w, h (px)
    v4 graph;                       // x, y, w, h (px)
    v4 text;                        // xy = first character cell (px), z = cell width, w = cell height
    v4 params;                      // x = font texel size (px), y = graph full-scale FPS, zw = reference lines (FPS)
    f32 fps[OVERLAY_GRAPH_COLS];    // oldest to newest; 0 = no data
    u32 chars[OVERLAY_TEXT_LINES * OVERLAY_TEXT_COLS / 4];   // 1 byte per cell, bit 7 = highlight
    u32 font[OVERLAY_GLYPHS * 2];
};

struct PushConstants {
    GPU_PTR(FrameConstants) frame;
    GPU_PTR(TerrainNode) nodes;
    GPU_PTR(VegScene) veg;
    GPU_PTR(TreeChunk) chunks;
    GPU_PTR(OverlayData) overlay;
    GPU_PTR(ViewConstants) view;
    GPU_PTR(PlantScene) plants;
};
