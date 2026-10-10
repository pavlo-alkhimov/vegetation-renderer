# 03 — Hardware mapping and budgets

All numbers here are **initial allocations to be replaced by measurements** (M0–M4, see
[08](08-validation-roadmap.md)). They exist so that every design decision can be checked against a budget.

## Test GPU: RTX 4060 (AD107, Ada)

| Resource | Spec | Per frame @ 30 FPS | Per pixel per frame @ 1080p |
|---|---|---|---|
| FP32 | 24 SM × 128 lanes × 2 (FMA) × 2.46 GHz ≈ 15.1 TFLOPS | 504 GFLOP | ~243 kFLOP (4K60: ~30 kFLOP) |
| DRAM bandwidth | 8 GB GDDR6, 128-bit, 17 Gbps → 272 GB/s | 9.1 GB peak, ~6–7 GB usable | ~4.4 KB peak |
| L2 | 24 MB | — | a 1080p RGBA16F target (16.6 MB) fits |
| Texture | 96 TMUs → ~236 Gtexel/s bilinear | 7.9 Gtexel | ~3.8k bilinear fetches |
| ROP | 48 → ~118 Gpixel/s | 3.9 Gpixel | ~1.9k writes |
| RT cores | 24 (3rd gen: opacity micromaps, SER; displacement micromaps deprecated) | — | — |
| Tensor cores | 96 (4th gen, FP8) | DLSS, cooperative vectors | — |
| SM | 4 partitions, 64K × 32-bit registers, 128 KB L1/shared (≤ 100 KB shared), ≤ 48 warps, ≤ 24 workgroups | — | — |
| Host link | PCIe 4.0 **×8** → 15.75 GB/s per direction (~13 practical) | ~430 MB peak; plan ≤ 100 MB | — |
| Power | 115 W | clocks vary with load → lock clocks for A/B runs | — |

### The 1080p30 argument, corrected

- 4K60 / 1080p30 = 4× pixels × 2× frames = **8×**, not 16×.
- Only per-pixel work (shading, GI rays per pixel, post) gets 8×. **Per-frame work** — culling, LOD selection,
  animation, BVH builds, shadow rasterization of the world — scales with scene complexity and gets only **2×**.
  Dense vegetation is dominated by per-frame work.
- Shipping "4K60" titles render ~1080p–1440p internally and upscale; against them the per-pixel advantage is ~2–4×.
- 30 FPS halves the temporal sample rate per second: temporal accumulation (TAA, ReSTIR, denoisers) converges half as
  fast in wall time and sees 2× larger motion per frame → more disocclusion. Spend part of the gain on more samples
  per frame and stronger spatial reuse, not only on longer history.
- Frame generation from a 30 FPS base gives poor latency and artifacts — not part of the design.

## Concept → hardware → Vulkan

| Engine concept | Hardware | Vulkan mechanism | Notes |
|---|---|---|---|
| Persistent scene data | VRAM | device-local buffers, addressed by BDA (`u64`) | no vertex/index buffers, no buffer descriptors |
| Per-frame CPU → GPU data | PCIe + ReBAR window | `DEVICE_LOCAL\|HOST_VISIBLE` ring, sequential CPU writes | fallback: staging + copy |
| Streaming | copy engines (DMA) | dedicated transfer queue + timeline semaphore | runs continuously, independent of frames |
| Culling, LOD, instance expansion | SMs | compute, chains of `vkCmdDispatchIndirect` | level-by-level; no persistent threads (no forward-progress guarantee in Vulkan) |
| HW raster (triangles ≳ 8 px) | raster engines | mesh shaders; fragment shader does 64-bit atomic max, no attachments | early-Z not used; two-phase occlusion limits overdraw |
| SW raster (triangles ≲ 8 px, voxels) | SMs | compute, 64-bit buffer atomics | Nanite reported ~3× over its best HW path for micro-triangles |
| Material resolve | SMs + TMUs | compute over 8×8 tiles, binned by shading model | explicit texture gradients |
| Ray tracing | RT cores | `VK_KHR_ray_query` in compute | OMM for alpha-tested proxies |
| Reconstruction | SMs (own TAA) or tensor cores (DLSS) | compute / NGX | — |
| Overlap | idle SMs during raster-bound passes | async compute queue + timeline semaphores | NVIDIA gains are modest — must work identically on one queue |
| Presentation | display engine | swapchain FIFO, present timing | — |

