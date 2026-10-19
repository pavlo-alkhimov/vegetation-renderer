# Procedural content core — operators, recipes, cache, live preview

## Context

`docs/15-procedural-content.md` decided that content is described by recipes (graphs of generator operators with parameters and seeds), Werkkzeug-style, with real data only as optional import and statistics (option b + c). Plants come first (`docs/14-procedural-plants.md`, plan `procedural-plants.md`), but that plan was written before the recipe system and reads parameters from `key = value` species files into a dedicated `gen_plants` tool. This plan builds the shared core underneath: operator declaration, recipe format, evaluation, caching, a first operator family (bitmaps, ported from Farbrausch's public-domain `ktg`) and live preview in `vr`. The plant plan then adds its generators as operators on this core instead of building a separate tool.

Nothing of this exists yet. The viewer has a gallery (`src/gallery.cpp`) that shows ground-cover and tree assets in cells (`gallery_count`, `gallery_element`) and a text overlay (`src/overlay.cpp`); there is no hot reload of anything.

## Decisions

| Question | Choice |
|---|---|
| Code location | `src/gen/` — a library of unity-includable `.cpp` files, used by the new batch tool `vrgen` (`src/tools/vrgen.cpp`) **and** by `vr` (preview). One implementation, two front ends. |
| Operator declaration | X-macro parameter tables + one `OPERATOR(...)` line per operator (docs/15 §3). The macros generate: parameter struct, defaults, range clamp, parser entries, hash input, panel rows, `vrgen --ops` reference. |
| Types in this plan | `Bitmap` (RGBA16 like ktg's `Pixel`, power-of-two sizes) and `Field` (f32 grid). `Mesh`, `Skeleton`, `Organ`, `Plant` are declared but implemented by the plant plan. |
| Recipe format | One node per line: `name = Operator key=value ...`; `#` comments; `include "file"`; values: int, float, `#rrggbb[aa]`, enum label, node reference, `a..b step s` sweeps. Order-free; unknown keys and out-of-range values are errors with file:line. |
| Seeds | `SEED` parameters; a node without an explicit seed gets `hash(recipe seed, node name)`. Renaming a node changes its seed (documented), adding nodes does not. |
| Cache | Key = 64-bit hash of (operator id, operator version, clamped parameter bytes, input keys). Memory cache in `vr`; disk cache `data/cache/<key>.bin` (git-ignored) for both front ends. Operator version is a constant in its `OPERATOR` line, bumped when the code changes. |
| Threads | Nodes of one topological level run in parallel (`std::thread` pool as in `cook_terrain`); each operator is single-threaded or internally deterministic. |
| Bitmap operators | Port of `ktg/gentexture.cpp` (public domain, F. Giesen): `Noise`, `Cells`, `Gradient`, `Flat`, `GlowRect`, `ColorMatrix`, `CoordMatrix`, `ColorRemap`, `CoordRemap`, `Derive` (normals), `Blur`, `Ternary`, `Paste`, `Bump`, `LinearCombine`, plus `Range`, `Normalize`. Rewritten to the house style (C-style, our types), behaviour kept. |
| Output of this plan | Bitmaps written as `.ppm` (8-bit) and `.vrb` (raw RGBA16 + header) by `vrgen`; in `vr` uploaded as textures for preview. The cooked asset formats come with the plant plan. |
| Preview in `vr` | `--recipe <file>`: the gallery gets a **recipe layout**: one cell per node (or per selected node), bitmap shown as a textured quad with its name, size, time and cache state. The file's modification time is polled once per second; on change the recipe is re-evaluated on a worker thread, the old results stay visible until the new ones are ready. |
| Parameter panel | Overlay box listing the selected node's parameters; keys change the value by its declared step; the change is written back into the recipe line (only that line is rewritten, comments kept). |

### Conflicts with existing plans and docs

**1. `procedural-plants.md` defines `gen_plants` and `assets/species/*.species`.** Superseded by recipes: species parameters become `GrowthParams` nodes in `assets/recipes/<species>.recipe`, and `gen_plants` becomes the set of plant operators executed by `vrgen`. The plant plan's step order, files for growth/mesh/texture code and verification stay valid; its "Tool" and "Parameters" decisions are updated to point here.

**2. docs/04 names glTF as the source format for assets.** For generated assets the recipe is the source; glTF remains the interchange format for anything modelled by hand and as an export of generated assets for inspection (later).

**3. `ktg` uses 16-bit pixels and power-of-two sizes; our GPU textures are 8-bit sRGB or BC.** Generation stays 16-bit (headroom for chained operators); conversion to the GPU format happens only at output (`ToTexture` step of the plant plan) and in the preview upload.

## Work

**New files**

| File | Contents |
|---|---|
| `src/gen/gen.h` | Types (`GenBitmap`, `GenField`, value union), `OPERATOR` / parameter X-macro machinery, node and graph structs, public API: `gen_parse`, `gen_eval`, `gen_result`, `gen_free`. |
| `src/gen/gen_core.cpp` | Recipe parser (includes, sweeps, errors with file:line), name resolution, cycle detection, topological levels, seed derivation, hashing (64-bit, e.g. xxHash-style, own implementation), memory and disk cache, thread pool, line rewrite for the parameter panel. |
| `src/gen/gen_bitmap.cpp` | ktg port + `Range`, `Normalize`; operator table entries. |
| `src/gen/gen_ops.cpp` | The single list of all operators (includes each family's table), so `--ops` and the parser see the same set. |
| `src/tools/vrgen.cpp` | Batch front end: `vrgen [-o dir] [--ops] [--threads N] [--nocache] <recipe>...`; prints per-node time and cache hits; writes outputs named by node. |
| `assets/recipes/test_bark.recipe` | First artistic test: a beech-like smooth bark made only from bitmap operators (noise, cells for lenticels, gradient remap, normals). |
| `assets/recipes/ktg_demo.recipe` | Port of `ktg/demo.cpp`'s texture as a recipe — the regression reference (see Verification). |

**Modified**

- `src/vr.cpp` — include `gen/gen_core.cpp`, `gen/gen_ops.cpp` before `gallery.cpp`.
- `src/gallery.cpp` — recipe layout (`GL_RECIPE`): cells from the recipe's nodes, bitmap cells drawn with a flat textured-quad pipeline (no orbit); labels via the existing overlay boxes.
- `src/viewer.cpp` — recipe state on `Viewer`, mtime poll, worker-thread re-evaluation, texture upload into a `TEX_GEN` slot range (new, after `TEX_FROZEN`), parameter panel input.
- `src/platform_sdl.cpp` — `--recipe <file>`; keys in recipe layout: arrows select node / parameter, `+`/`-` step, `Shift` ×10.
- `src/gpu_shared.h` — `TEX_GEN` slot range.
- `build.sh` / `build.bat` — `vrgen` compile line (always `-O2`).
- `.gitignore` — `/data/cache/`.
- `docs/plans/procedural-plants.md` — Tool/Parameters decisions point to this plan (done together with this file).
- `README.md`, `docs/15-procedural-content.md` — record what was built.

**Order of work** — each step ends at something visible:

1. **Parser + graph + one operator.** `Flat` and `Noise` only; `vrgen` evaluates `test_flat.recipe`, writes PPMs, prints the graph in topological order. Errors for unknown operator, unknown key, out-of-range value, cycle, missing reference — each with file:line.
2. **Operator macros complete.** `vrgen --ops` prints the generated reference (operator, inputs, parameters with type, range, default, description). This is the check that the declaration machinery covers everything the parser and panel need.
3. **ktg port.** All bitmap operators; `ktg_demo.recipe` reproduces the demo texture.
4. **Cache.** Hash keys, memory + disk cache; second run of the same recipe executes nothing; changing one parameter re-executes only its dependents (printed).
5. **Preview in `vr`.** `--recipe`, recipe layout in the gallery, hot reload on save, worker-thread evaluation.
6. **Parameter panel.** Select node and parameter, step values, write-back into the recipe text; the next hot reload picks it up, so file and screen never disagree.
7. **Artistic test.** `test_bark.recipe` tuned through the panel to a believable smooth beech bark; this is the first evidence that the control model works, and the input for `BarkSmooth` as a sub-recipe in the plant plan.

Then continue with `procedural-plants.md` (growth, organs, materials, `PlantAsset` as operators). Sub-recipes (a recipe used as an operator), mesh, terrain, placement and statistics operators are later plans.

## Verification

```sh
./build.sh release
build/vrgen --ops                                            # operator reference from the tables
build/vrgen -o /tmp/gen assets/recipes/ktg_demo.recipe       # outputs + per-node timing
build/vrgen -o /tmp/gen assets/recipes/ktg_demo.recipe       # second run: all cache hits
build/vr data/cooked/terrain.vrh --recipe assets/recipes/test_bark.recipe --gallery 1
```

- **ktg equivalence:** the ported demo texture equals the original `ktg/demo.cpp` output (build the original from `farbrausch/fr_public`, compare 16-bit pixels; any difference must be explained by a documented change, e.g. clamping).
- **Determinism:** two runs (and runs with 1 vs N threads) give byte-identical outputs and identical cache keys.
- **Cache correctness:** editing one parameter re-executes exactly the nodes downstream of it (vrgen prints the executed set); bumping an operator's version invalidates only nodes using it.
- **Errors:** a set of broken recipes (`assets/recipes/tests/*.recipe`) each fails with the expected message and line number.
- **Hot reload:** saving the recipe in an editor updates the gallery within ~1 s for bitmap recipes; the viewer never blocks on evaluation (frame time stays flat during re-evaluation).
- **Write-back:** after panel edits, the recipe file differs only in the edited values; comments and alignment of other lines unchanged (`git diff`).
- **Validation layer clean** with `--validation`; `vr` without `--recipe` unchanged.
