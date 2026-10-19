# Procedural plants — first implementation: beech and spruce with generated textures

## Context

`docs/14-procedural-plants.md` chose one offline generator for all plants, with different structure algorithms per class (self-organising growth for broadleaf trees, whorl rules for conifers, L-systems for herbs, parametric grasses) and generated textures (bark generators, leaf venation, PROSPECT-D colour). Today the trees in `vr` come from `tree_build` in `src/vegetation.cpp`: 5 species × 2 variants of tubes and alpha cards built at load, with leaf shapes drawn by `leaf_alpha()` in `shaders/vegetation.slang`. Trees have no textures, and all trees of a type are identical apart from scale, yaw and tint (docs/06 §5.13, known weaknesses).

**Builds on `procedural-content-core.md`** (docs/15): the generators below are implemented as operators of the recipe system, executed by `vrgen` and previewed live in `vr`; parameters live in recipes, not in a dedicated tool.

This plan builds the generator for the two extremes, **European beech** (emergent broadleaf crown, smooth bark, distichous leaves) and **Norway spruce** (rule-bound conifer, scaly bark, geometric needles), and shows them in the gallery and the world next to the baseline. Herbs, grasses, the other species, the season LUT and the M1 asset format come later.

## Decisions

| Question | Choice |
|---|---|
| Tool | Plant operators in `src/gen/` (`GrowthParams`, `GrowBroadleaf`, `GrowConifer`, `LeafShape`, `NeedleSpray`, `LeafMaterial`, `BarkSmooth`, `BarkPlated`, `PlantAsset`), executed by `vrgen` and previewed in `vr --recipe` (docs/15). Deterministic per seed; variants evaluated in parallel. A mature tree takes seconds to grow, so results come from the content cache after the first run. |
| Output | **`data/cooked/plants.vpl`** (`src/plants_file.h`): per variant the existing runtime data — `TreeType`, meshlets for 4 LODs, `TreeVertex` arrays — plus texture sets. Loaded by `vr` with `--plants <file>`; it replaces the baseline beech and spruce types, the other three species stay baseline. |
| Parameters | Recipes `assets/recipes/beech.recipe`, `assets/recipes/spruce.recipe` (docs/15 §3 format). Allometry targets and sources as comments and as parameters of a `Targets` node that `PlantAsset` checks against. |
| Broadleaf structure | Self-organising growth (Palubicki 2009): shadow-propagation grid, Borchert–Honda allocation, tropisms, shedding, pipe-model radii. |
| Conifer structure | Annual-whorl loop (leader + whorl per year, laterals with their own whorls), shedding from the same shadow grid. |
| Habitats | Each species grown in three settings from the same parameters: **solitary** (one tree), **forest** (centre tree of a 5 × 5 plot at stand spacing), **edge** (plot cut in half). 2 seeds each → 6 variants per species. |
| Leaves and needles | Geometry, no alpha test. Beech leaf: outline polygon from the species' margin parameters, 8–12 triangles, folded along the midrib. Spruce: needles as thin 4-sided prisms only in LOD 0; sprays as geometry from LOD 1. Each leaf gets a 4-bit bucket (docs/04). |
| Textures | Generated in the tool, stored RGBA8 with mips (BC compression comes with the M1 cooker). Bark: 1024 × 1024 per species (albedo, normal + roughness + AO). Leaf: 256 × 256 per leaf variant (albedo, transmission + thickness + normal). |
| Leaf colour | PROSPECT-D for the summer state, integrated to linear sRGB reflectance and transmittance (D65, CIE 1931). Only the resulting RGB values are written into the recipes if the coefficient tables cannot be committed. |
| LODs | LOD 0: full geometry (all leaves). LOD 1: branches to order 3, leaves merged into twig clusters (geometry, half the leaves at double area). LOD 2: trunk + main branches + crown cluster cards whose textures are **rendered from LOD 0 by the tool** (CPU rasteriser). LOD 3: 3 crossed billboards rendered from LOD 0. Thresholds stay those of the baseline. |

### Resolving conflicts with the design docs

**1. docs/04 says vegetation enters as glTF with a wind rig and assemblies (`PartInstance`).** That runtime does not exist yet (M1). The generator keeps the full structure internally (skeleton with branch orders = future wind bones, part placements) but writes flattened meshes in the baseline format. A glTF exporter following the docs/04 convention is a later step of the same tool, not a rewrite.

**2. docs/06 §5.1 says no alpha cards.** LOD 0 and LOD 1 have none. LOD 2 and LOD 3 keep cards because the baseline runtime has no aggregate representation yet (impostors/voxels/Gaussians, docs/06 §5.4 and docs/09). Their textures come from our own geometry, not from hand-painted images.

