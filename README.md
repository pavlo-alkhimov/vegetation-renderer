# vegetation-renderer

Research real-time renderer for dense **temperate vegetation of Central and Eastern Europe** — meadows, steppe grass,
crop fields, beech/oak/hornbeam/birch forests, spruce/fir/pine stands — targeting the best achievable image quality at
**1920×1080, ≥ 30 FPS** on current consumer GPUs.

**Status:** design draft v0.1 (2026-10-04). No code yet. The documents fix the data flow between subsystems and the
mapping to hardware; per sub-problem one technique is chosen and the alternatives are recorded.

## Scope

1. Engine structure: renderer pipeline and its contract with the simulation ("game").
2. Data representation: DCC sources → cooked assets → GPU runtime layouts.
3. Techniques: best available (October 2026) for 1080p30 — vegetation and grass first.
4. Style: data-oriented, hardware-aligned, no unnecessary abstraction (Muratori/Blow); C-style C++.

## Hardware, OS, API

- Dev/test GPU: RTX 4060 8 GB (Ada AD107). Used for measurement, not as a min-spec decision.
- API: Vulkan 1.4, single backend.
- OS: Linux primary, Windows kept building and benchmarked (same code; only the platform layer differs).
  Reasoning in [01](docs/01-platform-and-api.md).

## Key decisions (draft)

| #  | Area | Decision | Main alternative | Doc |
|----|------|----------|------------------|-----|
| 1  | GPU API | Vulkan 1.4 only, no RHI abstraction | D3D12 + Vulkan behind an RHI | [01](docs/01-platform-and-api.md) |
| 2  | OS | Linux dev, Windows build always green, A/B benchmarks | Windows dev | [01](docs/01-platform-and-api.md) |
| 3  | Shaders | Slang → SPIR-V, one shared C/Slang header for GPU structs | HLSL via DXC | [01](docs/01-platform-and-api.md) |
| 4  | Engine structure | Platform layer + hot-reloadable game + renderer; one-way `FramePacket`, delayed readbacks | engine framework / ECS | [02](docs/02-architecture.md) |
| 5  | Pipelining | 2 frames in flight, no render thread, paced input sampling | game/render/RHI threads | [02](docs/02-architecture.md) |
| 6  | Memory | CPU arenas; GPU: few large blocks + own sub-allocators; buffer device addresses everywhere | VMA, per-resource allocations | [02](docs/02-architecture.md), [03](docs/03-hardware-mapping.md) |
| 7  | Geometry | Cluster LOD DAG (Nanite-like), GPU-driven two-phase culling, 64-bit visibility buffer, HW mesh-shader + SW compute raster | discrete LODs + multi-draw-indirect + G-buffer | [05](docs/05-frame-pipeline.md) |
| 8  | Foliage geometry | Geometric leaves/needles, assemblies (instanced twigs), alpha cards only as fallback | alpha-tested cards | [06](docs/06-vegetation.md) |
| 9  | Far vegetation, aggregates | Octahedral impostors (phase A) → voxels or material Gaussians (phase B); needle/twig parts switch to aggregates by element size | billboards | [06](docs/06-vegetation.md), [09](docs/09-gaussian-splatting.md) |
| 10 | Grass | Per-frame procedural Bézier blades from mesh shaders; regenerated from ID in material resolve; nothing stored | stored instances / grass cards | [06](docs/06-vegetation.md) |
| 11 | Wind | GPU wind field with travelling gust fronts + per-tree bone rig; stateless evaluation → exact motion vectors | vertex-shader sine waves | [06](docs/06-vegetation.md) |
| 12 | Seasons | First-class: per-species colour ramps, leaf density (autumn → leaf-off winter), snow | fixed summer look | [06](docs/06-vegetation.md) |
| 13 | Shadows | Virtual shadow map clipmap rendered by the cluster rasterizer + screen-space contact shadows | CSM; RT shadows | [05](docs/05-frame-pipeline.md) |
| 14 | GI | Ray-query GI + world-space radiance cache + ReSTIR GI + denoiser; screen-space near-field term | SDF/voxel GI; DDGI probes | [05](docs/05-frame-pipeline.md), [07](docs/07-techniques-survey.md) |
| 15 | RT for vegetation | Per-species RT proxies (rest pose, opacity micromaps); grass not in BVH | animated BVH via RTX Mega Geometry (NVIDIA-only) | [06](docs/06-vegetation.md) |
| 16 | AA / upscaling | Native 1080p + own TAA; DLAA / DLSS-RR as NVIDIA option | upscale from 720p | [05](docs/05-frame-pipeline.md) |
| 17 | Validation | Built-in reference path tracer + FLIP; deterministic benchmark camera paths | visual inspection | [08](docs/08-validation-roadmap.md) |
| 18 | Concealment | Statistical grass occlusion beyond the blade radius, identical for rendering and AI; settings never change information | per-setting grass distance (genre default) | [10](docs/10-milsim-survey.md) |

## Throughput note

4K60 vs 1080p30 is **8×**, not 16× (4× pixels × 2× frame time). Only per-pixel work gains 8×. Per-frame work —
culling, geometry, animation, BVH maintenance, shadow rasterization, i.e. most of the vegetation cost — gains only 2×.
Shipping "4K60" titles render ~1080p–1440p internally and upscale, so the practical per-pixel advantage over them is
~2–4×. Details: [03](docs/03-hardware-mapping.md).

## Documents

| Doc | Content |
|-----|---------|
| [00 Task](docs/00-task.md) | The task as understood: goal, scope, targets, constraints, assumptions to confirm |
| [01 Platform and API](docs/01-platform-and-api.md) | Linux vs Windows on NVIDIA (2026), Vulkan feature baseline, shader language, toolchain |
| [02 Architecture](docs/02-architecture.md) | Subsystems, threads, frame timeline, memory, game ↔ renderer data contract, code conventions |
| [03 Hardware mapping](docs/03-hardware-mapping.md) | RTX 4060 numbers, GPU ms budget, VRAM budget, queues, occupancy rules, CPU mapping |
| [04 Data representation](docs/04-data-representation.md) | DCC → source → cooker → pack files; mesh/cluster, texture, material, vegetation, terrain layouts |
| [05 Frame pipeline](docs/05-frame-pipeline.md) | Pass list, queues, inter-pass resources, culling/visibility buffer, lighting, GI, post, sync |
| [06 Vegetation](docs/06-vegetation.md) | **Survey of vegetation/grass techniques (2017–2026) and the chosen design**, biome, seasons |
| [07 Techniques survey](docs/07-techniques-survey.md) | Non-vegetation state of the art per domain with chosen option |
| [08 Validation and roadmap](docs/08-validation-roadmap.md) | Measurement, reference path tracer, benchmark scenes, milestones, open questions |
| [09 Gaussian splatting](docs/09-gaussian-splatting.md) | 3DGS by subset: needle/twig aggregates and far field as candidates; captured splats offline or fixed-lighting only |
| [10 Mil-sim survey](docs/10-milsim-survey.md) | Grass/vegetation in Arma Reforger and five other photoreal mil-sims; concealment fairness; statistical grass occlusion |
| [References](docs/references.md) | Talks, papers, specs, SDKs |