### Queues

| Queue | Family | Work |
|---|---|---|
| Graphics ×1 | graphics+compute+transfer | culling, visibility raster, VSM, resolve, direct lighting, transparents, TAA, post, present |
| Async compute ×1 | compute-only | TLAS build, GI, reflections, clouds, volumetric fog (initial split — rebalanced in M4, see [05](05-frame-pipeline.md)) |
| Transfer ×1 | transfer-only (copy engines) | streaming uploads |

One timeline semaphore per queue. Frame N signals well-defined values at hand-off points; the CPU waits on the graphics
timeline for frame N-2 before reusing a frame slot.

## Shader execution rules (Ada, valid in spirit for RDNA)

| Registers / thread | Resident warps / SM | Occupancy |
|---|---|---|
| ≤ 40 | 48 | 100 % |
| 64 | 32 | 67 % |
| 96 | 21 | ~44 % |
| 128 | 16 | 33 % |

- Default workgroup 64 threads (2 warps / 1 wave64); screen passes as 8×8 tiles; mesh shaders 128 threads (one
  thread per vertex/triangle of a ≤ 128-triangle cluster); reductions/scans 256.
- Full occupancy with 64-thread groups needs ≤ ~4 KB shared memory per group (100 KB / 24 groups).
- Targets: screen passes ≤ 64 registers, ray-query passes ≤ 96 (ray state is large). Check in Nsight before tuning.
- Append-style outputs use one atomic per subgroup (ballot + prefix), never one per thread.
- Memory transactions are 32-byte sectors: GPU records are 16/32/64 B; no scattered 4-byte reads in hot loops.
- Divergence is handled by **binning** (raster path per cluster, shading model per tile, ray type per queue), not by
  per-pixel branching.
- A full-screen RGBA16F read+write at 1080p moves 33 MB ≈ 0.14 ms. Bandwidth is cheap at this resolution; ALU, latency
  and ray traversal are the real costs. Keep working sets L2-resident by processing in screen tiles.

## GPU frame budget — target tier, RTX 4060, native 1080p, 30 FPS

| Stage | ms |
|---|---|
| Frame begin: upload scatter, wind field, tree bone poses | 0.5 |
| Terrain RVT: page compositing + runtime BC compression | 0.4 |
| Culling + LOD: objects, trees, transient part instances (~0.1 M), terrain patches, clusters (phase 1 + 2) | 1.5 |
| Visibility raster: HW + SW clusters, terrain patches, grass blades, far-field vegetation | 4.0 |
| HiZ build, depth export | 0.3 |
| Virtual shadow maps: page marking + raster of invalidated pages | 3.0 |
| Material classify + resolve → G-buffer, motion vectors | 2.5 |
| Direct lighting: sun + sky + local lights, contact shadows, foliage transmission | 1.5 |
| TLAS build | 0.5 |
| GI: rays, radiance-cache update, ReSTIR GI, denoise, upsample | 6.0 |
| Reflections (glossy + mirror) | 1.5 |
| Screen-space near-field occlusion/indirect (grass, small leaves) | 0.8 |
| Atmosphere LUTs, clouds, froxel fog | 2.0 |
| Transparents, particles, water | 1.0 |
| TAA (or DLAA) | 1.0 |
| Post: exposure, bloom, tone mapping / HDR, grading, UI | 0.9 |
| **Total** | **27.4** |
| Headroom (spikes, streaming, clock variance) | 5.9 |

Grass is the tightest line inside "visibility raster": ~1.2 ms assumes HW raster variant B (early-Z); with variant A
(atomics only) expect ~2 ms ([05](05-frame-pipeline.md), open decision closed in M3).

