# 09 — 3D Gaussian splatting for vegetation: applicability

State: October 2026. Checked against the requirements in [00](00-task.md) and the design in [06](06-vegetation.md).

## Two different things called "Gaussians"

| | Captured splats | Material Gaussians |
|---|---|---|
| Source | photographs (trained radiance field) | fitted offline to **our own** assets (differentiable rendering against the reference path tracer) |
| Per-Gaussian data | radiance as spherical harmonics (lighting, shadows, occlusion of the capture moment) | albedo, normal distribution, leaf-area density / coverage, transmission, leaf/wood class, leaf bucket |
| Dynamic lighting, seasons | no | yes |
| Valid view directions | only inside the capture's view cone | all |
| Animation | research (learned deformation) | rigid with a wind bone (positions + covariances rotate) |
| Advantage | photographic realism | compact, prefiltered primitive for sub-pixel aggregates |

## Verdict by subset

| Subset | Verdict |
|---|---|
| Conifer needle masses, beyond ~10–20 m | **Promising** — material Gaussians per needle spray, bound to the wind rig (§1) |
| Leafless (winter) twig crowns, beyond ~15–30 m | **Promising** — elongated Gaussians for the twig haze, branches stay geometry |
| Broadleaf leaf clusters, hedgerows, shrubs, beyond ~50–100 m | Candidate |
| Whole-tree far field, 0.5–5 km (4–40 px) | Candidate, competes with voxels/impostors |
| Ridge-line forest silhouettes beyond ~5 km | Niche candidate (forest-patch Gaussians vs terrain canopy layer) |
| Fixed-lighting content seen from restricted viewpoints (cutscenes, photo mode, backdrops behind barriers) | **Fits** with captured or baked splats |
| Overcast-only scenes | Captured splats roughly transferable (§3); dynamic weather breaks it |
| Aerial / top-down only views | Fits with drone captures under fixed lighting |
| Near-field broadleaf leaves (< ~50 m), hero trees close up | No — geometry |
| Grass, crops, flowers (any distance, any angle) | No — procedural blades + terrain shading |
| Switching representation by camera angle at runtime | No — pops during camera rotation |
| Ray tracing / GI proxies | No — triangle proxies |
| Offline capture: look-dev reference, asset source | **Yes** |

## 1. By element scale and distance: the aggregate band

Vegetation becomes an *aggregate* once its elements are thinner than a pixel. At 1080p and 60° vertical FOV one pixel
is 1.07 mm per metre of distance:

| Element | Thickness | Sub-pixel beyond |
|---|---|---|
| Spruce/fir needle | 1–1.5 mm | ~1–1.4 m |
| Scots pine needle | ~2 mm | ~1.9 m |
| Bare twig | 2–10 mm | ~2–9 m |
| Leaf (beech, birch, oak) | 4–7 cm | ~37–65 m |
| Branch | 5–20 cm | ~47–187 m |
| Needle spray (as a whole) | ~30 cm | ~280 m |
| Tree crown, 10 m | — | 4 px at ~2.3 km |

Between "elements sub-pixel" and "whole tree ≈ 4 px", triangles alias and are wasted. Primitive counts for one spruce
seen at 20 m (≈ 2000 needle sprays, ~500 needles each):

| Representation | Primitives |
|---|---|
| Geometric needles (Witcher 4 approach) | 2–4 M triangles |
| Pixel-sized voxels (UE Nanite Voxels approach) — 2.1 cm cells, crown ~141 m³, 10–30 % occupied | 1.4–4.3 M voxels |
| Material Gaussians, 1–10 per spray | 2–20 k Gaussians |

Voxels work only where a voxel ≈ a pixel *and* the tree is small (far field). Gaussians scale with the number of
element groups, not with pixel coverage — that is their real advantage, and it applies **before** the far field:
conifer foliage and bare twig crowns, where the needles/twigs are sub-pixel but the sprays/branches are still clearly
resolved. Branches and trunks stay geometry (rigid parts → no aggregate needed until ~50–190 m).

