# vegetation-renderer

Research real-time renderer for dense **temperate vegetation of Central and Eastern Europe** — meadows, steppe grass, crop fields, beech/oak/hornbeam/birch forests, spruce/fir/pine stands — targeting the best achievable image quality at **1920×1080, ≥ 30 FPS** on current consumer GPUs.

**Status:** design draft v0.2 (2026-10-09, architecture review applied). The documents fix the data flow between subsystems and the mapping to hardware; per sub-problem one technique is chosen and the alternatives are recorded. First code: a terrain viewer with a vegetation baseline — trees and grass via task/mesh shaders, scanned ground-cover plants near the camera, TAA and sun shadows (milestones M0a–M0c in [08](docs/08-validation-roadmap.md)) — see below.

## Scope

1. Engine structure: renderer pipeline and its contract with the simulation ("game").
2. Data representation: DCC sources → cooked assets → GPU runtime layouts.
3. Techniques: best available (October 2026) for 1080p30 — vegetation and grass first.
4. Style: data-oriented, hardware-aligned, no unnecessary abstraction (Muratori/Blow et al.); C-style C++.

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
| 19 | Terrain | Quadtree heightfield patches (CDLOD morphing) as procedural clusters in the same cull/raster/resolve pipeline; adaptive runtime virtual texture; one shared height function for rendering, grass and gameplay | cooked cluster DAG per cell; Nanite-style landscape | [12](docs/12-terrain.md) |
| 20 | Coordinates | Render origin snapped to the camera's 256 m cell; all GPU world-space data relative to it, so persistent caches survive camera motion | camera-relative every frame | [02](docs/02-architecture.md) |

## Viewer: terrain + vegetation baseline (first code)

Fly or walk over real 1 m LiDAR terrain (Bavarian DGM1, [13](docs/13-reference-maps.md)). CDLOD heightfield patches (8×8 quads, morphing, [12](docs/12-terrain.md)) selected on the CPU and drawn through the vertex pipeline: an interim path until the GPU-driven visibility buffer of M1. Placeholder shading: meadow/forest from a procedural vegetation mask, slope soil/rock, sun + sky + aerial fog. Vegetation baseline ([06 §5.13](docs/06-vegetation.md)): 3.3 M procedural trees of 5 species with 4 LODs as meshlets (task shader culls and picks the LOD, mesh shader outputs meshlets) and grass blades generated every frame. Ground cover v1 ([06 §5.14](docs/06-vegetation.md); *ground cover* = all small non-woody plants): Poly Haven CC0 scans (grasses, dandelion, nettle, celandine, periwinkle, fern) as meshes around the camera, impostors to 120 m, a baked top view of the real mix on the terrain everywhere; crouch and prone stances; TAA; cascaded sun shadows. 9–14 ms GPU at 1080p on the RTX 4060. Trees and grass need `VK_EXT_mesh_shader`; without it the viewer shows terrain only.

Dependencies (Linux): clang, Vulkan headers + loader (`libvulkan-dev`; the loader is opened at runtime through SDL), SDL3 (`libsdl3-dev` on Debian 13 / Ubuntu 25.04+, `SDL3-devel` on Fedora, `sdl3` on Arch; on Ubuntu 24.04 build it from source and set `PKG_CONFIG_PATH`), `slangc` (LunarG Vulkan SDK ≥ 1.3.296 or a Slang release; set `SLANGC` if it is not on `PATH`).

```sh
./build.sh release                                   # build/vr, build/cook_terrain, build/shaders/terrain.spv
tools/fetch-bavaria.sh bbox 701 5503 716 5518        # once: 256 DGM1 tiles, ~1 GB
build/cook_terrain data/external/bavaria/dgm1        # -> data/cooked/terrain.vrh (16000², u16, 512 MB; ~15 s, ~3 GB RAM)
tools/fetch-polyhaven.sh                             # once: 8 CC0 plant scans, ~45 MB (2k textures)
build/cook_ground_cover data/external/polyhaven/*    # -> data/cooked/ground_cover.vgc (341 MB; ~2 s); without it no ground cover
build/vr                                             # or: build/vr <file.vrh> [--cam E N ALT YAW PITCH] [--novsync] ...
```

