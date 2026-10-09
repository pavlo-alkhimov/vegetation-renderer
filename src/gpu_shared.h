// GPU-visible structs, included by C++ and Slang (docs/01: one definition of every GPU struct).
// Only 4-byte scalars and 16-byte vectors, so std430 and scalar layout agree.
#pragma once

#ifdef __SLANG__
typedef uint     u32;
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

// Camera-space convention: x east, y up, z north (left-handed), all positions relative to the render origin
// (docs/02). Projection: reversed-Z, infinite far plane.
struct FrameConstants {
    v4 cam_pos;             // xyz, w = near plane (m)
    v4 cam_right;           // xyz, w = proj_x = 1 / (tan(fov_y / 2) * aspect)
    v4 cam_up;              // xyz, w = proj_y = 1 / tan(fov_y / 2)
    v4 cam_fwd;             // xyz, w = time (s)
    v4 sun_dir;             // xyz towards the sun, w = fog density (1/m)
    v4 terrain;             // x, z of sample (0,0) relative to render origin; z = spacing (m); w = height_min - origin.y
    v4 terrain_size;        // x = width, y = height (samples), z = height_scale * 65535 (m per unorm unit), w = unused
    v4 morph[TERRAIN_MAX_LEVELS];   // per level: x = morph start (m), y = 1 / (end - start)
    u32 height_tex;         // bindless index
    u32 debug_mode;         // DEBUG_*
    u32 pad0, pad1;
};

struct PushConstants {
    GPU_PTR(FrameConstants) frame;
    GPU_PTR(TerrainNode) nodes;
};
