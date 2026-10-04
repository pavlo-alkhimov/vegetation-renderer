# 11 — Architecture review (draft v0.1)

Scope: [02](02-architecture.md), [03](03-hardware-mapping.md), [04](04-data-representation.md),
[05](05-frame-pipeline.md), with the vegetation design in [06](06-vegetation.md) where it drives the architecture.
Method: read as a reviewer, trace every per-frame data item from producer to consumer, recompute the numbers.
Nothing here has been applied to the design docs yet; each finding ends with a proposed change.

## Verdict

The core is right for the content: cluster LOD DAG + two-phase GPU culling + 64-bit visibility buffer + material
resolve, stateless wind, one shared CPU/GPU header, a three-channel game↔renderer contract. Three things are missing
at the structural level (H1–H3) and must be decided before M1; the medium items are decisions or rules that the
docs leave open; the low items are numbers and wording.

| # | Severity | Area | Finding |
|---|---|---|---|
| H1 | High | 04, 05 | Two kinds of instances, one pipeline: tree **part instances** have no representation |
| H2 | High | 02, 04 | No stable render origin: camera-relative coordinates break every persistent world-space GPU structure |
| H3 | High | 04, 05 | Terrain renderer is undesigned although grass, occlusion and far field stand on it |
| M1 | Medium | 04 | Clusters vs wind bones: posed cluster bounds and DAG groups across bone boundaries |
| M2 | Medium | 05 | HW raster via atomics only forgoes early-Z; grass overdraw is the worst case for exactly that |
| M3 | Medium | 06 | Parts → merged threshold (200 px) is ~2× too low; drives the 0.5 M part-instance figure |
| M4 | Medium | 05, 03 | Async schedule leaves the graphics queue idle ~7 ms waiting on the GI chain |
| M5 | Medium | 02, 05 | `views[4]` without a multi-view design (PiP scopes, mirrors) |
| M6 | Medium | 05, 08 | GI as specified (ReSTIR GI + hash cache + denoiser) is the largest single risk; no v0 |
| M7 | Medium | 02, 04 | Streaming/residency correctness rules are not written down |
| M8 | Medium | 02 | Game → renderer back-call `render_alloc_handle` crosses the hot-reload boundary |
| L1 | Low | 02 | Latency estimate optimistic for FIFO at 30 Hz on a 60 Hz display |
| L2 | Low | 02, 03 | CPU decompression placed on the I/O thread; bursts need 2–3 cores |
| L3 | Low | 05 | VSM clipmap first level (8 m at 16k²) is never selected |
| L4 | Low | 04 | Vegetation cell residency tiers and cell base altitude undefined |
| L5 | Low | 02 | Render-target aliasing plan must be derived per configuration, not hand-written |
| L6 | Low | 04 | Cluster group ↔ geometry page containment unstated |
| L7 | Low | 02 | Editor live edits of cooked cells have no path |
| L8 | Low | 03, 06 | Grass raster budget (1.2 ms gen + raster) is tight; depends on M2 |

## High

### H1 — Part instances are not in the instance model

`GpuInstance` (64 B, persistent) covers props and dynamic objects. Trees are `TreeInstance` (16 B, cooked) ×
`PartInstance` (32 B, per species) × `BonePose` (per frame). `VisibleCluster.instance` is a `u32` into
`GpuInstance[]`, and the cluster culling and rasterizer read `GpuInstance.m[12]`. There is no description of how the
~0.5 M visible part instances per frame enter culling, rasterization and the resolve, nor what `instance` means for
them. This is the single largest gap: it is the path most triangles take.

Options:
1. **Transient instance table (recommended).** Pass 1 (vegetation instances) appends, for every part instance that
   survives tree-level culling, a `GpuInstance`-compatible record into a per-frame table placed after the persistent
   instances: `m = cell × tree × bone_pose(bone) × part_local`, `mesh = part mesh`, `flags |= TRANSIENT`.
   `VisibleCluster.instance` indexes the unified table `[persistent | transient]`. One code path for culling, raster,
   resolve and VSM. Cost: 0.5 M × 64 B = 32 MB written and read 2–3 times per frame ≈ 0.4 ms; 32 MB of the
   vegetation VRAM budget. Previous-frame transforms for motion vectors are recomputed in the resolve from
   `BonePose prev` and the part's (tree, part) reference stored in 8 spare bytes of the record.
