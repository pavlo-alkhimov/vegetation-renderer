# 08 — Validation and roadmap

## Measurement

- **Always on:** GPU timestamps per pass, CPU phase timers, VRAM usage vs `VK_EXT_memory_budget`, counters (visible
  clusters, HW/SW triangles, grass blades, trees per LOD class, rays, VSM pages rendered).
- **Tools:** Tracy (CPU + GPU timeline), Nsight Graphics GPU Trace (SM throughput, L2 hit rate, RT-core utilization,
  warp stall reasons), RenderDoc (correctness), Vulkan validation incl. synchronization and GPU-assisted validation.
- **Method:** p50/p95/p99 frame times and per-pass means over deterministic camera paths; locked GPU clocks
  (`nvidia-smi --lock-gpu-clocks`) or steady-state runs for A/B comparisons; Linux and Windows on the same build.

## Reference path tracer

- Same scene data and the **same material-evaluation code** as the real-time path; RT pipelines + SER; progressive
  accumulation; time and wind frozen so a real-time frame can be compared at the same timestamp.
- Geometry: full detail via Mega Geometry on NVIDIA where possible, otherwise RT proxies (the comparison then isolates
  lighting from geometry).
- Metrics: ꟻLIP image difference; temporal stability (frame-to-frame difference on a static camera, wind on/off);
  mean luminance under canopies (catches GI bias).
- Used for: GI/denoiser tuning, voxel attribute fitting, LOD error thresholds, far-field transition checks.

## Benchmark scenes (target biome)

| # | Scene | Stresses |
|---|---|---|
| 1 | Meadow + forest edge, summer golden hour: grasses and flowers, hazel/blackthorn belt, oak and beech, gusty wind | grass, backlit translucency, gust fronts |
| 2 | Beech forest interior: sunny and overcast; summer, autumn, leaf-off winter | canopy GI, dappled light, seasons, bare twig LOD |
| 3 | Carpathian slope: spruce, fir, beech; valley mist; snow variant | needles, fog, snow |
| 4 | Scots pine and birch on sand: high crowns, heather/blueberry/moss floor | open canopy, ground cover |
| 5 | Forest-steppe and fields: feather grass, wheat, sunflower; vista to 5–10 km | wind waves, far field, canopy layer |
| 6 | Stress: maximum density, all LOD ranges, fast camera | streaming, worst-case culling |

Each scene: fixed camera path, fixed time of day / season / wind seeds; output CSV + screenshots + reference diffs.
Content bootstrap: own procedural plant generator / Houdini / Blender geometry nodes, CC0 libraries (e.g. Poly Haven);
check licences of Fab/Megascans/Megaplants assets before using them outside Unreal Engine.

## Milestones

| M | Content | Exit criterion |
|---|---|---|
| M0 Skeleton | platform layers (Linux + Windows), Vulkan device/swapchain, upload ring, GPU timers, hot reload of game and shaders, debug UI | runs on both OSes; debug build < 3 s |
| M1 GPU-driven core | cooker (glTF → clusters, textures → BC), pack files, bindless, GPU scene, visibility buffer (HW raster), two-phase culling, material resolve, sun + sky, CSM via cluster raster, TAA | static forest scene; first measured budgets |
| M2 Virtual geometry | DAG LOD, page streaming, SW raster, VSM clipmap | 10× M1 geometry at equal cost |
| M3 Vegetation I | species pipeline (assemblies, rig), wind field + bones, grass blades, ground-cover placement, impostor far field, foliage shading, seasons v1 | scenes 1, 2, 5 at ≥ 30 FPS |
| M4 Ray tracing | RT proxies + OMM, TLAS, RT GI (cache, ReSTIR GI, denoiser), reflections, reference path tracer, FLIP tooling | GI within budget; reference diffs |
| M5 Atmosphere and weather | clouds, froxel fog, valley mist, rain/wetness, snow, basic water | scenes 2–3 all variants |
| M6 Output | DLAA/RR option, HDR10, post stack, latency pacing | — |
| M7 Vegetation II (research) | aggregate representations ([09](09-gaussian-splatting.md)): needle sprays and twig crowns (geometry vs material Gaussians), far field (impostors vs voxels vs Gaussians); stateful wind, Mega Geometry path, NTC experiment | measured against M3 |
| M8 Scaling | low tier without RT, 60 FPS tier, AMD (RADV) and Intel validation | — |

## Open questions (assumptions in brackets)

1. Photoreal or stylized? [photoreal]
2. World size and view distance? [≤ 16 × 16 km, vistas to 5–10 km]
3. Time of day and seasons dynamic at runtime? [both dynamic → no baked lighting]
4. RT hardware required for the target tier? [yes; non-RT tier later]
5. Interaction depth: trampling, grass cutting, tree felling? [trampling, cutting, tree-removal events]
6. Native 1080p or upscaled? [native + TAA/DLAA]
7. Authoring tools? [Blender + Houdini; SpeedTree optional]
8. A game beyond viewer/editor (characters, physics)? [viewer + editor + one character as interactor]