Costs of large splats: a Gaussian covering many pixels loses the inner detail (needle stripes) → synthesize it in
the resolve from procedural noise and the normal distribution; partial coverage is a stochastic test (see "Rendering
path" below).

## 2. By view angle

- **Captured splats** degrade outside the training view distribution (holes, noise); ground-level captures have no
  canopy tops, drone captures no undersides. Use them only where the camera cannot leave the capture cone:
  backdrops across a valley or lake, aerial-only views, fixed camera paths.
- **Material Gaussians** have no angle limits: training views are rendered from every direction.
- **No runtime switching by camera angle:** the representation would change while the camera rotates. Select by
  projected element size only.
- **Grazing views over meadows and fields:** the visible effect (sheen waves, layer darkening) is a shading problem
  of the grass layer, not a representation problem → no Gaussians.

## 3. By lighting and weather

- Captured radiance is valid only under the capture's lighting: fits cutscenes, photo mode, fixed-time benchmark
  variants. Under overcast skies (common in the biome) appearance ≈ albedo × sky visibility, so an overcast capture
  can be reused under other overcast conditions with an exposure/colour scale — but weather and time of day are
  dynamic in our design, so this stays a special case.
- Material Gaussians are a **turbid medium**: leaf-area density, leaf-angle distribution, leaf reflectance and
  transmittance — the standard canopy model of remote sensing (Ross 1981; SAIL). That gives physically grounded,
  dynamic shading including backlit transmission and crown self-shadowing.

## 4. By vegetation type (target biome)

| Type | Fit |
|---|---|
| Spruce, fir, pine (Carpathians, sandy pine forests) | best case: needles sub-pixel from ~1–2 m, sprays are compact anisotropic fans |
| Deciduous trees in winter (leaf-off half the year) | strong case: twig haze from ~10–30 m; branches as geometry |
| Deciduous trees in leaf, shrubs, hedgerows | from ~50–100 m, as leaf-cluster Gaussians |
| Grass, cereals, flowers, ferns | no |
| Landmark trees seen close up | geometry; capture → mesh conversion (LeafFit-style) |

## 5. By render pass

| Pass | Use |
|---|---|
| Primary visibility | yes, inside the bands above |
| Shadows (VSM) | stochastic depth from the same Gaussians → fractional shadows after filtering |
| RT / GI | no — triangle proxies (as for all vegetation) |
| Water reflections | same representation via SSR; RT hits proxies |

## Rendering path: stochastic splats, blending only as reference

**Decision:** splats are rendered as opaque fragments with stochastic coverage at every footprint size, and TAA turns
the per-frame binary coverage into partial coverage. Blending (weighted-sum OIT, sorted) is kept as an alternative and
as the correctness reference, not as the runtime path.

Per pixel inside a splat's 3σ quad:

```
α = o · exp(-½ dᵀ Σ'⁻¹ d)                    projected 2D Gaussian (EWA), as in sorted 3DGS
keep  ⇔  α > ξ,   ξ = blue-noise(pixel, frame) ⊕ hash(splat id)
kept  → opaque fragment: depth, payload (vis64 kind 5) or colour, exact motion vector
```

- **Unbiased:** E[coverage] = α, and with several overlapping splats the nearest kept fragment wins, which is a Monte
  Carlo estimate of front-to-back compositing (StochasticSplats, ICCV 2025: no sort, no popping, > 4× faster than
  sorted rasterization at comparable quality; samples per pixel trade cost against noise).
- **Same mechanism as the rest of the vegetation:** hashed alpha for cards (Wyman & McGuire 2017) and stochastic
  coverage for voxels and impostors ([06](06-vegetation.md)) already work this way. Splats become one more producer
  of opaque `vis64` fragments; resolve, deferred lighting, VSM shadows (same test in the shadow pass → fractional
  shadows after filtering), fog, SSR and depth-based effects need nothing splat-specific.
- **Motion vectors stay exact:** a kept fragment belongs to one Gaussian, so its motion comes from the wind rig
  (decision 11) without the approximation of blended motion.
- **Decorrelation:** ξ must differ between splats covering the same pixel (hash of the splat id), otherwise their
  coverage is correlated and the composite is wrong. Spatially stratified noise (blue noise, rotated per frame with
  the TAA jitter) keeps even a single frame looking like fine dither instead of white noise.

| Path | Role |
|---|---|
| Stochastic, 1 sample per pixel + TAA | runtime, all footprints inside the bands |
| Stochastic, 2–4 samples per pixel | where history is invalid (disocclusion, camera cuts); photo mode |
| Weighted-sum OIT (ICLR 2025), forward-shaded before TAA | fallback for large, uniformly low-α splats if their stochastic noise is unacceptable |
| Sorted alpha blending | correctness reference: matches SuperSplat/SIBR pixel for pixel; offline comparisons |

### Field evidence: Broken Arrow (2025, Unity 6)

Screenshots of the RTS (shared October 2026, not a published breakdown) show the technique and its failure mode at
once. Outside smoke the tree crowns look soft, without card or polygon edges: coverage is stochastic per pixel and TAA
averages it. Inside a moving smoke cloud the same crowns appear as an even, pixel-fine stipple whose density follows
crown opacity (solid cores, sparse rims): the smoke changes every frame, TAA rejects the history there, and the raw
single-frame coverage becomes visible. Reviews of the game report smearing and flicker on vegetation, consistent with
history rejection elsewhere.

### Failure modes and mitigations

| Cause | Effect | Mitigation |
|---|---|---|
| History rejected: disocclusion, camera cut, fast motion | raw dither visible for a few frames | blue-noise ξ (looks like ordered dither, not noise); 2–4 samples per pixel where history is invalid |
| Transparent particles (smoke, dust) over foliage | history of the opaque layer rejected under the particles (the Broken Arrow case) | TAA on the opaque layer only; particles composited after the resolve or excluded from history rejection; no "reactive mask" that resets the foliage history beneath |
| Animated foliage | ghosting or rejection | exact motion vectors from the stateless wind rig |
| Uniformly low α (thin far haze) | slow convergence, visible noise | fit fewer, more opaque Gaussians (minimum α constraint in fitting), or the OIT fallback for those splats |
| Correlated ξ between splats | wrong (too high or too low) combined coverage | per-splat hash in ξ |
| Overdraw | every candidate fragment pays the Gaussian test | 3σ quads clipped where α < 1/255; the test is a branch before the depth/payload write (SW raster) or a discard (HW) |

## Cost estimates (to be measured)

| Case | Load | RTX 4060 |
|---|---|---|
| Conifer slope, near–mid band | 1–6 M visible Gaussians | 2–5 ms (replaces sub-pixel needle triangles) |
| Whole-tree far field | 2–8 M splat evaluations | 1–2 ms (≈ voxels) |
| Memory | sprays/twig groups fitted once per species and instanced | ~10–100 KB per species; far field ~0.25 MB per variant |

## Captured splats: requirements check

- Standard 3DGS: 59 floats = 236 B per Gaussian (SPZ: 64 B). Original paper: ~3 M Gaussians, 734 MB, 134 FPS at 1080p
  on an RTX A6000 → ≈ 20 ms on an RTX 4060 for one scene with frozen lighting.
- Relighting (GaRe, DeferredGS, SSD-GS, …) is research; decomposing translucent foliage from one capture is unsolved.
- Wind on captured trees is research (Wind on Trees, Sep 2026: damping not recoverable from video).
- Memory: a captured plant has 10⁵–10⁶+ Gaussians → 6–64 MB; 30 species × 3 seasonal states → 0.6–6 GB.
- Ecosystem: `KHR_gaussian_splatting` ratified 2026; Houdini, Nuke 17, OpenUSD, V-Ray; Unreal via third-party
  plugins only (e.g. NanoGS with Nanite-style LOD); LOD for huge scenes: Virtualized 3D Gaussians (SIGGRAPH 2025).

## Offline uses (recommended now)

- **Look-dev ground truth:** capture beech, oak, hornbeam, birch, aspen, spruce, fir, Scots pine and meadow patches,
  ideally the same specimen per season; compare silhouette, crown density, colour variation, backlit translucency.
- **Asset source:** LeafFit-style conversion (segment leaves, fit a template leaf, extract meshes → instanced
  leaves = our assemblies); skeleton extraction → wind rig.
- Accept `KHR_gaussian_splatting` glTF in the cooker for these tools.

## Experiment plan (M7)

1. **Conifer foliage band** (10–200 m): geometric needles vs material Gaussians per spray (vs alpha-card baseline).
2. **Winter twig band**: geometric twigs vs elongated twig-haze Gaussians on geometric branches.
3. **Far field**: impostors vs voxels vs tree-level Gaussians.
4. **Splat rendering path**: stochastic (1 and 4 samples per pixel) vs weighted-sum OIT vs sorted reference, static and
   under camera motion, wind and smoke particles in front.

Metrics for all: FLIP vs path-traced reference, temporal stability with wind, ms, MB. Shared infrastructure: fitting
(Slang autodiff), splat rasterizer, resolve — the extra cost of testing Gaussians is small.

## Watch list

- Relightable outdoor splats that decompose foliage translucency correctly.
- Physically grounded wind for captured trees (Wind on Trees 2026, DynamicTree 2025).
- Mesh + Gaussian hybrids (Gaussian Frosting 2024, HaloGS 2025).
- Gaussian ray tracing (GRTX 2026), occlusion culling (NVGS, CVPR 2026), texture-codec compression (2026).
