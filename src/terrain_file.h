// Cooked heightfield file (.vrh), written by cook_terrain, read by the viewer.
//
// Layout: TerrainFileHeader, then width * height u16 samples, row-major, row 0 = south, column 0 = west.
// Height of sample s in metres: height_min + s * height_scale. Sample (0,0) lies at (origin_e, origin_n) in the
// projected CRS `epsg` (EPSG:25832 for Bavaria). Interim format until the cell-based cooker of docs/04 exists.
#pragma once

#define TERRAIN_FILE_MAGIC   0x31485256u   // "VRH1"
#define TERRAIN_FILE_VERSION 1u

typedef struct {
    u32 magic;
    u32 version;
    u32 width, height;      // samples
    f32 spacing;            // metres between samples
    f32 height_min;         // metres
    f32 height_scale;       // metres per u16 step
    u32 epsg;
    f64 origin_e, origin_n; // projected coordinates of sample (0,0)
    u32 holes_filled;       // samples that had no source data (missing tiles, nodata)
    u32 pad[3];
} TerrainFileHeader;        // 64 B
static_assert(sizeof(TerrainFileHeader) == 64, "TerrainFileHeader size");
