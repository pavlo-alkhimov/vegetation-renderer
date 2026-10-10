# 12 — Terrain

Terrain carries everything else: grass generation, placement, statistical grass occlusion, the far field, the RT
range and the gameplay height queries. This document surveys what the titles already discussed in
[06](06-vegetation.md) and [10](10-milsim-survey.md) do, and fixes the design (review finding H3,
[11](11-architecture-review.md)).

## Survey

**Confirmed** = talk, official docs or developer statement; **inferred** = the engine's standard path, use by the
title not confirmed.

| Title | Engine | Geometry | Texturing | Status |
|---|---|---|---|---|
| Arma Reforger | Enfusion | One terrain object; **blocks** are the LOD unit (32×32 faces at full detail down to 2×2, 5 LODs, one draw call each); tiles carry the surface masks | Surface masks blended 0–1, ≤ 5 detail surfaces per block; a "height" texture gives parallax (per-pixel tessellation illusion, no geometry); occluders | confirmed |
| Far Cry 5 (GDC 2018) | Dunia | **GPU compute pipeline for quadtree heightfield LOD, culling and stitching**; procedural cliffs and displacement geometry | — | confirmed; target ~2 ms GPU for full-screen terrain |
| Far Cry 4 (GDC 2015) | Dunia | Heightmap near; terrain beyond ~300 m is offline-baked geometry + textures | **Adaptive virtual texture**: 512K² virtual, 2K² indirection, 9K² physical, 10 texels/cm over 10×10 km; terrain layers and decals splatted into the VT at runtime, frame-to-frame coherence | confirmed |
| Call of Duty (SIGGRAPH 2023) | IW engine | "Super Terrain" (Treyarch, GDC 2021): quadtrees, mesh simplification for coarser levels | Virtual texturing extending the AVT idea; scales from mobile to high-end PC | confirmed |
| Ghost of Tsushima (GDC 2021) | Sucker Punch | **Height-map tile hierarchy** at several resolutions, paged in and out; dynamically tessellated ground mesh; cliffs are separate rock models; the same heightmap is queried by particles and wind | Artist rules in a GPU-interpreted bytecode language drive texturing and placement | confirmed |
| UE5 titles: Squad, Gray Zone Warfare, Hell Let Loose: Vietnam, Delta Force, Witcher 4 demo | UE5 | **Landscape** heightfield components; optional Nanite landscape (GPU culling, streaming, LOD — keeps the non-Nanite data too, ~2× memory, required for RVT and water); Nanite tessellation (5.4+); **Mesh Terrain** in 5.8 (experimental, volumetric, modifier-based, Nanite, World Partition only) | Runtime virtual texture caches the landscape material | Epic docs confirmed; per-title use inferred |
| Assassin's Creed Shadows | Anvil | "Micropolygon" virtual-geometry system; terrain specifics unpublished | — | partial |
| Escape from Tarkov | Unity | Unity terrain: heightmap with pixel-error LOD | Splat maps near, pre-composited basemap beyond the basemap distance (visible pop) | Unity docs confirmed; Tarkov use inferred |
| Kingdom Come: Deliverance II | CryEngine | Heightmap; distant landscape rendered as real geometry (no flat backdrops); parallax/tessellation setting | — | partial |

### What the survey says

1. **Every shipped large world uses a heightfield quadtree** (Enfusion blocks, Far Cry 5, Call of Duty, Tsushima,
   UE Landscape, Unity). Cliffs and overhangs are separate meshes. Nanite landscape is an add-on that doubles the
   data; volumetric Mesh Terrain is experimental.
2. **GPU-driven terrain LOD and culling** fit in ~2 ms for full-screen terrain (Far Cry 5, 2018 consoles).
3. **Texturing at scale is virtual texturing composited at runtime** from layers and decals (Far Cry 4 → Call of
   Duty); the lower end uses a pre-composited basemap or a material cache (Unity, UE RVT).
4. **One heightmap serves rendering and gameplay** (Tsushima: particles and wind; Reforger: grass from surface
   materials).

## Design

### Geometry: quadtree heightfield patches as procedural clusters

- **Source data per 256 m cell:** 257×257 `u16` heights (1 m spacing) + a mip pyramid of heights (1.33×) + per
  quadtree node `{min height, max height, max geometric error}`. No stored vertices: ~175 KB per cell.
