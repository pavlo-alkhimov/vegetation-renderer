# 04 — Data representation: DCC → cooked → GPU

## Pipeline

```
 DCC / tools                 SOURCE (git + LFS)              COOKER (offline, incremental)        RUNTIME
 -----------                 ------------------              -----------------------------        -------
 Blender, Houdini,      -->  *.glb   meshes, skins,     -->  meshes → clusters + LOD DAG     -->  *.pak files
 SpeedTree (via glTF/         instanced parts                 textures → BCn mip chains            (per cell / per
 USD conversion),            *.exr/*.png/*.tif textures       species → parts, wind rig,            asset group)
 Substance, photogrammetry   *.species / *.mat / *.grass      merged mesh, far field, RT proxy      |
 Gaea / World Machine /       (text: rig params, LOD,         terrain → height tiles, layers,       | mmap / io_uring
 own placement tools          seasons, materials)             density maps (BC4)                    v
                             heightmaps (16-bit), masks      cells → tree/prop instance lists     staging → GPU pools
                             *.meta import settings (text)   content hash → blob cache            CPU headers → arenas
```

Principles:
- **Source formats are open and inspectable.** The only engine-specific binary format is the cooked output.
- **Cooked = exactly the runtime memory image.** No parsing at load: pointer-free (offsets), 16-byte-aligned
  sections, pack entries 4 KB-aligned for direct I/O. Loading = read → (decompress) → copy to GPU.
- **Content-addressed cache:** blob key = hash(source bytes, import settings, cooker version). Incremental and
  parallel; target: seconds per asset, minutes for the whole world.
- **One decision per format.** Fixed-size records and fixed formats first; variable-bit-rate compression only when
  measurements say geometry memory or disk size is the bottleneck.

### Why glTF as the interchange format