2. Two-level reference `(tree, part)` composed on the fly in every consumer: less memory traffic, ALU and code in
   four places; rejected for complexity.

Change: 04 `GpuInstance` gets `u32 tree; u32 part;` (replacing `material_remap`+`flags` packing → 64 B holds it);
05 pass 1 writes the transient table; `vis64` index space documented as unified.

### H2 — No stable render origin

02/04: "the GPU never sees absolute world coordinates; per frame, a cell-origin table stores each resident cell's
offset relative to the camera". That is right for per-frame geometry, but the design also has **persistent**
world-space GPU structures: the radiance cache (spatial hash keyed by position), VSM clipmap pages cached across
frames (texels must stay fixed in world space), the trample/cut map, the wind field (advected gust fronts), the
canopy layer, ReSTIR reservoirs (reprojected by motion vectors — fine). If their coordinates move with the camera,
the caches are invalidated every frame.

Change: introduce the **render origin** = corner of the camera's 256 m cell, an `i32[3]` that changes only when the
camera crosses a cell boundary. All GPU world-space coordinates are relative to the render origin (`f32`, ≤ ±512 m
for near content, larger for far cells with reduced precision that no longer matters). Persistent structures:
radiance-cache keys include the origin cell (entries written under an old origin age out naturally); VSM clipmaps are
already snapped to texel grids — the page table shifts by an integer number of pages on an origin change; trample
map and wind field shift by integer texels. Per-frame "cell-origin table" becomes "cell offset from render origin"
and is constant between crossings. Document in 02 (contract: `FramePacket.render_origin`) and 04 (precision).

### H3 — Terrain is undesigned

Doc 04 has the height tile (257² `u16`), layer weights and the canopy layer; 05 has `vis64 kind 4 = terrain patch |
triangle` and "runtime virtual texture". Missing: patch LOD scheme, crack handling, how terrain patches are culled
and rasterized, RVT page table/feedback/compositing passes, height queries shared with the game (collision, grass
generation reads height + normal per blade), and the canopy-layer hand-off. Grass placement, statistical grass
occlusion, the far field and the RT terrain BLAS all depend on it.

Change (recommended): **cook terrain tiles into the same cluster DAG format** and render them through the same
cull/raster/resolve pipeline with a `TERRAIN` shading model — no separate terrain renderer, `kind 4` disappears,
crack-free by the DAG construction, HiZ occlusion for free. A 257² tile is 131k triangles at full detail; the DAG
gives ~1 px triangles everywhere. The RVT becomes a texture-space system only (page table, feedback from the
resolve, GPU compositing of layers/decals into the physical cache). Runtime height edits (editor) re-cook a tile's
DAG on the GPU or CPU in milliseconds (131k triangles). Height queries for grass and gameplay use the height tile
directly (shared function), not the mesh. Add a terrain section to 04 and the RVT passes to 05.

## Medium

### M1 — Clusters and wind bones

`PackedVertex.wind.bone` is per vertex (9 bits), but culling and LOD need a **posed cluster bound**. A cluster whose
vertices span two bones at a branch junction has no single rigid transform, and a DAG group that merges clusters
from different bones produces a parent with mixed influences — exactly the problem Nanite skinning solves with
per-cluster influence lists.

Change: one bone per cluster. The cooker partitions the mesh by bone before clustering and builds the DAG **per
rig level**: fine levels partitioned by fine bones, coarser levels by the reduced rig, with the hand-off levels where
the rig coarsens (twig bones → branch bone). `GpuCluster` gets `u16 bone` (there are 4 pad-free bytes in `flags`'
neighbourhood: shrink `flags` to `u16`); the vertex keeps flutter weight and leaf bucket (7 bits) and frees 9 bits.
Part meshes have `bone = 0` (rigid to `PartInstance.bone`).

