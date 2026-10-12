# Splat pass in `vr` — 3D Gaussian splatting, first implementation

## Context

`docs/09-gaussian-splatting.md` picked 3DGS as a research candidate for two vegetation sub-problems — conifer needle masses beyond ~10–20 m and leafless winter twig crowns beyond ~15–30 m — where elements are sub-pixel but the sprays/branches are still resolved. Its argument: for one spruce at 20 m, geometric needles cost 2–4 M triangles and pixel voxels 1.4–4.3 M, while material Gaussians cost 2–20 k, because "Gaussians scale with the number of element groups, not with pixel coverage". Nothing of this exists in code; `vis64` kind 5 is reserved for it in `docs/05-frame-pipeline.md` and the M7 experiment plan assumes a splat rasteriser exists.

Earlier research in this session established that **no tool for hand-modelling Gaussians exists** — every editor found (SuperSplat, the Unity package, Splatshop, GSOPs, BlendSplat) only edits splats that came from photo reconstruction. Authoring the minimal objects we want (a grass blade as a chain of 3–8 stretched Gaussians, a leaf as 1–3 flat ones) means writing Gaussians directly, and that is impossible without a renderer that shows them. A Blender extension for authoring is wanted later; building it before a truth renderer would be authoring blind.

So: add a splat pass to `vr` that loads, sorts, rasterises and debug-visualises 3D Gaussians inside the existing frame, sharing the camera, render origin, depth buffer and TAA.

## Decisions

| Question | Choice |
|---|---|
| Input | **PLY first.** Standard INRIA-format `.ply` cooks to a runtime format, so reference and captured splats work immediately and the same file opens in SuperSplat / SIBR for cross-checking. Hand-authored text format and a `.ply` writer come later. |
| Sorting | **GPU radix sort now** (4-pass, 8-bit, in Slang compute). |
| Colour | **Material Gaussians** — albedo, roughness, transmission, class — lit by the existing sun. No runtime spherical harmonics. |
| Blending | **Both paths.** Weighted-sum OIT is the runtime path per `docs/09`; sorted alpha blending exists as the correctness reference. |
| Debug views | **Ellipsoids** (analytic, SIBR-style) and **overdraw heatmap** (`DEBUG_OVERDRAW` already exists). |

### Resolving two conflicts

**1. `docs/09` says sorting is not needed.** It prescribes sort-free weighted-sum blending above ~2 px and stochastic coverage into `vis64` kind 5 below, and states "Sorted 3DGS blending is not needed. Foliage colours are similar, so order-independent blending errors stay small." That stays the shipping path. The sorted path is built anyway because it is the only way to render a captured `.ply` *correctly* and match SuperSplat pixel-for-pixel — without it, a projection bug is indistinguishable from an OIT approximation error. It also gives the A/B comparison infrastructure M7 asks for. Sorted = reference, OIT = runtime; both behind one toggle.

**2. "PLY first" and "material Gaussians" pull apart.** A captured `.ply` stores baked view-dependent radiance, not material parameters. Bridge in the **cooker**, matching `docs/09`'s "fitted offline" stance:

- `albedo = 0.5 + C0 * f_dc`, `C0 = 0.28209479177387814` (SH degree-0 basis); `f_rest_*` discarded.
- `opacity = sigmoid(stored)`, `scale = exp(stored)` — PLY stores both pre-activation.
- `roughness`, `transmission` = defaults; class = leaf.

A captured splat then renders **double-lit** (its baked lighting times our sun) and will not match SuperSplat. That is expected, not a bug. `DEBUG_UNLIT` (already defined, value 4) bypasses lighting and shows raw albedo — that is the mode used for apples-to-apples comparison.

## Step 0 — get the code on disk

The working tree is checked out on `main`, which holds only `LICENSE`. All 40 commits of renderer work are on `origin/claude/friendly-einstein-ncnbu9` (tip `90de785`), unmerged and not checked out.

```sh
git checkout -b splats origin/claude/friendly-einstein-ncnbu9   # or check out the branch itself
./build.sh release                                              # confirm the baseline builds
```

## How it fits the existing frame

`viewer_render` (`src/viewer.cpp:374`) records one scene pass into offscreen targets, then TAA resolve, then overlay:

- `src/viewer.cpp:560` `vkCmdBeginRendering` — 2 colour attachments (`vk->scene` RGBA16F, `vk->motion` RG16F) + depth D32, reversed Z, depth clear 0.
- gallery cells → terrain (`:607`) → trees (`:614`) → ground cover (`:619`) → **sky last** (`:626`), `vkCmdEndRendering` at `:629`.
- Per-pass timestamps at queries 2/3/4 (`:612`, `:617`, `:624`).

**Splats draw between the sky and `vkCmdEndRendering`.** They must follow the sky: they do not write depth, and the sky fills every pixel where depth is still 0, so drawing earlier would have the sky paint over them. Depth-testing `GREATER` against the stored depth keeps them correctly occluded by terrain and trees.

The sort runs **before** `vkCmdBeginRendering`, as compute dispatches alongside the shadow pass (`src/viewer.cpp:503`).

Everything else is already in place and must be reused, not rebuilt:

- `PipelineDesc` (`src/vk.cpp:675`) already supports `blend` (premultiplied over, `src/vk.cpp:728`), mesh/task pipelines, depth-test-without-write, and custom colour formats. **No changes needed for the raster pipelines.**
- `vk_buffer_static` (used at `src/vegetation.cpp:693`) uploads device-local scene buffers; `VegScene`/`GcScene` (`src/gpu_shared.h`) are the pattern for `SplatScene`.
- `ViewConstants` (`src/gpu_shared.h`) carries `view_proj`, `prev_view_proj`, 6 frustum planes and TAA `jitter` — use these, not the old hand-rolled `project()` in `shaders/terrain.slang`.
- `shaders/common.slang` holds the shared lighting/sky/fog helpers the material shading must call.
- Upload buffer layout: `UPLOAD_VIEWS_OFFSET 1024`, `UPLOAD_NODES_OFFSET 4096` (`src/vk.cpp:8-9`), 4 MiB per frame.

## Work

**New files**

| File | Contents |
|---|---|
| `src/splat_file.h` | Cooked `.vrs` header + `Splat` record, mirroring `src/terrain_file.h` / `src/ground_cover_file.h`. |
| `src/tools/cook_splats.cpp` | PLY → `.vrs`. Binary and ASCII, **properties indexed by name from the header** (producers vary), activations applied, SH DC → albedo, bounds computed. Unity TU like `cook_terrain.cpp`. |
| `src/splats.cpp` | Load, GPU buffers, pipeline creation, per-frame record (sort dispatches + draws). |
| `shaders/splat.slang` | Compute entries (`cs_splat_key`, `cs_hist`, `cs_scan`, `cs_scatter`) and raster entries (`vs_splat`/`fs_splat`, `fs_splat_ellipsoid`, `vs_oit_composite`/`fs_oit_composite`). |

**Modified**

- `src/gpu_shared.h` — `Splat` (32 B: `f32 x,y,z`; `3 x f16` scale; quaternion as `4 x snorm8`; albedo+opacity RGBA8; material byte quad = roughness / transmission / class / leaf bucket), `SplatScene { GPU_PTR(Splat) splats; GPU_PTR(u32) order; GPU_PTR(u32) keys; u32 count; }`, `PushConstants` += `GPU_PTR(SplatScene) splats` (7 → 8 pointers = 64 B, well under the 128 B limit), `DEBUG_SPLAT_ELLIPSOID 8`, `TEX_SPLAT_ACCUM` slot.
- `src/vk.cpp` — `vk_create_compute_pipeline` helper; the OIT accumulation target created and destroyed next to `vk->scene`/`vk->motion` (`src/vk.cpp:67`, bound at `:627`); bump `GPU_TIMESTAMPS` (queries 0–5 are taken, splats need 6 and 7).
- `src/vk_functions.cpp` — add `vkCreateComputePipelines`, `vkCmdDispatch`, `vkCmdFillBuffer` to the `VK_INSTANCE_FUNCTIONS` X-macro list (`:10`). Missing names are null pointers at runtime, not link errors.
- `src/vr.cpp` — `#include "splats.cpp"` after `ground_cover.cpp`, before `overlay.cpp`.
- `src/viewer.cpp` — `Splats` field on `Viewer`; sort dispatches before `vkCmdBeginRendering`; draw after the sky block (`:626`); timestamps; one overlay line (splat count, sort ms, raster ms) in `viewer_overlay` (`:262`).
- `src/platform_sdl.cpp` — `--splats <file.vrs>` in the arg loop (`:48`), keys in `map_key` (`:18`): `M` toggles the pass, `N` cycles sorted / OIT / ellipsoid.
- `build.sh` / `build.bat` — one `slangc` line per splat entry point (follow the `-fvk-use-entrypoint-name -entry … -stage …` pattern at `build.sh:35-46`), plus a `cook_splats` compile line next to `build.sh:57`.
- `docs/09-gaussian-splatting.md`, `docs/08-validation-roadmap.md` — record what was built, the double-lit caveat, and measured numbers.