Open spec, native Blender export, `EXT_mesh_gpu_instancing` and node instancing for assemblies, skins for wind rigs,
`extras` for custom data. USD is richer (layers, variants; UE's Dynamic Wind defines a USD schema for wind bones) but
heavier — accepted later via conversion if needed. FBX is avoided (proprietary SDK).

### Vegetation source convention (one glTF per species variant)

| glTF element | Meaning |
|---|---|
| skin joints | wind rig: trunk → branches → sub-branches; `extras`: stiffness, frequency, damping, group |
| mesh `wood` (skinned) | trunk + main branches — the only unique geometry |
| meshes `part:*` | twigs, leaf clusters, needle sprays, cones, flowers |
| nodes referencing `part:*` meshes, parented to joints | assembly placement (→ `PartInstance`); thousands of nodes share a dozen part meshes |
| per-leaf connected components | leaves/needles are geometry (no alpha cards); the cooker assigns each leaf a random bucket for seasonal thinning |
| `*.species` text side-car | LOD thresholds, season colour ramps, material mapping, RT proxy settings, far-field settings |

Cooker outputs per species: part meshes with cluster DAGs, `wood` DAG (partitioned per bone and rig level), merged
whole-tree DAG (parts baked in, rig reduced, only the levels coarser than the parts → merged switch), far-field
representation (impostor atlas, later voxel mips), RT proxy, wind rig.

## Pack files

```c
typedef struct { u32 magic; u32 version; u32 entry_count; u32 pad; u64 toc_offset; u64 data_offset; } PakHeader; // 32 B
typedef struct {                                                                                                  // 32 B
    u64 key;            // content hash (or asset id for named roots)
    u64 offset;         // 4 KB aligned
    u32 stored_size;
    u32 raw_size;
    u16 codec;          // NONE | ZSTD (CPU) | ZSTD_GPU (later: GPU decompression, as DirectStorage 1.4 does)
    u16 type;           // MESH_PAGE | TEXTURE_MIPS | CELL | SPECIES | ...
    u32 flags;
} PakEntry;
```
TOC sorted by key (binary search). Entries of one world cell are stored contiguously.

## World cells

256 m × 256 m terrain columns (`WorldPos` uses 3D cells of 256 m so altitude never loses precision). Every cell blob
starts with a header:

```c
typedef struct {                // 64 B
    i32 cell[2];                // column coordinates (256 m)
    f32 base_altitude;          // metres; tree/prop heights in the cell are relative to it
    u32 tier;                   // FULL | FAR (which sections are present)
    u32 section_offset[8];      // height, layers, density, trees, props, canopy, quadtree errors, reserved
    u32 section_count;
    u32 pad[3];
} CellHeader;
```

**Residency tiers:** `FULL` cells (all sections) within ~5 km of the camera; `FAR` cells (canopy layer + coarse height
mips only) out to the horizon. One cell blob:

| Section | Format | Size (typ.) | Consumers |
|---|---|---|---|
| Height tile | 257×257 `u16` + mip pyramid + per-node min/max/error ([12](12-terrain.md)) | ~175 KB | renderer (terrain patches, grass), game (collision) |
| Terrain layer weights | 512×512 (0.5 m) RGBA8 | 1 MB → compressed | renderer (RVT compositing) |
| Ground-cover density maps | per type, 512×512 BC4 | 128 KB each | renderer (grass/plant generation), game (queries) |
| Tree instances | `TreeInstance[]` | 16 B/tree, ~40 KB in dense forest | renderer, game (collision, AI) |
| Prop instances | position/rotation/scale + mesh id | 48 B each | renderer, game |
| Canopy layer | 128×128 (2 m) RGBA8 colour + R8 height | 80 KB | renderer (farthest LOD) |

Dense temperate forest ≈ 1 tree / 25 m² → ~2600 trees per cell.

## Precision

- Absolute positions: `WorldPos { i32 cell[3]; f32 local[3]; }`. The GPU never sees absolute world coordinates: all
  GPU world-space data is relative to the **render origin** (the corner of the camera's cell, changing only on cell
  crossings — [02](02-architecture.md)), and a table stores each resident cell's offset from it in `f32`.
  Reason: at 8 km from the origin a `f32` has ~1 mm resolution, which at 1 m viewing distance is ~1 pixel — enough
  to make near geometry shimmer.
- Mesh positions: quantized to a mesh-wide grid (`pos_step`, e.g. 1/1024 m) → identical vertices at cluster
  boundaries across LOD levels → crack-free.

## Meshes: cluster LOD hierarchy

Built with meshoptimizer 1.x (`clusterlod.h`): cluster → group → simplify with locked group borders → re-cluster →
repeat to a single root (Nanite-style DAG). A BVH over cluster groups prunes culling and LOD tests.

- Cluster limits: **≤ 128 triangles, ≤ 128 vertices** (leaf quads need 2 vertices per triangle), one material per
  cluster, **one bone per cluster**.
- **Skinned (wind-rigged) meshes:** the cooker partitions the mesh by bone before clustering and builds the DAG per
  rig level — fine levels partitioned by the fine bones, coarser levels by the reduced rig; at the level where the rig
  coarsens (twig bones → branch bone) the groups are re-partitioned. Every cluster therefore has one rigid transform,
  and its bound can be posed for culling. Part meshes use bone 0 (rigid to `PartInstance.bone`).
- Geometry pages: **64 KB**, ~25 clusters each; unit of streaming and pool allocation. Root pages always resident.
  A DAG group never straddles pages (groups of 8–16 clusters at ≤ 2.6 KB each fit), so one residency check per
  hierarchy leaf suffices.

```c
typedef struct {                // 48 B, one per mesh, always resident
    f32 bounds[4];              // sphere, mesh space
    f32 uv_offset[2];           // uv = unorm16 * uv_scale + uv_offset
    f32 uv_scale[2];
    u32 root_node;              // first GpuHierarchyNode
    u32 root_page;              // always-resident page
    f32 pos_step;               // metres per position unit
    u32 flags;                  // FOLIAGE | TWO_SIDED | ALPHA_MASK | WIND_RIG
} GpuMeshHeader;

typedef struct {                // 64 B, BVH over cluster groups
    f32 bounds[4];              // culling sphere of the subtree
    f32 lod_bounds[4];          // sphere used to project LOD error
    f32 max_parent_error;       // projected <= threshold → whole subtree too fine → skip
    u32 first_child;            // node index, or first cluster (leaf)
    u32 child_count;            // | LEAF flag
    u32 page;                   // leaf: geometry page (residency check)
    u32 pad[4];
} GpuHierarchyNode;

typedef struct {                // 64 B, inside a geometry page
    f32 bounds[4];              // culling sphere, mesh space
    f32 lod_bounds[4];          // group sphere: all clusters of a group decide identically
    f32 lod_error;              // metres
    f32 parent_error;           // error of the group this cluster was simplified into; +inf at roots
    i32 pos_base[3];            // on the mesh-wide grid
    u16 vertex_offset;          // 16-byte units within the page
    u16 index_offset;           // 4-byte units within the page
    u8  vertex_count;           // 1..128
    u8  triangle_count;         // 1..128
    u16 material_slot;
    u16 flags;                  // LEAF_CLUSTER | WOOD_CLUSTER | TWO_SIDED | ...
    u16 bone;                   // the single wind bone of this cluster (0 for rigid meshes and parts)
} GpuCluster;

typedef struct {                // 16 B, fixed stride
    u32 tangent_frame;          // oct normal 2x10 | tangent angle 11 | bitangent sign 1
    u16 pos[3];                 // offset from pos_base (cluster extent <= 65535 grid steps)
    u16 uv[2];                  // unorm16, mapped by GpuMeshHeader uv range
    u16 wind;                   // flutter weight:3 | leaf bucket:4 (0 = wood, 1..15 = leaf drop order) | spare:9
} PackedVertex;

// Triangles: u32 each = three 8-bit cluster-local vertex indices + 8 spare bits.
```

Cluster render rule (DAG cut, per view): draw if `project(lod_error) ≤ τ` and `project(parent_error) > τ`, with τ ≈
1 pixel for the main view and ≈ 1 texel for shadow views. Errors are monotonic up the DAG, so exactly one LOD covers
every surface region.

Size: closed meshes ~14 B/triangle, leaf clusters ~37 B/triangle in this fixed format (before Zstd). Assemblies keep
unique foliage geometry small; variable-bit-width packing is a later optimization.

## Instances

```c
typedef struct {                // 64 B; previous transform in a parallel 48 B array for moving objects
    f32 m[12];                  // 3x4 affine, relative to the cell origin
    u32 cell;                   // index into the cell-offset table (offsets from the render origin)
    u32 mesh;                   // GpuMeshHeader index
    u32 material_remap;         // offset into slot → material table
    u32 flags;                  // DYNAMIC | CAST_SHADOW | IN_BVH | ...
} GpuInstance;
```

**One instance table, two parts.** Indices `[0, P)` are persistent instances (props, dynamic objects, written by
scene commands and dynamic transforms). Indices `[P, P + T)` are **transient part instances**, rebuilt every frame by
the vegetation-instances pass ([05](05-frame-pipeline.md)) for every assembly part that survives tree-level culling:
`m = cell offset × tree × BonePose(part.bone) × part local`, `mesh = part mesh`, `flags |= TRANSIENT`. A parallel
array gives the resolve what it needs to recompute the previous-frame transform for motion vectors:

```c
typedef struct { u32 tree; u32 part; } TransientRef;   // 8 B, indexed by (instance - P)
```

Culling, rasterization, VSM and resolve see one table and one code path. Cost at ~0.1 M visible parts: 6.4 MB + 0.8 MB
per frame.

Per frame the GPU produces `VisibleCluster { u32 instance; u32 cluster_ref; }` (8 B; `cluster_ref` = page:18 |
cluster-in-page:6 | flags) lists consumed by rasterization and material resolve; `instance` indexes the unified table.

## Materials

A material is a parameter record; shading code is selected by a small enum (no per-material shaders, no permutation
explosion, a few dozen pipelines total).

| Shading model | Used for |
|---|---|
| `DEFAULT` | rocks, props, bark, deadwood |
| `FOLIAGE` | leaves, needles: two-sided thin surface, diffuse transmission, specular sheen of the cuticle |
| `GRASS` | procedural blades (geometry and colour from `GrassType`, not from vertices) |
| `TERRAIN` | terrain via runtime virtual texture |
| `WATER`, `UNLIT` | — |

```c
typedef struct {                // 48 B
    u16 shading_model;
    u16 flags;                  // ALPHA_MASK | TWO_SIDED | ...
    u32 tex[4];                 // bindless indices: albedo/opacity, normal, ORM(+thickness), extra; ~0u = none
    u32 albedo_tint;            // RGBA8 sRGB
    u32 transmission_tint;      // RGBA8 sRGB (foliage)
    f16 roughness_scale, metallic_scale, normal_scale, alpha_cutoff;
    f16 specular, thickness, translucency, pad16;
    u32 pad;
} GpuMaterial;
```

## Textures

| Content | Format |
|---|---|
| Albedo (+opacity for fallback cards) | BC7 sRGB (BC1 where no alpha) |
| Normal | BC5 (XY, Z reconstructed) |
| AO / roughness / thickness / height | BC7 or BC4 per channel group |
| Sky/HDR | BC6H |
| Grass | none by default: colour ramps per `GrassType` |

- Alpha-tested fallback textures get coverage-preserving mips (Castaño 2010).
- **Streaming:** the material resolve writes the required mip per texture into a feedback buffer (atomic min); read
  back after two frames; the streamer reallocates the image with the new mip count, copies resident mips, uploads the
  missing ones, swaps the bindless index. No sparse residency.
- **Terrain:** adaptive runtime virtual texture (page table + 8192² physical cache, 128² pages, runtime BC
  compression) composited on the GPU from layer weights, layer textures, decals and dynamic layers (snow, litter,
  trample marks); the same RVT gives grass and far-forest colour matching. Details: [12](12-terrain.md).

## Vegetation layouts

```c
typedef struct {                // 16 B per tree, cooked per cell
    u16 pos[3];                 // x, z: cell-local, 256 m / 65536 = 3.9 mm; y: 0..512 m above base_altitude (7.8 mm)
    u16 species;
    u8  yaw;                    // 256 steps
    i8  lean[2];                // small tilt, snorm8
    u8  scale;                  // log-encoded 0.5..2
    u32 seed;                   // colour/season offset, wind phase, part culling
} TreeInstance;

typedef struct {                // 64 B
    u32 trunk_mesh;             // unique 'wood' geometry
    u32 parts_offset, part_count;
    u32 merged_mesh;            // whole tree, parts baked in, reduced rig (mid distance)
    u32 far_field;              // impostor atlas, later voxel mip set
    u32 rt_proxy;               // BLAS index, rest pose
    u32 bones_offset, bone_count;
    f32 radius, height;
    f32 px_merged;              // projected height in pixels below which parts → merged mesh
    f32 px_far;                 // ... merged mesh → far field
    f32 px_canopy;              // ... far field → terrain canopy layer
    u32 flags;                  // DECIDUOUS | CONIFER | SHRUB | ...
    u32 season_ramp;            // row in the season colour/leaf-density LUT
    u32 pad;
} GpuSpecies;

typedef struct {                // 32 B, shared by all trees of a species
    f32 pos[3];                 // tree space
    u32 rot;                    // quaternion, 4 x snorm8
    u32 mesh;                   // part mesh
    f16 scale;
    u16 bone;                   // attachment bone
    u16 flags;                  // LEAFY | FRUIT | CONE | FLOWER ...
    u16 leaf_seed;
    u32 pad;
} PartInstance;

typedef struct {                // 32 B
    f32 pivot[3];               // rest pivot, tree space
    u32 axis;                   // rest direction, oct 2 x snorm16
    u16 parent;                 // 0xFFFF = root
    u8  level;                  // 0 trunk, 1 branch, 2 sub-branch, 3 twig
    u8  group;                  // artist tuning group
    f16 length, stiffness, frequency, damping;
    u32 pad;
} WindBone;

typedef struct { f32 q[4]; f32 t[3]; u32 pad; } BonePose;   // 32 B, transient: rest tree space → posed tree space

typedef struct {                // 64 B per grass / ground-cover type
    f16 height[2];              // min, max (m)
    f16 width;                  // root width (m)
    f16 density;                // blades per m² at full density
    f16 bend, stiffness, clump_size, clump_strength;
    u32 color_root, color_tip;  // RGBA8 sRGB, modulated by season LUT and terrain RVT
    u32 material;
    u16 segments_near, segments_far;
    u32 pad[8];
} GrassType;
```

## Ray-tracing data

- **RT proxies:** per species variant a merged, simplified tree (~10–100k triangles), rest pose, vertex colours instead
  of textures, leaves either simplified geometry or cards with opacity micromaps. Built once at load, compacted.
- **Terrain:** per resident cell inside the RT range, a BLAS built on the GPU from the 2 m quadtree patches when the
  cell streams in ([12](12-terrain.md)).
- **Props/rocks:** BLAS from a fixed DAG cut (target error ~1–2 cm), compacted.
- **TLAS:** rebuilt every frame from instance lists (no refit bookkeeping).
- Experimental NVIDIA tier: BLAS directly from geometry pages via cluster acceleration structures (RTX Mega Geometry),
  enabling animated foliage in RT.