**3. Hallé–Oldeman assignments are unverified** (docs/14 §1). Spruce (Massart's model) is safe. For beech the growth model needs no named architecture model to start: the distichous leaf arrangement and plagiotropic laterals are parameters. Record the verified model in `beech.recipe` once checked.

## Work

**New files**

| File | Contents |
|---|---|
| `src/gen/gen_plant_ops.cpp` | Operator table entries for the plant family (parameters, inputs, versions); calls into the modules below. |
| `src/gen/plant_growth.cpp` | Skeleton (nodes: position, direction, parent, order, age, radius, bud state), shadow grid, allocation, broadleaf growth, conifer whorl loop, shedding, pipe-model radii. |
| `src/gen/plant_mesh.cpp` | Tubes along branch chains with parallel-transport frames and junction collars, leaf and needle meshes, LOD construction, meshlet split (port of `meshletize` from `src/vegetation.cpp`). |
| `src/gen/plant_texture.cpp` | Bark generators on top of the core bitmap operators (ktg port), where a sub-graph is not enough (smooth + lenticels + branch scars for beech; scale plates for spruce), leaf generator (outline mask for cards only, venation via Runions space colonization in 2D, PROSPECT-D colour), CPU rasteriser for LOD 2/3 card textures. |
| `src/gen/prospect.cpp` | PROSPECT-D forward model (plate model, absorption from pigments) and spectral → linear RGB. |
| `src/plants_file.h` | `.vpl` header, variant table, offsets of vertex/meshlet/texture blocks (mirrors `terrain_file.h` / `ground_cover_file.h`). |
| `assets/recipes/beech.recipe`, `assets/recipes/spruce.recipe` | Parameters with units and sources in comments; bark as bitmap-operator sub-graphs (from the core plan's `test_bark.recipe`). |

**Modified**

- `src/gpu_shared.h` — `TreeType` gets `u32 bark_tex, leaf_tex, pad[2]` (96 → 112 B, update the `static_assert` in `src/vegetation.cpp`); `TEX_PLANTS` slot range after `TEX_FROZEN`; `MAT_LEAF_GEOM` (geometric leaf, textured, no alpha) next to the baseline `MAT_*`.
- `src/vegetation.cpp` — `veg_init`: if a `.vpl` is given, load it, append its meshlets and vertices to the arrays built by the baseline, overwrite the beech and spruce `TreeType` slots (variants beyond 2 per species need `TREE_VARIANTS` raised to 6 and the type field in `TreeInstance.packed` checked: it has 8 bits), upload the texture sets into the `TEX_PLANTS` slots.
- `shaders/vegetation.slang` — bark sampling with the cylindrical uv from the generator, `MAT_LEAF_GEOM` shading (albedo + transmission from the leaf set; two-sided normal), card materials sampling the baked LOD 2/3 textures instead of `leaf_alpha()` / `billboard_alpha()` for generated types.
- `src/platform_sdl.cpp` — `--plants <file.vpl>`.
- `src/gallery.cpp` — generated types appear as their own rows (label with species, habitat, seed).
- `src/gen/gen_ops.cpp` — include the plant family table.
- `docs/06-vegetation.md` §5.13 (tree assets row), `docs/14-procedural-plants.md`, `README.md` — record what was built and measured.

**Order of work** — each step ends at something visible:

1. **Skeleton only.** Growth for beech (solitary), output as line segments in a debug `.vpl` (wood tubes of constant thin radius). Gallery shows the branching. Tool prints height, crown radius, crown base, number of metamers per order.
2. **Shadow grid and habitats.** Forest and edge plots. Gallery LEVELS row: solitary / edge / forest side by side; forest-grown must show the high crown base and narrow crown, solitary the low wide crown.
3. **Wood mesh.** Pipe-model radii, tubes with collars, branch-order pruning for LOD 1/2. Check DBH and taper against allometry targets in the `Targets` node of `beech.recipe`.
4. **Spruce.** Whorl loop, droop by order and age, needles in LOD 0, geometric sprays in LOD 1. Same habitat comparison.
5. **Leaves.** Beech leaf outline and folding, distichous placement on the current-year shoots, buckets. Leaf-off rendering = leaves dropped by bucket (all buckets) to check the winter silhouette.
6. **Textures.** Bark (beech, spruce), leaf maps from venation, PROSPECT-D colours. Gallery TEXTURES view (`DEBUG_UNLIT`) and FINAL.
7. **LOD 2/3 bakes.** CPU rasterisation of LOD 0 into crown-card and billboard textures; LEVELS layout shows all four LODs; BLEND layout at matching distances.
8. **World.** `--plants` in the world view at the Grafenwöhr test camera; measure tree ms against the baseline with `--frames 200 --novsync`.

Deliberately **not** in this plan: other species, herbs and grasses, autumn/season LUT, wind rig export, glTF export, assemblies/`PartInstance` runtime, impostors/voxels/Gaussians, bark detail by age band, moss layer, calibration against TLS scans (needs the data licence checked) — each is a follow-up listed in docs/14.

## Verification

```sh
./build.sh release
build/vrgen -o data/cooked assets/recipes/beech.recipe assets/recipes/spruce.recipe     # PlantAsset nodes write plants.vpl
build/vr data/cooked/terrain.vrh --plants data/cooked/plants.vpl --gallery 1 --gallerylayout 1
build/vr data/cooked/terrain.vrh --plants data/cooked/plants.vpl --cam <E> <N> <ALT> <YAW> <PITCH> --frames 200 --novsync
```

- **Determinism:** two runs with the same files produce byte-identical `.vpl` (compare with `cmp`).
- **Shape numbers** printed per variant against the `Targets` nodes of the recipes: height, DBH, crown radius, crown base fraction, leaf count, leaf area, stand LAI (forest plot). Deviations are listed by the tool, not judged by eye.
- **Habitat difference:** solitary vs forest crowns must differ in crown base and width by the amounts in the `Targets` node; if they do not, the shadow grid or shedding is wrong.
- **Visual:** gallery LEVELS and BLEND layouts against reference photos of beech and spruce (summer and winter); compared with the baseline rows in the same gallery.
- **Textures:** bark texel density constant along the trunk (UV view, mode 7); no visible tiling seams on the cylinder; leaf colour in the albedo range of PROSPECT-D for the chosen pigments.
- **Cost:** triangles per LOD printed by the tool; tree ms in the world view no more than the baseline + the measured share of the extra LOD 0 geometry; if LOD 0 is too heavy in the world, raise its projected-size threshold (document the value) rather than thin the asset.
- **Validation layer clean** with `--validation`; the viewer still runs without `--plants`.
