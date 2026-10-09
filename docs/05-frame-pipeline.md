# 05 — Frame pipeline

## Passes and queues

```
GRAPHICS  |Begin|RVT|VegInst|Wind+Bones|Cull P1|Raster P1|HiZ|Cull+Raster P2|HiZ|>g1|VSM mark|VSM raster|>g2|<c1 Direct|<c2 Composite|Transp|TAA|Post|UI|Present
ASYNC     |<g0 TLAS build ........................|<g1 Classify|Resolve|>c1|GI|Reflections|Clouds|<g2 Fog|>c2
TRANSFER  |streaming uploads (continuous) ---------------------------------------------------------------------->
           >x = signal timeline point x, <x = wait for it.  g0 = Begin done (instance data scattered).
```

**The queue split is an initial guess.** Serialized, the frame is ~27.4 ms; on this split the critical path is
Begin, RVT and visibility (6.7) → classify + resolve (2.5) → GI + reflections + clouds/fog (9.5) →
composite/TAA/post (3.2) ≈ 21.9 ms, and the graphics queue idles ~7 ms after direct lighting waiting for `c2`. Balancing moves, decided by GPU
Trace in M4: reflections, SSILVB and the GI denoiser to graphics; async keeps TLAS build, GI rays + ReSTIR, clouds,
fog and atmosphere LUTs.

| # | Pass | Q | Reads | Writes | Technique |
|---|---|---|---|---|---|
| 0 | Begin | G | upload ring, completed transfers | instance arrays, view constants, cell offsets, residency tables, deferred frees | scatter copies of dynamic data, scene commands, cell patches; the only place residency changes ([02](02-architecture.md)) |
| 0b | RVT update | G | RVT page requests (readback N-2), layer textures, masks, decals, dynamic layers | RVT physical cache, indirection | composite + runtime BC compression of missing/invalidated pages ([12](12-terrain.md)) |
| 1 | Vegetation instances | G | vegetation cells, species, HiZ(prev) | near-tree list, **transient part instances + `TransientRef`**, far-field list, grass tile list | per-tree LOD class by projected height (bounds padded for wind sway); two-phase culling like all instances; surviving assembly parts appended to the unified instance table ([04](04-data-representation.md)) |
| 2 | Wind + bones | G | `WindGlobal`, interactors, wind bones, near-tree list | wind field, trample map, `BonePose` cur/prev | stateless evaluation ([06](06-vegetation.md)) |
| 3 | Cull P1 | G | unified instance table, hierarchy nodes, terrain quadtree, HiZ(prev) | cluster bins {HW, SW}, P2 lists, page requests | two-phase occlusion, DAG cut, terrain patch selection |
| 4 | Raster P1 | G | cluster bins, pages, bone poses | `vis64` | mesh shaders (HW) + compute (SW), grass mesh shaders, far-field splats |
| 5 | HiZ | G | `vis64` | HiZ | min-depth pyramid (reversed Z) |
| 6 | Cull + Raster P2 | G | P2 lists, HiZ | `vis64` | retest occluded items, raster newly visible |
| 7 | HiZ final | G | `vis64` | HiZ, depth (D32) | kept for next frame, transparents, SSR |
| 8 | TLAS build | C | RT instance lists | TLAS | full rebuild per frame |
| 9 | Classify | C | `vis64`, clusters, materials | tile lists per shading model | 8×8 tiles |
| 10 | Resolve | C | `vis64`, pages, instances, bone poses, materials, textures | G-buffer, motion vectors, texture feedback | analytic barycentrics + gradients |
| 11 | VSM mark + raster | G | depth, VSM page table | VSM physical pages | cluster rasterizer in shadow views |
| 12 | Direct lighting | G | G-buffer, VSM, depth | `hdr_light` | sun + sky + local lights, contact shadows, foliage transmission |
| 13 | GI | C | G-buffer, TLAS, radiance cache, previous lit frame | GI diffuse | screen trace → ray query → cache, ReSTIR GI, denoise |
| 14 | Reflections | C | G-buffer, TLAS, radiance cache | reflections | SSR → ray query, denoise |
| 15 | Clouds, fog | C | atmosphere LUTs, VSM (coarse), wind | cloud buffer, froxel volume | ray march + temporal reprojection |
| 16 | Composite | G | all lighting terms, fog | `hdr_light` | demodulated GI × albedo, aerial perspective |
| 17 | Transparents | G | depth, `hdr_light` | `hdr_light` | particles (pollen, falling leaves, snow, rain), water |
| 18 | TAA / DLAA | G | `hdr_light`, motion, depth, history | output, history | own TAA; DLSS (DLAA, Ray Reconstruction) on NVIDIA |
| 19 | Post + UI | G | output | swapchain | exposure, bloom, tone map / HDR10, grading, UI |
| 20 | Readback | G | stats, pick results, feedback | readback ring | read by CPU two frames later |

