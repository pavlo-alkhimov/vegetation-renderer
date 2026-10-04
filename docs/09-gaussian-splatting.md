# 09 — 3D Gaussian splatting for vegetation: applicability

State: October 2026. Checked against the requirements in [00](00-task.md) and the design in [06](06-vegetation.md).

## Verdict

| Use | Verdict | Reason in one line |
|---|---|---|
| Runtime near/mid-field trees, shrubs | **No** | baked lighting, no seasons, research-grade animation, sorting/overdraw, memory |
| Runtime grass and ground cover | **No** | procedural blades cost zero memory and are fully dynamic |
| Runtime far field (4–40 px) | **Experiment (M7)** | "material Gaussians" fitted from our own assets compete with voxels; voxels stay default |
| RT / GI proxies | **No** | Gaussian ray tracing needs proxy geometry + per-hit evaluation; triangle proxies are cheaper |
| Offline capture of real plants | **Yes** | best available capture of thin, fuzzy foliage → look-dev ground truth and asset source |
| Fixed-lighting captured backdrops | Only if dynamic time of day is dropped | lighting is baked into the colours |

## What 3DGS is

- Scene = millions of anisotropic 3D Gaussians: position, rotation, scale, opacity, view-dependent colour (spherical
  harmonics degree 3) = 59 floats = **236 B** each. SPZ quantizes to **64 B** (~10× smaller files).
- Rendering: project to screen-space ellipses → tile binning → **global depth sort** → front-to-back alpha blending.
  Trained from photographs by differentiable rendering; the colour is the **captured radiance**, i.e. lighting,
  shadows and occlusion of the capture moment.
- Reference cost: the original paper reports ~3 M Gaussians (734 MB) at 134 FPS, 1080p on an RTX A6000 (Mip-NeRF 360
  average). Scaled by compute and bandwidth (~2.5–2.8×) that is **≈ 20 ms on an RTX 4060** — for one captured scene
  with frozen lighting, before anything else in the frame.

## Why it is attractive for vegetation

- Thin, fuzzy, semi-transparent aggregates are exactly what photogrammetry meshes fail on; a soft volumetric
  primitive prefilters them, so distant foliage stays stable (Mip-Splatting).
- Scale is solved in principle: hierarchical 3DGS (2024), Virtualized 3D Gaussians (SIGGRAPH 2025: composed scenes of
  ~0.1 B Gaussians in real time, up to 6.19× faster at far distances), NanoGS for UE5 (2026, Nanite-style clusters +
  GPU radix sort).
- Ecosystem: `KHR_gaussian_splatting` (release candidate Feb 2026, now ratified); Houdini, Nuke 17, OpenUSD, V-Ray;
  Unreal only through third-party plugins (no first-party module); 4D splats in film final pixels.
- Plant-specific research is active: LeafFit (Eurographics 2026), GaussianPlant (2025), skeleton extraction from
  splats, physically parameterized wind (Wind on Trees, Sep 2026).

## Requirements check

| Requirement ([00](00-task.md)) | 3DGS state, Oct 2026 | Fit |
|---|---|---|
| Dynamic time of day | Standard 3DGS bakes radiance. Relightable variants (GaRe, DeferredGS, SSD-GS, …) are research; decomposing **translucent, multiply-scattering foliage** from one capture lighting is unsolved | ✗ |
| Seasons (colour, leaf thinning, leaf-off) | One capture = one season; leaf-off branches are hidden in a leaf-on capture; per-leaf control needs segmentation (LeafFit) | ✗ |
| Wind animation, exact motion vectors | Bind Gaussians to rigid parts/bones (RigGS, TreeSplat, 4DGS); Wind on Trees (2026) shows damping is not recovered from video and frequency only for sparse trees | ◐ |
| Shadows | Capture shadows are baked in (double shadowing when relit); casting needs stochastic depth or opacity-threshold depth | ◐ |
| RT GI | 3DGRT/3DGUT, GRTX: BVH of bounding proxies + per-hit Gaussian evaluation; Vulkan sample exists; far above our ray budget | ✗ |
| Visibility buffer / deferred | Order-dependent transparency, no single surface per pixel. Sort-free stochastic splatting (StochasticSplats, ICCV 2025: > 4× faster than sorted) maps onto one opaque sample per pixel + TAA | ◐ |
| Memory (8 GB card, 1 GB geometry pool) | Captured plant 10⁵–10⁶+ Gaussians → 6–64 MB at 64 B; 30 species × 3 seasonal states → **0.6–6 GB**. LeafFit names memory as the main barrier to adoption | ✗ |
| LOD to 5–10 km | Cluster hierarchies exist (V3DG, NanoGS) | ✔ |
| Close-up quality | Soft leaf edges, blobs and floaters at close range | ✗ |
| Variation, procedural placement | Instancing works; per-instance colour/season needs an attribute model, not baked radiance | ◐ |
| Gameplay / collision | No surfaces → proxies needed (we have them anyway) | — |

