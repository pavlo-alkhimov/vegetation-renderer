# 07 — Techniques survey (non-vegetation), October 2026

Legend: **✔** chosen · **◐** optional tier / later · **✗** not used. Vegetation-specific techniques: [06](06-vegetation.md).

## Geometry and visibility

| Technique | Production use | Note | Us |
|---|---|---|---|
| GPU-driven culling + multi-draw-indirect | AC Unity (Haar & Aaltonen 2015), most AAA engines | still the baseline for non-cluster engines | ✔ (as cluster pipeline) |
| Two-phase HiZ occlusion culling | Nanite, Alan Wake 2, AC Shadows | conservative, no reprojection heuristics | ✔ |
| Mesh shaders | Alan Wake 2 (required), UE5, Northlight | Turing+/RDNA2+/Arc | ✔ |
| Visibility buffer / deferred texturing | Burns & Hunt 2013; Nanite 2021; Horizon Forbidden West 2022 | decouples geometry density from shading cost | ✔ |
| Virtualized geometry (cluster LOD DAG + streaming) | Nanite (UE5); meshoptimizer `clusterlod.h` (v1.0, Dec 2025); nvpro `vk_lod_clusters` | open-source tooling now mature | ✔ |
| Software rasterization of micro-triangles | Nanite | ~3× over HW for tiny triangles (Nanite) | ✔ |
| GPU tessellation / displacement | Nanite tessellation (UE 5.4+); SIGGRAPH 2026 adaptive tessellation talk | terrain/rock detail | ◐ |
| 3D Gaussian splatting (captured radiance) | film/VFX, viewers, UE third-party plugins; `KHR_gaussian_splatting` ratified 2026 | baked lighting, sorting, memory; see [09](09-gaussian-splatting.md) | ✗ runtime, ◐ far-field experiment, ✔ offline capture |
| D3D12 work graphs / mesh nodes | demos (AMD 2024), HPG 2025 trees | Microsoft drops them from SM 6.10, replaced by "Work Lists" (expanded ExecuteIndirect); Vulkan only AMDX | ✗ |
| Device-generated commands | D3D12 ExecuteIndirect, `VK_EXT_device_generated_commands` | unnecessary with mesh shaders + indirect dispatch | ✗ |

## Shadows

| Technique | Production use | Note | Us |
|---|---|---|---|
| Cascaded shadow maps (+ SDSM, caching) | most engines | simple, robust; wastes resolution, poor for foliage detail | ◐ (M1 stepping stone) |
| Virtual shadow maps (clipmap, page caching) | UE5 | high resolution where needed; caching conflicts with animated foliage | ✔ |
| Ray-traced shadows | Shadow of the Tomb Raider, Cyberpunk 2077, most path-traced titles | exact, soft; needs animated BVH + denoising | ◐ (quality tier) |
| Screen-space contact shadows | Days Gone / Bend Studio (open-sourced 2023), UE | fills small-scale detail | ✔ |
| Stochastic many-light shadows | UE MegaLights (5.5 → production in 5.8) | many shadowed area lights | ◐ |

## Global illumination

| Technique | Production use | Note | Us |
|---|---|---|---|
| Baked lightmaps/probes | Ghost of Yōtei ambient probes + RT (GDC 2026) | no dynamic time of day / seasons | ✗ |
| DDGI probe grids | RTXGI 1.x, many titles | cheap, leaky in dense foliage | ◐ (fallback tier) |
| SDF + surface cache (Lumen SW) | UE5; Lumen Lite / medium quality in UE 5.8 | thin foliage is a weak case for SDFs | ✗ |
| Voxel GI | KCD2 (SVOGI) | works without RT hardware | ◐ (fallback candidate) |
| HW ray-traced GI with caches | Indiana Jones, Doom TDA (idTech 8, SIGGRAPH 2025), AC Shadows, Avatar, Lumen HWRT | 2025–2026 industry default | ✔ |
| ReSTIR GI / DI / PT | Cyberpunk 2077 RT Overdrive, Alan Wake 2, RTXDI | spatiotemporal reuse; needs good denoising | ✔ (GI) |
| Spatial hash radiance cache (SHaRC), neural radiance cache (NRC) | RTXGI 2.x, path-traced titles | world-space caching of multi-bounce light | ✔ hash grid; ◐ NRC |
| ORCA — online radiance cache acceleration | EA SEED, SIGGRAPH 2026 | per-frame cache, no temporal history | ◐ (evaluate) |
| Variable-rate ray tracing | Call of Duty: Modern Warfare 4, SIGGRAPH 2026 | spend rays where they matter | ◐ |
| Radiance cascades | Path of Exile 2 | strong in 2D/2.5D; 3D memory-heavy | ✗ |
| Screen-space indirect with visibility bitmask (SSILVB) | Blender EEVEE Next, others | near-field occlusion/indirect | ✔ |
| Full real-time path tracing | Cyberpunk 2077, Alan Wake 2, Indiana Jones, Witcher 4 demo | needs DLSS-RR class reconstruction; borderline on RTX 4060 at 1080p30 | ◐ (reference mode, later real-time option) |