Everything must also run correctly on a single queue in the same order (debug option and fallback).

## Visibility: culling, LOD, rasterization

### Two-phase occlusion culling (Haar & Aaltonen 2015, Nanite 2021)

1. **Phase 1:** test instances and clusters against the **previous** frame's HiZ using previous transforms. Visible →
   rasterize. Rejected → appended to phase-2 lists.
2. Build HiZ from phase-1 depth.
3. **Phase 2:** retest the phase-2 lists against the new HiZ with current transforms; rasterize newly visible items.

Conservative without reprojection heuristics: anything wrongly rejected in phase 1 is caught in phase 2.

### Hierarchy traversal and LOD

Level-by-level: one indirect dispatch per BVH level (depth ≤ ~12), each consuming the node list of the previous level.
Node test: frustum, HiZ, and `project(max_parent_error) > τ` (otherwise the whole subtree is finer than needed).
Cluster test: `project(lod_error) ≤ τ < project(parent_error)` + frustum + HiZ. No persistent-thread scheduling:
Vulkan gives no forward-progress guarantee across vendors.

### Raster binning

Per visible cluster, estimate triangle edge length in pixels (projected cluster radius / √triangle_count):
- **≥ ~8 px → HW:** mesh shader, 1 workgroup per cluster (128 threads), fragment shader does `atomicMax` on `vis64`.
  No attachments.
- **< ~8 px → SW:** compute, 1 workgroup per cluster, edge functions over the triangle bounding box, `atomicMax`.
- Thresholds tuned by measurement. Long thin triangles (grass blades) always go HW.

**Open decision — HW raster target** (closed by measurement on benchmark scene 1, meadow, in M3):

| Variant | HW path | Trade-off |
|---|---|---|
| A (Nanite-style) | fragment shader `atomicMax` into `vis64`, no attachments | one target for HW and SW; no early-Z — every hidden grass fragment still costs an L2 atomic (~30 M atomics ≈ 1 ms for 1 M blades) |
| B (classic visibility buffer) | `D32` depth + `R32_UINT` payload attachments, early-Z | hidden fragments rejected in the ROP; SW raster keeps the atomic buffer and a merge step (or SW tests against HW depth) unifies them |

Both are implemented in M1 (B is about a day of extra work); the loser is deleted after M3.

### Terrain

Terrain patches are procedural clusters: quadtree nodes selected by projected geometric error, 8×8-quad patches
generated from the height mips in the mesh shader, CDLOD morphing for crack-free transitions. They share culling
phases, raster bins, HiZ and VSM with all other geometry ([12](12-terrain.md)).

### `vis64` layout