### M2 — HW raster: atomics only vs depth test

05 rasterizes the HW path with a fragment shader doing a 64-bit `atomicMax`, no attachments (Nanite's choice, so
HW and SW write the same target). This gives up early-Z. Grass is the worst overdraw case in the project: a meadow
at eye level has depth complexity 20+ in the horizon band, and every hidden fragment still costs an L2 atomic.
Estimate for 1 M visible blades: ~30 M atomics ≈ 1 ms on the 4060 before any tree.

Alternative: HW path with a `D32` depth attachment + `R32_UINT` payload attachment (classic visibility buffer),
early-Z rejects hidden fragments in the ROP; SW raster keeps the 64-bit atomic buffer; a merge step (or the SW
raster testing against the HW depth) unifies them. Cost: one extra pass and two code paths for the payload.

Change: do not fix this in the design. Implement both in M1 (the HW variant is ~a day), measure on scene 1
(meadow) in M3, then delete one. Record in 05 as an open decision with the measurement that closes it.

### M3 — Parts → merged threshold

06 switches from assemblies to the merged tree below 200 px tree height (≈ 93 m for a 20 m tree). At 93 m one
pixel is 9.9 cm; a 30 cm needle spray is 3 px tall, yet it still owns ≥ 1 cluster (its DAG root, 8–32 triangles):
2000 parts × ~16 triangles ≈ 32k triangles on ~18k px² → ~2 triangles per pixel, and this is where the 0.5 M
visible part instances come from.

Change: switch when the **median part** is < ~8–16 px (≈ 20–40 m for a 30 cm spray), i.e. `px_merged` ≈ 400–600
for a 20 m conifer; the merged DAG stores only levels coarser than the switch (≈ 100k triangles finest level for a
spruce; the full 2–4 M-triangle assembly is never stored merged). Visible part instances drop to ~0.1 M. This is
also exactly the band where material Gaussians per spray ([09](09-gaussian-splatting.md)) compete.

### M4 — Async compute schedule

Serialized sum = 27 ms. On the drawn two-queue schedule the critical path is visibility (6.3) → classify + resolve
(2.5) → GI + reflections + clouds/fog (9.5) → composite/TAA/post (3.2) ≈ 21.5 ms, while the graphics queue idles
~7 ms after direct lighting waiting for `c2`. The budget correctly does not count on overlap, but the diagram
presents an unbalanced split as the design.

Change: label the queue split as an initial guess; list the balancing moves (reflections, SSILVB and the GI denoiser
on graphics; async keeps TLAS build, GI rays + ReSTIR, clouds, fog, atmosphere LUTs); decide by GPU Trace in M4.

### M5 — Multi-view

`ViewDesc views[4]` exists, nothing says what views 1–3 are or how they are rendered. Picture-in-picture scopes
(standard in mil-sims), mirrors and planar reflections each need their own frustum culling and a visibility pass;
they are not shadow views.

Change: either `views[1]` for now, or define secondary views as reduced-resolution visibility + resolve + lighting
passes sharing the scene state but not the culling output, with their own `vis64` and depth, composited into the
main view before TAA. Gameplay decision (needs a scope? [00](00-task.md)).

### M6 — GI implementation order

ReSTIR GI + world-space hash radiance cache + temporal/spatial denoiser + upsample is the longest single item in the
plan and the one with the least certain outcome on a 4060. There is no v0.

Change: M4a = radiance-cache-only GI (primary hits look up the cache; cache updated by the 130k rays; screen-space
near field) + sky visibility — ships a complete, stable image; M4b = ReSTIR GI on top, kept only if FLIP and
stability improve for ≤ 2 ms. DDGI probes remain the non-RT fallback. Update 08.

### M7 — Streaming and residency rules

Implicit in 02/04/05, must be explicit because every one of them is a class of GPU hangs or corruption:
1. Residency tables (geometry pages, texture mips, cells) are updated **only in pass 0**, from transfers whose
   timeline value the graphics queue has waited on this frame.