A 60 FPS tier (16.7 ms) halves GI resolution again, drops RT reflections, renders clouds at lower rate and limits VSM
page updates (~14 ms). Async-compute overlap may return 1–3 ms; the budget does not count on it.

**Ray budget:** GI 518k rays (quarter resolution) + radiance-cache update ~130k (+ shadow rays) + reflections
~130–260k ≈ **1 M rays/frame**. Assumption to verify in M4: ≥ 0.5 Grays/s incoherent in our scenes → ≤ 2 ms traversal.

## VRAM budget (8 GB card)

| Item | MB |
|---|---|
| Render targets (aliased) + VSM physical pool + DLSS internals | 500 |
| Geometry page pool | 1000 |
| Texture pool (incl. 128 MB terrain RVT physical cache) | 2000 |
| Ray tracing: BLAS, TLAS, opacity micromaps, scratch | 800 |
| Vegetation + terrain: cells, height mips, transient part instances (~7 MB), cluster lists, bone poses, transient grass lists | 300 |
| GI caches, atmosphere/cloud LUTs and noise volumes | 150 |
| Upload/readback rings (device-local part) | 100 |
| Pipelines, descriptors, driver internals | 200 |
| **Application total** | **5050** |
| Headroom | ~950 |
| **Ceiling** | **6000** |

The OS, compositor and other applications take 1–2 GB on an 8 GB desktop card. The real budget is read from
`VK_EXT_memory_budget` at startup and continuously; texture and geometry pools scale to fit. Reference point:
dynamic-vegetation BLASes in Indiana Jones needed 1027 MB before and 606 MB after compaction — RT memory for
vegetation is a first-order budget item.

## Render targets at 1920×1080

| Target | Format | MB | Lifetime |
|---|---|---|---|
| Visibility buffer | `u64` buffer (depth:32 \| payload:32) | 16.6 | frame |
| Depth (for HW-depth-tested passes) | D32F | 8.3 | frame |
| Previous depth | R32F | 8.3 | history |
| HiZ | R32F mips from 960×540 | 2.8 | history |
| Motion vectors | RG16F | 8.3 | frame |
| G-buffer albedo / transmission | RGBA8 | 8.3 | frame |
| G-buffer normal (octahedral) | RG16 | 8.3 + 8.3 | frame + history |
| G-buffer material (roughness, specular, translucency, model id) | RGBA8 | 8.3 | frame |
| HDR lighting | RGBA16F | 16.6 | frame |
| TAA history | RGBA16F | 2 × 16.6 | history |
| GI diffuse, quarter resolution, with history/temp | RGBA16F | 16.6 | history |
| Reflections, quarter resolution, with history/temp | RGBA16F | 16.6 | history |
| ReSTIR GI reservoirs (32 B, quarter resolution, 2 frames) | buffer | 33 | history |
| Froxel volume 160×90×64 (+ history, integration) | RGBA16F | 22 | history |
| Clouds 480×270 (+ history) | RGBA16F | 3 | history |
| Bloom chain, swapchain ×3, UI | — | 39 | frame |
| **Total before aliasing** | | **~258** | |

Plus VSM physical pool (8192 × 4096 × D32 = 128 MB, 2048 pages of 128²) → ~390 MB, inside the 500 MB line above.

## CPU mapping

- Assumed host: 6–8 cores / 12–16 threads, x86-64-v3, NVMe SSD.
- Threads: main + (cores − 2) workers + 1 I/O.
- Main-thread budget excluding simulation: **< 4 ms** (packet → uploads → record → submit).
- The CPU does no per-object rendering work: no CPU culling, no per-draw command recording. CPU-heavy work is
  streaming decisions, decompression and (offline) cooking.
- Data: 64-byte cache lines, linear traversal, SoA in hot loops, no pointer chasing. SIMD (AVX2) only where profiling
  shows a hot loop.
- Large pages for big arenas are optional (`MADV_HUGEPAGE` / `MEM_LARGE_PAGES`).
- Streaming: NVMe 3–7 GB/s, Zstd-compressed pages (geometry ~2:1, BC textures ~1.3:1); sustained upload ≤ 100 MB per
  frame (~3 GB/s), bursts during loads.