The decisive point: **3DGS's main value is photographic capture, and captured radiance is incompatible with dynamic
lighting and seasons.** Fitting Gaussians to our own assets instead keeps them relightable but leaves only "a
primitive for aggregate appearance" — which is the far-field problem voxels already address.

## Far-field experiment: material Gaussians vs voxels

| | Octahedral impostor (phase A) | Voxels (phase B) | Material Gaussians (experiment) |
|---|---|---|---|
| Primitive | quad + view atlas per tree | 4×4×4 bricks on a grid | free anisotropic ellipsoids |
| Memory per species variant | 1–4 MB atlas | ~0.2–0.5 MB sparse | ~0.25 MB (~10k × 24 B, estimate) |
| Shape per primitive | view-interpolated image | isotropic, blocky at coarse levels | anisotropic: conifer spires, branch directions, bare twig crowns |
| LOD | atlas mips | mip pyramid, trivial | cluster hierarchy, offline merging |
| Raster | HW quads + hashed coverage | compute splat, stochastic | compute splat, stochastic, footprint evaluated per pixel |
| Lighting, seasons | wood/leaf layers, dynamic shading | per-voxel attributes, dynamic | per-Gaussian attributes, dynamic |
| Risk | low | medium | medium-high |

Sketch (only the primitive differs from the voxel path):
1. **Offline:** per species variant and leaf state, fit Gaussians carrying *material* attributes — albedo, normal
   distribution, coverage, transmission, leaf/wood class, leaf bucket — to multi-view renders of our own asset from
   the reference path tracer (Slang autodiff). Build a cluster hierarchy (V3DG-style).
2. **Runtime:** far-tree list → cluster LOD → stochastic splat: per covered pixel α = opacity · G(x),
   `hash(gaussian, pixel, frame) < α` → `atomicMax(depth | payload)` into `vis64` kind 5 (visible cluster | Gaussian).
   Resolve reads the attributes; standard deferred lighting, GI, fog. VSM pages get the same stochastic splat.
3. **Animation:** whole-tree sway — rotate positions and covariances about the root.
4. **Estimated cost:** 2–8 M splat evaluations per frame ≈ 1–2 ms on the RTX 4060, similar to voxels.

Measured in M7 against voxels on the same scenes: FLIP vs path-traced reference, temporal stability, memory, ms.
The fitting pipeline, splat rasterizer and resolve are shared, so the comparison is cheap. Expected: similar cost
and memory; Gaussians may win on coarse levels (fewer primitives for spires and twig crowns), voxels win on
simplicity. UE 5.7 chose voxels for the same problem.

## Offline uses (recommended now)

- **Look-dev ground truth:** capture reference specimens — beech, oak, hornbeam, birch, aspen, spruce, fir, Scots
  pine, meadow patches — with phone or drone, ideally the same specimen per season. Compare silhouette, crown
  density, colour variation and backlit translucency against our renders (perceptual only: capture lighting differs).
- **Asset source:** LeafFit-style conversion — segment leaves, fit a template leaf to all instances, extract meshes →
  instanced leaves = our assemblies; skeleton extraction from splats → wind rig.
- Interchange: accept `KHR_gaussian_splatting` glTF in the cooker as an input format for these tools.

## Watch list

- Relightable outdoor splats where foliage translucency is decomposed correctly (none convincing as of Oct 2026).
- Physically grounded wind for captured trees (Wind on Trees 2026, DynamicTree 2025).
- Gaussian ray tracing performance (GRTX 2026) and occlusion culling (NVGS, CVPR 2026).
- GPU-friendly compression using texture codecs (2026).