2. Every GPU resource free (page evicted, texture reallocated, pipeline replaced by hot reload, descriptor slot
   reused) goes through a per-frame-slot **deferred-free list**, executed when the slot's fence of two frames ago
   has passed.
3. The culling traversal **emits page requests** when it wants to descend into a non-resident page and draws the
   coarsest resident ancestor instead (error > τ accepted); requests are read back with the texture feedback.
4. Bindless descriptor writes use `updateAfterBind`/`partiallyBound`; a slot is rewritten only after its deferred
   free has run.

### M8 — Handle allocation direction

`render_alloc_handle(Renderer*)` is called by the game, so the game depends on a renderer function across the
hot-reload boundary (it would have to live in `PlatformApi`).

Change: the game owns handle allocation (index + generation, its own free list); `SceneCmd CREATE` carries the
handle; the renderer maps handle → GPU slot. The game never calls the renderer.

## Low

- **L1 Latency:** with FIFO at 30 FPS on a 60 Hz display: CPU ~4 + GPU ~27 + vblank wait 0–16 + scanout 16 ≈
  50–65 ms (1.5–2 frames), not 45–55. Lever: `VK_EXT_present_timing` / VRR displays.
- **L2 Decompression:** Zstd runs 1–2 GB/s per core; the 3 GB/s burst needs 2–3 cores → decompression is a worker
  job, the I/O thread only submits and completes.
- **L3 VSM levels:** level 0 = 8 m at 16k² = 0.5 mm/texel; marking never selects levels 0–2 (a shadow texel should
  ≈ a screen pixel: 8.5 mm at 8 m). First level 32–64 m, 10–11 levels to 16–32 km; same 128 MB pool. Note that
  low sun (the biome's normal case) multiplies page demand.
- **L4 Cells:** vegetation cell residency has tiers (full tree list ≤ ~5 km, canopy layer only beyond); a cell has
  a base altitude so `TreeInstance.pos[2]` (u16 over 256 m) is well defined.
- **L5 Aliasing plan:** derived from a declared per-pass resource list for the active configuration (RT on/off,
  DLSS, 60 FPS tier, single queue, debug views), recomputed on any change; the pass list is hand-ordered, the plan
  is not hand-written.
- **L6 Groups and pages:** a DAG group must lie within one 64 KB page (cooker constraint; groups of 8–16 clusters
  at ~2.6 KB each fit), or leaf nodes carry a page range.
- **L7 Editor edits:** painting density, placing trees and moving height live requires a "cell patch" path (CPU
  edit → re-cook affected tile/cell data → upload) that bypasses the pak files; decide whether the editor is in
  scope for M1–M3.
- **L8 Grass budget:** see M2; keep 1.2 ms as the target, expect 2 ms with atomics only.

## What is sound

- Visibility buffer + cluster DAG + two-phase culling as the one geometry path for everything, including shadows.
- Stateless wind → exact motion vectors, no per-tree state, deterministic.
- One `gpu_shared.h`, size-checked layouts; cooked = runtime image.
- The three-channel packet contract; static objects cost nothing; readbacks delayed two frames.
- Budgets exist for every pass and pool and are labelled as guesses to be measured.
- Information fairness as an architectural rule rather than a settings policy.

## Proposed edits (for approval)

| Doc | Edit |
|---|---|
| 02 | render origin in the contract; game-owned handles; deferred-free and residency rules; latency numbers; decompression on workers; aliasing plan derivation; multi-view decision |
| 04 | `GpuInstance.tree/part`, transient instance table; `GpuCluster.bone`, vertex `wind` repack; terrain tiles as cluster DAGs; group ≤ page; cell tiers and base altitude |
| 05 | pass 1 writes the transient instance table; `kind 4` removed (terrain = kind 0); RVT passes; HW raster open decision (M2); queue split labelled as initial, balancing moves; VSM level range |
| 06 | `px_merged` ≈ 400–600 for a 20 m tree; merged DAG stores only coarse levels; part-instance estimate ~0.1 M |
| 08 | M4 split into M4a (cache-only GI) and M4b (ReSTIR); M1 includes both HW raster variants |
