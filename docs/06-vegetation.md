# 06 — Vegetation and grass

This is the core of the project: survey of what shipped or was shown up to October 2026, and the chosen design.

## 1. Target biome: Central and Eastern Europe

| Habitat | Key species | Rendering-relevant traits |
|---|---|---|
| Lowland/hill broadleaf forest | European beech, pedunculate/sessile oak, hornbeam, lime, sycamore/Norway maple, ash | closed canopies, deep shade, smooth grey beech bark, strong autumn colours, thick leaf litter, **leaf-off half the year** |
| Pioneer stands, Baltic/Polesia | silver birch, aspen, grey alder | white bark; pendulous birch twigs; aspen leaves flutter in the faintest wind |
| Mountain forest (Carpathians, Sudetes) | Norway spruce, silver fir, European larch, beech; dwarf pine at the treeline | dense needle sprays, snow loading, larch turns gold and sheds needles, valley mist |
| Sandy pine forest (Poland, Belarus, Ukraine) | Scots pine with birch | high crowns, orange upper bark, open light, heather/blueberry/moss/lichen floor |
| Floodplains, wetlands | willow, black alder, poplar; reed, cattail, sedge | tall grasses at water margins, reflections |
| Meadows, pastures | mixed grasses + forbs (clover, dandelion, buttercup, chamomile, poppy, cornflower) | mown vs unmown, flowers in spring/summer |
| Steppe, forest-steppe (south-east) | feather grass (*Stipa*), dry grasses, blackthorn | long silky awns → large travelling waves in wind |
| Agriculture | wheat, rye, barley, maize, sunflower, rapeseed | huge monocultures, row structure, growth stages, stubble |
| Understory, floor | hazel, elder, hawthorn, blackthorn, juniper; bracken, bramble, nettle; moss, litter, deadwood | mostly ground-cover types + terrain layers |

### Consequences for the design

| Trait | Consequence |
|---|---|
| Strong seasons | Season is a first-class parameter: per-species colour/leaf-density ramps, per-leaf variation, litter, falling leaves, snow, crop stages (§5.7) |
| Leaf-off winter | Bare twig networks dominate the image → twigs are real geometry; sub-pixel twig crowns need aggregate LOD (voxel coverage); RT proxies and impostors need leaf-on/leaf-off variants |
| Conifer needles | Needle sprays as geometry; aggressive aggregate LOD (needles → clumps → voxels); dark high-frequency foliage → specular AA and temporal stability matter |
| Overcast skies, fog, low sun | Sky light and GI under canopies dominate the look → GI quality ranks above shadow sharpness; low sun → long shadows and strongly backlit grass; valley mist |
| Sunny deciduous canopy | Dappled light (sunflecks) needs physically sized penumbrae |
| Wheat, steppe grass | Wind must produce spatially coherent gust fronts travelling across fields, not per-blade noise |
| Vistas over mixed forest | Far field must keep per-tree colour and crown shape (round broadleaf crowns vs conifer spires, autumn mosaic) |

## 2. Why vegetation is the hardest case

- **Instance counts:** dense forest ≈ 1 tree / 25 m² = 40k trees/km². The Witcher 4 GDC 2026 demo: ~1 M trees and
  ~60 M plants on 5×5 km.
- **Geometry:** a broadleaf tree carries 10⁵–10⁶ leaves; geometric leaves mean 10⁶–10⁷ triangles per tree at full
  detail (Witcher 4: large trees > 10 M polygons).
- **Sub-pixel detail:** at 1080p and 60° vertical FOV one pixel spans ≈ 1.07 mm per metre of distance. A 1 mm wide
  spruce needle is sub-pixel beyond ~1 m, a 1 cm twig beyond ~9 m. A 20 m tree is 200 px tall at ~93 m, 40 px at
  ~465 m, 4 px at ~4.7 km.
- **Overdraw:** stacked leaves → deep depth complexity; alpha testing disables early-Z, slows software raster and
  needs any-hit shaders in RT.
- **Animation:** everything moves → invalidates shadow caches and BVHs; every vertex needs a correct motion vector.
- **Shading:** thin, two-sided, translucent surfaces; severe specular aliasing.
- **LOD:** edge-collapse simplification of disconnected aggregates (leaves, needles) shrinks or thins the canopy →
  aggregate-aware representations are required at distance.