- **Patch = one cluster:** 8×8 quads, 81 vertices, 128 triangles — exactly the cluster limits. A patch at quadtree
  level `k` spans 8·2ᵏ m; level 0 = 8 m at 1 m spacing; coarser levels continue across cells up to a world-level
  coarse heightmap for the horizon.
- **Selection** like clusters: projected `max geometric error ≤ τ` (≈ 1 px), frustum and HiZ against the node's
  min/max box, both culling phases. Selected patches go into the normal raster bins (mostly HW; small far patches SW).
- **Cracks and popping:** CDLOD vertex morphing (Strugar) — vertices morph towards the coarser level's positions
  within the transition band, so neighbouring levels meet exactly; no skirts, no stitching tables.
- **Raster and resolve:** the mesh shader (or SW raster) generates vertices from the height mips; `vis64` kind 4 =
  patch index | triangle; the resolve regenerates the triangle from the same function, normals from height central
  differences. HiZ, both culling phases and the VSM all reuse the same path.
- **Overhangs, cliffs, rock outcrops, riverbanks:** separate cluster meshes (photogrammetry rocks), blended into the
  terrain at the base with the terrain RVT colour.
- **Height edits** (editor sculpting, optional craters): modify the height tile and its mips, recompute the node
  errors of the touched subtree — milliseconds, no re-cook.
- Rejected: cooked cluster DAG per cell (one code path, but ~2–4 MB per cell instead of ~175 KB and no cheap
  edits); Nanite-style landscape keeping both representations (UE: 2× data); volumetric terrain (no need for caves;
  the biome's overhangs are rocks).

### Texturing: adaptive runtime virtual texture

- **Layers:** a library of ground materials (meadow soil, forest floor, leaf litter, needle litter, moss, mud,
  gravel, rock, field soil, snow, asphalt, ...); each cell carries weights for ≤ 8 layers at 0.5 m (Reforger: ≤ 5
  per block) plus decals (tracks, paths, burnt ground).
- **Adaptive virtual texture** (Far Cry 4 / Call of Duty lineage): indirection per world sector sized by camera
  distance; target ~4–10 texels/cm within ~5 m (prone view), falling with distance; 128² pages + 4-texel border.
- **Physical cache:** 8192² texels, composited on the GPU, compressed at runtime (BC1 albedo + BC5 normal + BC4
  roughness/AO ≈ 2 B/texel → 128 MB).
- **Feedback:** the material resolve writes requested pages (like texture mips); the CPU decides residency (LRU)
  from the readback two frames later; missing pages fall back to coarser mips.
- **Dynamic layers** written into the RVT, not shaded per pixel: snow cover, wetness, seasonal litter under
  deciduous trees, trample/flatten marks — a layer change invalidates only the affected pages.
- **Far terrain** (beyond the coarsest AVT level): per-cell canopy layer (colour + canopy height, already in
  [04](04-data-representation.md)) — the equivalent of Far Cry 4's baked far terrain and Unity's basemap, but
  season-aware.
- **Near detail:** parallax from a layer height channel only within a few metres; geometric displacement later if
  measurements justify it.

### Shared height function

`terrain_height(cell, x, z)` interpolates the height tile with the **same triangle split** as the leaf patches; it
lives in `gpu_shared.h` and is used by the game (collision queries, AI) and by grass generation. Grass and ground
cover sample the **rendered** height (the morphed LOD level at that point) so blades never float above a coarser
patch.

### Ray tracing and shadows

- RT: per resident cell inside the RT range, a BLAS built on the GPU from the level-1 patches (2 m spacing) when the
  cell streams in; terrain beyond the RT range is represented by the coarse heightfield in the GI miss path.
  (Avatar ray-marches its terrain heightfield instead — an alternative if BLAS memory becomes tight.)
- Shadows: terrain renders into the VSM through the same path; long valley shadows at low sun come from the coarse
  clipmap levels.

### Budget

| Item | ms / MB |
|---|---|
| Patch selection + raster (inside "culling" and "visibility raster" lines of [03](03-hardware-mapping.md)) | ~0.6 ms |
| RVT page compositing + compression | ~0.4 ms (new line in 03) |
| Height data, resident cells | ~10–30 MB |
| RVT physical cache | 128 MB (inside the texture pool) |