```
bits 63..32  depth: reversed-Z f32 bit pattern (non-negative floats order like uints → atomicMax keeps the nearest)
bits 31..29  kind
  0 cluster   : [28:7] visible-cluster index (4 M)   | [6:0] triangle (128)
  1 grass     : [28:4] visible-blade index (32 M)    | [3:0] blade segment
  2 impostor  : [28:0] visible far-tree index        (resolve re-derives the atlas sample from the pixel ray)
  3 voxel     : [28:6] visible brick index (8 M)     | [5:0] voxel in a 4×4×4 brick
  4 terrain   : [28:7] terrain patch index (4 M)     | [6:0] triangle  (vertices regenerated from height mips)
  5 gaussian  : reserved for the far-field Gaussian experiment (09)
  6..7 reserved
Clear value 0 = far plane, nothing.
```
Per-frame lists give each index meaning: `VisibleCluster` (8 B), visible blade `(tile, blade)` (8 B), visible far
tree (4 B), visible brick `(tree, brick)` (8 B). Overflow is clamped and reported in the stats.

A storage buffer (not an image) with 8×8-tile addressing; 64-bit buffer atomics are core Vulkan features.

## Shadows: virtual shadow map clipmap (sun)

- 12 clipmap levels, 8 m → 16 km, each 16k² virtual texels in 128² pages; physical pool 2048 pages (128 MB).
- **Mark:** every visible pixel marks the page of the level matching its footprint (+ filter margin).
- **Raster:** invalidated pages are rendered by the **same cluster pipeline** (culling, DAG cut with τ in shadow
  texels, HW/SW binning; depth-only payload).
- **Caching:** static geometry pages persist until the sun direction or streaming invalidates them. Animated
  vegetation is re-rendered each frame only within the near levels (≈ < 50 m); farther levels use the rest pose.