## 3. State of the art (2017–2026)

### Grass and ground cover

| System | Technique | Takeaway |
|---|---|---|
| Horizon Zero Dawn (GDC 2017, van Muijden) | GPU run-time procedural placement from density/world data | ground cover is generated, not stored |
| Ghost of Tsushima (GDC 2021, Wohllaib) | Blades generated per tile on the GPU every frame; Bézier blade shape; clumping; rounded normals; noise-driven wind; vertex LOD; density/width trade-off with distance | the reference design for grass fields |
| AMD GPUOpen (2024) | Mesh-shader procedural grass: tiles emit up to 16×16 patches, LOD reduces groups and triangles; GDC 2024 work-graph mesh-node procedural world | mesh shaders remove the compute → buffer → indirect-draw round trip |
| Ghost of Yōtei (2025) | GPU compute grass renderer, doubled density and renderable count; weapon sweeps written into a "cut buffer" | interaction as screen/world-space buffers |
| Kingdom Come: Deliverance II (2025) | Dense Central European vegetation in heavily modified CryEngine; SVOGI voxel GI without HW RT; plant animation at half frame rate | same biome; vegetation density + voxel GI as non-RT fallback reference |

### Trees and foliage

| System | Technique | Takeaway |
|---|---|---|
| Crysis (GPU Gems 3, 2007) | Vertex-shader wind: main bending + detail bending from vertex colours | baseline that most wind systems still extend |
| Horizon Forbidden West (GDC 2022, McLaren) | Visibility-buffer pre-pass + compute "deferred texturing" to accelerate foliage and alpha-tested geometry; software VRS; overlapped with shadow rendering | visibility buffer is the right base for foliage |
| Alan Wake 2 (2023; GDC 2024, Kandar; Jansson) | GPU-driven mesh-shader pipeline with fine occlusion; GPU skinning of bone rigs on all vegetation in compute; opacity micromaps (GDC 2025) | bones for foliage at scale are affordable |
| Avatar: Frontiers of Pandora (GDC 2024, Kuenlin) | RT GI/reflections in dense jungle: RT world uses lower LODs, one material per mesh without textures, alpha test with dithering; terrain ray-marched separately | simplified RT proxies are enough for GI |
| Indiana Jones and the Great Circle (Vulkan; NVIDIA 2025) | `VK_EXT_opacity_micromap`: vegetation path-tracing pass 7.90 → 3.58 ms (RTX 5080); dynamic vegetation BLAS compaction 1027 → 606 MB | OMM and compaction are mandatory for foliage RT |
| Assassin's Creed Shadows (GDC 2025, Lopez; SIGGRAPH 2025, Leblanc & Conte) | GPU-driven pipeline, RT GI in a dynamic open world with **dynamic seasons**; vegetation animated on the GPU by wind from a fluid simulation; alpha-tested vegetation stays expensive in RT | seasons + RT GI + GPU wind in one shipped game |
| UE 5.7 Nanite Foliage (Nov 2025, experimental), Witcher 4 UE5 demo (Jun 2025, PS5 at 60 FPS) | **Assemblies** (instanced parts), **Skinning** (bone rigs for wind; Dynamic Wind: ~100k bones ≈ 0.1 ms GPU), **Voxels** (near-pixel-sized aggregate voxels at distance: no cross-fades, no authored LODs, no overdraw); PVE graph tool outputs skeletal assemblies | the current industry direction; closest to our design |
| Witcher 4 + RTX Mega Geometry foliage (GDC 2026) | Path-traced foliage: ~1 M trees, ~60 M plants, 200+ species on 5×5 km; geometric needles/leaves, no alpha cards; trees built from ~a dozen twig models instanced thousands of times; BVH built from clusters; RTX 5090 ~80 FPS at 4K with DLSS Quality | upper bound of what is possible; NVIDIA-only RT path |
| HPG 2025 "Real-Time GPU Tree Generation" | Trees generated every frame from kilobytes of code (work graphs + mesh nodes), continuous LOD, animation, seasonal details; 3.13 ms on RX 7900 XTX | procedural seasons idea; work graphs not portable (dropped from D3D12 SM 6.10) |
| Octahedral impostors (Brucks 2018) | Per-species atlas of views on an octahedral layout, parallax-corrected | robust far field, phase A |
| SpeedTree | Authoring middleware with wind model, LOD and billboard generation | possible source tool; we use our own runtime representation |