## Reflections

| Technique | Note | Us |
|---|---|---|
| SSR (hierarchical) | cheap first hit, fails off-screen | ✔ (first step) |
| RT reflections with reuse + denoise | standard in RT titles | ✔ glossy/mirror |
| Reflection probes | static, no seasons/time of day | ✗ |

## Materials and textures

| Technique | Note | Us |
|---|---|---|
| GGX + multiple-scattering energy compensation (Kulla & Conty 2017) | baseline PBR | ✔ |
| OpenPBR (2024) / UE Substrate | layered, physically based standards | ✔ as reference for parameter meaning, reduced model set |
| Thin-surface diffuse transmission (foliage) | UE two-sided foliage, OpenPBR thin-walled | ✔ |
| Geometric specular anti-aliasing (Kaplanyan 2016, Tokuyoshi 2019) | essential for foliage | ✔ |
| BCn + mip streaming | universal | ✔ |
| Runtime virtual texturing (terrain) | Far Cry, UE RVT | ✔ terrain |
| Sampler feedback | D3D12 only | ✗ (own feedback buffer) |
| Neural texture compression (RTXNTC) | no shipping game as of Apr 2026; Vulkan: inference on load / on sample | ◐ research |
| Neural materials | NVIDIA RTX Kit | ◐ watch |

## Atmosphere and volumetrics

| Technique | Production use | Us |
|---|---|---|
| LUT-based physical atmosphere (Hillaire 2020) | UE, many | ✔ |
| Ray-marched volumetric clouds (Nubis, Horizon series) | Horizon, many | ✔ |
| Froxel volumetric fog (Wronski 2014, Hillaire 2015) | AC, Frostbite, UE | ✔ |
| Volumetric effects ("Smolder", IOI, SIGGRAPH 2026) | Glacier / 007 First Light | ◐ reference for smoke/fire |

## Temporal reconstruction, upscaling, frame generation

| Technique | Status Oct 2026 | Us |
|---|---|---|
| TAA (Karis 2014 lineage) | universal, vendor-neutral | ✔ own |
| DLSS 4.5 Super Resolution / DLAA | 2nd-gen transformer (CES 2026), FP8 → best on Ada/Blackwell | ◐ NVIDIA quality option (DLAA) |
| DLSS Ray Reconstruction | 4.5 RR released Aug 2026 for all RTX | ◐ replaces our denoisers on NVIDIA |
| DLSS (multi) frame generation | MFG D3D12-only; FG meaningless at 30 FPS base | ✗ |
| DLSS 5 "3D-guided neural rendering" | shipped Sep 2026, RTX 50 only, 1 frame in → 1 frame out, generative enhancement | ✗ (not on RTX 40; changes the art, not reconstruction) |
| FSR 3.1 | open source, Vulkan | ◐ comparison |
| FSR 4 (ML) | FidelityFX SDK 2.0, officially D3D12-only, RDNA4 | ✗ for now |
| XeSS 2 | Vulkan available | ◐ comparison |
| PSSR (PS5 Pro, upgraded per SIGGRAPH 2026) | console only | ✗ |

## Post and display

| Technique | Us |
|---|---|
| Histogram auto-exposure, eye adaptation | ✔ |
| Energy-conserving bloom (dual filter, Jimenez 2014) | ✔ |
| Tone mapping: ACES 2.0 / AgX / Khronos PBR Neutral | ✔ one chosen by evaluation |
| HDR10 (PQ) and scRGB output | ✔ |
| Motion blur (tile-max velocity), depth of field | ◐ |
| Film grain, 3D LUT grading | ✔ |

## Streaming and I/O

| Technique | Status | Us |
|---|---|---|
| DirectStorage 1.4 (Zstd, GPU decompression, GACL) | Windows, GDC 2026; Zstd GPU shader open-sourced | ✗ API, ✔ ideas (Zstd, GPU decompression later) |
| io_uring / IoRing | Linux / Windows 11 | ✔ |
| Page-based geometry streaming | Nanite, `vk_lod_clusters` | ✔ |

## Engine architecture

| Technique | Note | Us |
|---|---|---|
| Frame graph / render graph (Frostbite 2017) | automatic barriers and aliasing; valuable for many teams and many passes | ✗ — fixed hand-ordered pass list + access tracker + static aliasing plan |
| Fiber job system (Naughty Dog 2015) | CPU-bound engines | ✗ — simple worker pool; our CPU load is small |
| Game/render/RHI thread split (UE) | hides CPU cost, adds latency | ✗ — GPU-driven, single main thread |
| Bindless resources | universal in modern engines | ✔ |
| Descriptor heaps (`VK_EXT_descriptor_heap`, Jan 2026) | new; driver maturity still growing | ◐ later switch |