**Order of work** — each step ends at something visible:

1. `cook_splats` + `.vrs` format. Verify with `--info`-style output: count, bounds, scale/opacity ranges after activation.
2. Load + **ellipsoid debug pipeline only** (analytic ray–ellipsoid intersection in the fragment shader of a screen-aligned quad, opaque, depth write). This validates positions, scales and the quaternion before any blending exists — a wrong `rot_0..3` order shows up instantly as mis-oriented ellipsoids.
3. Sorted splat raster: instanced quad per splat through the vertex pipeline (6 vertices from `SV_VertexID`, 3σ extent, EWA 2D covariance from `ViewConstants.view_proj`), premultiplied blend, order taken from a **CPU-sorted** index buffer. Gives correct pixels before the compute sort exists, and becomes the reference the GPU sort is checked against.
4. GPU radix sort: key pass (view depth → `u32` key) + 4 × (histogram, scan, scatter). Verify by comparing its output order against step 3's CPU sort on the same frame.
5. Weighted-sum OIT: additive accumulation into the new RGBA16F target, composite pass into the scene. Toggle against the sorted path.
6. Material shading: albedo / roughness / transmission lit by the existing sun and sky through `shaders/common.slang`; normal = the Gaussian's shortest axis. `DEBUG_UNLIT` bypasses it.
7. Overdraw heatmap via the existing `DEBUG_OVERDRAW` convention; overlay stats line.

Deliberately **not** in this step: mesh-shader raster path (the vertex path keeps lavapipe and the Mac working, where `vk->mesh_shaders` is false and vegetation is already disabled), `vis64` kind 5 stochastic coverage (no visibility buffer exists yet), splat shadows, wind, LOD, gallery integration, the hand-authored text format, and the fitting tool.

## Verification

```sh
./build.sh release
build/cook_splats <some>.ply -o data/cooked/test.vrs          # count, bounds, ranges
build/vr data/cooked/terrain.vrh --splats data/cooked/test.vrs
build/vr data/cooked/terrain.vrh --splats data/cooked/test.vrs \
    --cam <E> <N> <ALT> <YAW> <PITCH> --frames 120 --shot splat.ppm --validation
```

- **Correctness against a known-good viewer:** open the same `.ply` in SuperSplat, match the camera, compare against our sorted path in `DEBUG_UNLIT`. Shapes and silhouettes must match; absolute colour may differ by tonemapping.
- **Sort:** GPU order must equal the CPU order from step 3 on the same frame (compare the index buffers, not the pixels).
- **Orientation:** ellipsoid debug view — flat Gaussians must lie flat on surfaces, not stand on edge. This is the quaternion-order check.
- **OIT vs sorted:** toggle on the same frame; on foliage-like content the difference should be small, which is the claim `docs/09` rests on. If it is large, that claim needs revisiting in the doc.
- **Cost:** overlay sort/raster ms against the `docs/09` budget (2–5 ms for the conifer band on a 4060) and the overdraw heatmap for where it goes.
- **Validation layer clean** with `--validation`, and the vertex path must still run where mesh shaders are unavailable (lavapipe, macOS).
- **TAA:** move the camera and watch for smearing. Splats write motion into the second attachment with blending enabled, which is approximate; if it smears, that is the known risk and may need a reactive mask (`docs/05` mentions one for particles).