## 4. Chosen design — overview

```
cooked cell ──► tree instances (16 B) ──┐
                                        ├─► per-tree LOD class (projected height) ──► assemblies │ merged │ far │ canopy
density maps ──► GPU ground cover ──────┤                                                 │
                                        └─► grass tiles ──► mesh shaders ──► vis64 (kind 1)
wind (game) ──► wind field + gusts ──► bone poses (cur, prev) ──► raster + resolve (exact motion vectors)
season/snow (game) ──► season LUT ──► leaf density + colour ──► raster (leaf collapse) + resolve (shading)
```

## 5. Chosen design — details

### 5.1 Representation

- **Geometric leaves and needles; no alpha cards by default.** Alpha testing costs at every stage: early-Z (HW),
  per-pixel texture fetch in SW raster, any-hit shaders in RT, meaningless simplification, temporal noise. Geometry
  costs triangles, which the cluster DAG and SW raster are built for.
- **Assemblies:** species = unique `wood` mesh (trunk, main branches) + a dozen part meshes (twigs with leaves,
  needle sprays, cones, flowers) placed thousands of times on wind bones (`PartInstance`, 32 B).
- **Individually addressable leaves:** every leaf has a 4-bit bucket in its vertices (drop order for seasonal
  thinning, colour-change order).
- Fallback for third-party card assets: hashed alpha test (Wyman & McGuire 2017), coverage-preserving mips, OMM in RT.

### 5.2 Placement

| Class | Examples | Storage | Placement |
|---|---|---|---|
| Trees, large shrubs | beech, spruce, birch, hazel | cooked per cell, 16 B/tree, stable ids | offline rules + painting (cooker step) |
| Ground cover | grasses, flowers, ferns, small plants, crops | nothing per instance | GPU every frame from density maps + deterministic hash; same function on CPU for gameplay queries |

Rules (offline and runtime): slope, altitude, moisture, terrain layer, shade under trees (shade-tolerant understory),
forest edges (shrub belts), paths and roads, species clustering, crop rows aligned to field polygons.

### 5.3 LOD chain — driven by projected size, not distance

| Projected tree height | Distance (20 m tree) | Representation | Raster | Shadow | RT |
|---|---|---|---|---|---|
| > 200 px | < ~93 m | wood + parts (assemblies), DAG per part, full rig | HW + SW | VSM, animated | proxy, rest pose |
| 40–200 px | 93–465 m | merged whole-tree DAG, reduced rig | mostly SW | VSM, rest pose beyond ~50 m (cached) | proxy |
| 4–40 px | 0.47–4.7 km | far field: impostor (phase A) → voxels (phase B) | compute splat | VSM cached | coarse proxy or none |
| < 4 px | > 4.7 km | canopy layer in terrain (per-tree colour, canopy height) | terrain | terrain + canopy height | terrain heightfield |

Visible-count estimate for dense forest (one tree per 25 m², ~¼ of the disc in the frustum, before occlusion):
~275 assembly trees (~0.5 M part instances at ~2000 parts/tree), ~6.5k merged trees, ~680k far-field trees (mostly
occluded by terrain and nearer trees). Far-field culling must cost O(10 ns) per tree.

Transitions: within a DAG continuous; parts ↔ merged at matched error; geometry ↔ far field as a dithered swap over
~0.3–0.5 s (resolved by TAA); far field ↔ canopy layer by coverage blend.

### 5.4 Far field

- **Phase A — octahedral impostors** per species variant: albedo + coverage, normal + depth, transmission; separate
  wood and leaf layers so leaf density and season colour apply at runtime (leaf texels above the current density are
  dropped and the wood layer shows through). Per-tree tint from seed. Rasterized as small quads with a hashed coverage
  test into `vis64` kind 2.
- **Phase B — voxels** (Nanite Voxels direction): per species variant a sparse voxel mip chain; per voxel: leaf-area
  density per bucket class, wood coverage, mean albedo, normal distribution (mean + variance), transmission. Select
  the mip where a voxel ≈ 1 px, splat each voxel (4×4×4 bricks, one workgroup per visible brick) to one pixel with
  stochastic coverage (hash(voxel, pixel, frame) < coverage) into `vis64` kind 3; TAA integrates coverage.
  Animation: whole-tree sway only.
  Voxel attributes are fitted offline against the path-traced reference (Slang autodiff).