- **Time of day:** sun direction updates are quantized and page re-rendering is amortized over frames.
- **Level range check:** with footprint-based marking, a receiver at distance `d` uses the coarsest level with
  texel ≤ 1.07 mm × `d`, i.e. level width ≈ 16 `d`: the 8 m level serves receivers at ~0.5 m (prone view, weapon),
  16 m and 32 m serve 1–2 m. All 12 levels are used. Low sun (the biome's normal case) stretches shadows over more
  receiver pixels and multiplies page demand — the 2048-page pool is sized for that and measured in M2.
- **Render origin:** clipmaps are snapped to texel grids in render-origin space; an origin change shifts page tables
  by whole pages, cached pages survive ([02](02-architecture.md)).
- **Filtering:** penumbra from the sun's angular size (blocker search + PCF; SMRT-style marching as an upgrade).
  Physically sized penumbrae are required for dappled light under canopies ([06](06-vegetation.md)).
- **Contact shadows:** screen-space ray march (Bend Studio style) for blade- and leaf-scale detail.
- Local lights: few, shadowed through the same page pool (cube/spot virtual maps). Many-light stochastic sampling
  (MegaLights-like) is a later option.
- Alternative tier: ray-traced sun shadows near the camera — requires an animated BVH for near vegetation (Mega
  Geometry on NVIDIA, or refit) plus denoising. Decided by measurement in M4.

## Material resolve

1. **Classify** 8×8 tiles by shading model → indirect dispatch per model; mixed tiles go to an ubershader.
2. **Resolve** per pixel:
   - kind 0: fetch cluster, 3 vertices, decode, apply instance transform and the cluster's bone pose for **current
     and previous** frame → exact motion vectors for wind-animated foliage (transient part instances recompute the
     previous transform from `TransientRef` and the previous `BonePose`).
   - Intersect the pixel ray with the triangle → barycentrics; derivatives from neighbouring pixel rays → texture
     gradients (Burns & Hunt 2013).
   - kind 1 (grass): regenerate the blade from its id, intersect the ribbon → position along the blade, side, normal.
   - kind 2 (impostor): view-cell selection + atlas sample from the pixel ray.
   - kind 3 (voxel): voxel attributes (albedo, normal, coverage, transmission).
   - kind 4 (terrain): regenerate the patch triangle from the height mips, normal from height differences, colour
     and material from the RVT (writes RVT page requests).
3. Write thin G-buffer (albedo/transmission, normal, roughness/specular/translucency/model), motion vectors, and
   texture mip feedback.
4. Geometric specular anti-aliasing (normal-variance → roughness widening) here — crucial for foliage shimmer.

## Lighting

### Direct
Sun: VSM + contact shadows; foliage transmission (thin-surface diffuse transmission, back side lit by the same shadow
lookup); grass translucency at low sun angles. Sky: irradiance from the atmosphere/cloud state; occlusion comes from
the GI system. Local lights: clustered light lists.

### Indirect diffuse (GI) — target tier
- Rays: 1 per 2×2 pixels (518k), cosine-distributed. Short screen-space trace against HiZ first (hits reuse last
  frame's lit colour), then `rayQuery` against the TLAS (terrain, rocks, RT tree proxies), max ~300 m, miss = sky.
- Hit shading: **world-space radiance cache** (spatial hash grid, cell size grows with distance; SHaRC-like), updated
  each frame by ~130k cache rays with one bounce + cache lookup (multi-bounce by feedback) and an RT shadow ray to the
  sun. Leaf proxies use the two-sided material (green transmitted bounce under canopies).
- **ReSTIR GI** temporal + spatial reuse at quarter resolution, then denoise (temporal accumulation + variance-guided
  spatial filter) and depth/normal-aware upsample. Lighting is kept demodulated (divided by albedo).
- **Near field:** screen-space indirect lighting/occlusion with visibility bitmask (SSILVB) at full resolution —
  grass and small leaves are not in the BVH, this supplies their contact occlusion.
- On NVIDIA, DLSS Ray Reconstruction can replace the denoisers + TAA (quality option, not a dependency).

### Reflections
Roughness > 0.4: from GI/radiance cache. 0.1–0.4: quarter-resolution rays with temporal/spatial reuse. Mirror (water):
half-resolution rays, SSR first.

## Atmosphere, clouds, fog

- Atmosphere: Hillaire 2020 LUTs (transmittance, multiple scattering, sky view, aerial-perspective froxels).
- Clouds: ray-marched volumetric layer (Nubis-inspired) at 480×270, amortized + temporal reprojection; overcast
  stratus/stratocumulus decks are a primary case for the biome; cloud shadow map for the ground.
- Fog: froxel volume 160×90×64 (Wronski 2014, Hillaire 2015) with sun shadowing from coarse VSM levels → light shafts
  through canopies; height fog + noise for valley mist.

## Transparents
Particles (pollen, seeds, falling leaves driven by season and wind, snow, rain) and water (rivers, lakes: RT/SSR
reflections, refraction, shore foam). Forward-shaded after composite, depth-tested against the exported D32.

## Reconstruction and output

- **TAA:** Halton jitter, motion-vector reprojection, YCoCg variance clipping, depth/id disocclusion, reactive mask
  for particles. Native 1080p by default: upscalers struggle most with sub-pixel foliage.
- **DLSS (NVIDIA option):** DLAA at native resolution; Ray Reconstruction when enabled. FSR 3.1 (open, Vulkan) as
  vendor-neutral comparison.
- **Post:** histogram auto-exposure, energy-conserving bloom, tone mapping (AgX-style vs ACES 2.0 — decided by
  evaluation), HDR10 PQ path with paper white from display metadata, 3D LUT grading, film grain, UI in display space.

## Synchronization

- Timeline semaphore per queue; per-frame points `value = frame * 8 + point`.
- Cross-queue resources use `VK_SHARING_MODE_CONCURRENT` (simplest; check image-compression impact on AMD).
- Within a queue: synchronization2 barriers from a small access tracker (last stage/access/layout per resource).
  With `VK_KHR_unified_image_layouts` all images stay in `GENERAL`.
- Validated continuously with the synchronization validation layer in dev builds.

## Debug views (always available)
`vis64` kind/cluster/triangle ids, LOD level and DAG error, HW vs SW raster, overdraw (atomic count), instance/part
expansion, VSM pages (resident/invalidated/level), every G-buffer channel, motion vectors, GI rays/hits/cache cells,
ReSTIR reservoirs, wind field, bone poses, grass tiles and blade LOD, texture mip feedback, per-pass GPU time.