| Input | Action |
|---|---|
| click / Esc | capture mouse / release (Esc again quits) |
| mouse, WASD | look, move (physical key positions, so QWERTZ works) |
| Q / E (Space) | down / up |
| Shift / Ctrl, wheel | ×8 / ×⅛, change base speed |
| Z, X, F (G), C | camera presets: lying (eye 0.35 m), standing (1.75 m), free flight (G toggles); C crouch (1.0 m). Changes glide: ~0.7 s between stances, from free flight the camera descends to the ground |
| 1–4, L | shaded, LOD levels (terrain and trees), 10 m contours, normals; wireframe (terrain) |
| T, B, - / = | trees on/off, ground cover on/off (meshes + impostors; the terrain keeps its baked top view), tree draw distance ÷/× 1.25 (default 3 km) |
| , / . | ground-cover distance factor ÷/× 1.25 (0.5–8, default 2; `--coverdist F`): scales mesh distances, thinning and the impostor range |
| O, J | sun shadows on/off, TAA on/off |
| [ / ] | finer / coarser terrain (target triangle size in pixels) |
| V, F12 / K, P | vsync toggle, screenshot (`shot_NNNN.ppm`), print camera as `--cam` arguments |
| H | overlay (top right): FPS, FPS graph of the last 5 s, GPU ms, key list |

**macOS (Apple Silicon, e.g. MacBook Air M4):** same code and script, Vulkan via KosmicKrisp (LunarG's conformant Vulkan-on-Metal driver) or MoltenVK. Install the [LunarG Vulkan SDK for macOS](https://vulkan.lunarg.com/sdk/home#mac) with KosmicKrisp selected (it also provides `slangc` and the validation layer; check its release notes for the minimum macOS version), plus `brew install sdl3 pkg-config`. Then:

```sh
source ~/VulkanSDK/<version>/setup-env.sh            # sets VULKAN_SDK, PATH, DYLD_LIBRARY_PATH
./build.sh release
tools/fetch-bavaria.sh bbox 701 5503 716 5518        # needs curl only (aria2 for metalink: brew install aria2)
build/cook_terrain data/external/bavaria/dgm1        # ~3 GB RAM; on 16 GB machines fine, else --step 2
build/vr                                             # --hidpi for native Retina resolution (4x the pixels)
```

On the Mac the window is sized in points and rendered at 1 pixel per point unless `--hidpi` is given; the title bar shows the actual render resolution. If both KosmicKrisp and MoltenVK are installed, the conformant driver is preferred; `VK_DRIVER_FILES=<icd.json>` forces one. Missing Vulkan features are reported by name at startup. F12 needs fn on a MacBook keyboard; `K` also takes a screenshot. Not yet run on a Mac.

The window title shows CPU/GPU ms (total and per pass: shadows, terrain, trees, ground cover, sky + TAA), tree candidates and chunks, UTM position and height above ground. `build/vr --frames N [--shot f.ppm]` runs N frames and prints average timings (deterministic camera via `--cam`, `--stance N` to start walking at ground level); `--notrees`, `--nocover`, `--noshadows`, `--notaa`, `--treedist M` for A/B measurements, `--cover FILE` for another cooked ground-cover file. Ground-cover scans: Poly Haven, CC0 (Arma Reforger's assets are not licensed for use outside Reforger); on Windows run `tools/fetch-polyhaven.sh` from Git Bash, and in PowerShell pass the asset directories to `cook_ground_cover` explicitly (`(Get-ChildItem data\external\polyhaven -Directory).FullName`). `cook_terrain` accepts any single-band GeoTIFF tiles sharing one CRS and pixel size (uncompressed/LZW/Deflate, predictors 1–3, strips or tiles); missing tiles and nodata are filled by pull-push interpolation and reported; `--step 2` halves the resolution for smaller GPUs. `build.bat` (Windows: clang + Visual Studio, `VULKAN_SDK`, `SDL3_DIR`) mirrors `build.sh`.

## Throughput note

4K60 vs 1080p30 is 8× (4× pixels × 2× frame time). Only per-pixel work gains 8×. Per-frame work — culling, geometry, animation, BVH maintenance, shadow rasterization, i.e. most of the vegetation cost — gains only 2×. Shipping "4K60" titles render ~1080p–1440p internally and upscale, so the practical per-pixel advantage over them is ~2–4×. Details: [03](docs/03-hardware-mapping.md).

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
| [11 Architecture review](docs/11-architecture-review.md) | Adversarial review of 02–05 and its resolution: 16 findings applied, 1 withdrawn, 2 open decisions with defined closing criteria |
| [12 Terrain](docs/12-terrain.md) | Terrain techniques in the surveyed titles (Enfusion, Far Cry 4/5, Call of Duty, Tsushima, UE5, Unity) and the chosen design |
| [13 Reference maps](docs/13-reference-maps.md) | Usable large reference terrains: why Reforger/Arma maps are reference-only (licence), open Czech/Polish/Bavarian LiDAR + forest data, candidate regions |
| [References](docs/references.md) | Talks, papers, specs, SDKs |