### 5.5 Grass and ground cover

1. **Tile selection** (compute): 8×8 m terrain tiles within ~100 m; frustum + HiZ; per tile LOD (density, segments).
2. **Generation** (task + mesh shader): task shader per tile spawns patch workgroups; each mesh workgroup emits up to
   128 vertices / 128 triangles (e.g. 16 blades × 3 segments). Blade = cubic Bézier ribbon with taper. Inputs:
   density/type maps, hash(tile, index) → jittered position, clump cells (height, direction and colour correlation),
   terrain height/normal, terrain RVT tint, season LUT (green → dry → snow-flattened), snow depth, trample/cut map,
   wind. View-dependent widening keeps edge-on blades from vanishing.
3. **Raster:** HW (long thin triangles), `vis64` kind 1 = visible-blade index + segment. The mesh shader appends
   `(tile, blade)` (8 B) per visible blade — the only per-blade memory, transient.
4. **Resolve:** regenerate the blade with the same function, intersect the pixel ray with the ribbon → position along
   the blade, side, normal; shade with model `GRASS`.
5. **Shading:** normals rounded across the width, translucency (strong at low sun), sheen along the blade, root
   darkening from density × height (self-occlusion), root → tip colour ramp, dryness.
6. **Distance:** density falls with distance while width grows to preserve coverage; beyond ~100 m only the terrain
   carries the grass look (RVT colour + grass-like roughness/sheen and back-scatter hotspot).
7. **Wind:** travelling gust fronts + turbulence; per-type stiffness — *Stipa* and wheat bend far and show waves.
8. **Interaction:** interactors → trample map (bend direction + amount, 64×64 m around the camera at 12.5 cm,
   recovers over time); cut map removes blades.
9. **Other ground cover** (flowers, ferns, crops like sunflower and maize, small shrubs): same placement, but emitted
   as instances of small cluster meshes into the regular cluster pipeline. Crops use field-aligned row grids instead of
   random jitter.

Density is an art parameter: 50–300 blades/m² near, 10–30 % of that with wider blades beyond 30 m. Expect 0.3–1 M
visible blades per frame; budget ≤ 2 ms for generation + raster + resolve.

### 5.6 Wind and animation

- **Wind field** = global wind from the game + **gust fronts**: noise along the wind direction, advected at wind
  speed, scaled by `gust_wavelength` → coherent bands moving downwind (the "waves over wheat" look) + turbulence +
  local impulses from interactors (explosions, rotor downwash) splatted into a decaying 2D texture around the camera.
  Upgrade path: coarse fluid advection (AC Shadows uses a fluid simulation).
- **Bones:** for trees above the merged threshold, per frame and per bone: rotation = f(wind at the bone at
  `t − delay(level)`, stiffness, natural frequency, phase from seed), composed parent → child (one workgroup per tree,
  levels in shared memory). Trunk 0.2–0.5 Hz, branches 0.5–2 Hz, twigs 2–5 Hz. Output `BonePose` for `t` and `t − dt`.
  Budget ≤ 200k bones/frame.
- **Stateless by design:** every pose is a pure function of (time, wind parameters, seed) → previous-frame positions
  are re-evaluated, giving exact motion vectors; no per-tree state memory; deterministic.
- **Leaves/needles:** per-vertex flutter (3-bit weight), high-frequency rotation about the attachment; aspen/poplar
  flag → strong flutter at low wind speed. Parts follow their bone rigidly + flutter.
- Merged LOD: reduced rig (trunk + primary branches) as single-bone skinning (9-bit index). Far field: trunk sway only.
- v2 option: stateful springs for near trees (state pool keyed by tree id, seeded from the stateless pose on entry)
  for realistic gust after-sway.

### 5.7 Seasons and weather

- **Season LUT** per species × 64 time-of-year columns: leaf colour, leaf density, transmission tint, flower/fruit
  visibility. Per-tree offset from seed (early/late trees), per-leaf offset from the leaf bucket (leaves turn and fall
  individually).
- **Leaf density:** leaves whose bucket exceeds `density × 15` collapse to a point in raster (zero cost in SW raster)
  → autumn thinning → bare winter crowns. Impostors/voxels carry leaf coverage per bucket class.
- **Litter:** terrain RVT layer accumulating under deciduous trees in autumn. **Falling leaves:** wind-driven
  particles from near deciduous trees.
- **Snow:** terrain layer; on branches and conifers by shading (up-facing, sky-visible surfaces, noise mask); grass
  flattened/hidden by snow depth; RT proxies switch to leaf-off variants.
- **Wetness:** darker albedo, lower roughness for bark, leaves, rocks. **Crops:** growth stage per season.

### 5.8 Shading

- **Leaves (`FOLIAGE`):** front GGX specular (waxy cuticle) + diffuse; back: diffuse transmission =
  `transmission_tint × back-side light`, scaled by thickness; normals flipped for back faces.
- **Needles:** rounded normals, little transmission.
- **Bark:** `DEFAULT` with detail normal; moss mask on up/north-facing sides; birch white bark.
- **Grass (`GRASS`):** §5.5.
- **Geometric specular anti-aliasing** in the resolve (normal variance → roughness).
- One material-evaluation file shared by the raster resolve, RT hit shading (proxy: vertex-colour albedo +
  transmission) and the reference path tracer (full BSDF incl. diffuse transmission) — consistency between direct
  and indirect lighting depends on it.

### 5.9 Shadows and light under canopies

- VSM clipmap rendered by the cluster pipeline: animated vegetation in near levels, cached rest pose farther.
- **Dappled light:** sunflecks are pinhole images of the sun (0.53° wide): penumbra width ≈ 0.0093 × blocker
  distance (9 cm at 10 m). Requires blocker-distance-aware filtering (PCSS / SMRT-style) on the VSM, or RT shadows with
  sun-disk sampling in the quality tier. A fixed-width PCF kernel destroys the effect.
- Screen-space contact shadows for blade/leaf scale; grass in VSM only in the nearest levels; analytic root
  darkening for the rest.
- **Overcast days:** sky occlusion from the GI system defines the image — GI quality has priority over shadow
  sharpness for this biome.
- Light shafts: froxel fog sampling the VSM.

### 5.10 Ray-tracing representation

- RT proxy per species variant and leaf state (on/off): merged, simplified (~10–100k triangles), rest pose, vertex
  colours, leaves as simplified geometry or as cards with OMM; compacted BLAS.
- TLAS instances for trees within ~300–500 m (≤ ~100k); beyond, the terrain heightfield with canopy height.
- Grass is never in the BVH; its near-field occlusion comes from the screen-space term.
- NVIDIA experimental tier: RTX Mega Geometry (cluster acceleration structures from our geometry pages, animated
  assemblies) → exact animated foliage in RT, first for the reference path tracer.

### 5.11 Anti-aliasing and temporal stability

- LOD capped at ~1 triangle per pixel; voxels for sub-pixel aggregates; stochastic coverage + TAA.
- Exact motion vectors (re-evaluated wind); TAA with variance clipping and id-based rejection, tuned to avoid
  smearing foliage.
- Native 1080p by default (upscalers smear sub-pixel foliage); DLAA with the transformer model as NVIDIA option.

### 5.12 Vegetation share of the frame budget

| Item | ms |
|---|---|
| Wind field + bone poses | 0.3 |
| Tree / part / far-field culling and LOD | 0.8 |
| Grass generation + raster | 1.2 |
| Tree raster (HW + SW) + far field | 2.5 |
| VSM, vegetation share | 2.0 |
| Resolve, foliage + grass share | 1.5 |
| GI/reflection rays hitting vegetation proxies | ~2.0 |
| **Total vegetation-attributable** | **~10.3 of 27** |

## 6. Research items and watch list

- Switch point between foliage DAG simplification and voxels; aggregate-aware simplification of leaf clusters.
- Voxel attribute filtering (leaf area density, normal distribution, transmission) fitted to the reference.
- Stateful near-field wind vs stateless quality.
- Mega Geometry path on NVIDIA; portable alternative when cross-vendor cluster AS appears.
- Neural texture compression for leaf/bark atlases (NVIDIA RTXNTC: on Vulkan "inference on load" and "on sample";
  no shipping game as of April 2026).
- Neural/Gaussian representations for distant vegetation (watch only).
