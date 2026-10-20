# Plant editor

Browser tool for building plants from a node network (docs/14, docs/15): edit the network in a table, see the result immediately, export a textured mesh (`.glb`) for the project.

Open `index.html` directly from disk (Chrome, Edge, Firefox, Safari; WebGL2). No server, no build step, no dependencies. Work in progress is kept in the browser's local storage; use **Save species / Save seeds** for files.

## Species and seeds

| | Species (`.species`) | Instance (seeds) |
|---|---|---|
| What | the node graph + every parameter's **value** and **variation** | 3 numbers: `structure`, `foliage`, `texture` |
| Edited in | the inspector (left) | the seeds panel (right) |
| Stored | once per species (later possibly hard-coded) | per plant in a level, nothing else |

Each node draws its randomness from one seed channel: trunk, branches, tropism, prune and wood use `structure`; leaf shape, leaves and leaf mesh use `foliage`; the texture nodes use `texture`. The node's stream is `hash(channel seed, node name)`, so renaming a node changes its result.

A float or integer parameter's effective value is `clamp(value + variation × r, min, max)`, with r in [−1, 1) from the node's seed. The inspector's "now" column shows it. With all variations at 0 the seeds still change branch and leaf placement, but not the parameters.

Effects of the channels:
- `foliage` changes leaves and the leaf texture (it depends on the leaf shape).
- `texture` changes only the images.
- `structure` changes the branching; the leaves follow it.

The variant strip under the viewport shows the next 8 seed combinations; click one to use it. **lock** keeps a channel fixed there, and for "all ›" / "all ⟳". **+ keep in list** collects seed vectors; Save seeds writes them as a `.seeds` file (one `structure foliage texture` line per plant).

## Table editor

Nodes sit in table cells; wires connect typed ports. Port colours: skeleton brown, organ green, placements light green, mesh blue, texture purple.

| Keys | Action |
|---|---|
| arrows | move the cell cursor |
| Ctrl + arrows | move the node to the neighbouring empty cell |
| Enter | on a node: edit it (inspector); on an empty cell: insert a node (list: arrows or first letter, Enter) |
| Del | remove the node |
| C | connect: arrows to the target node, Enter (wrong types and cycles are refused) |
| X | disconnect the node's inputs |
| Space | preview this node: skeleton as lines, leaf shape, placements, mesh, texture maps; Space again returns to the output |
| Ctrl + D | duplicate the node |
| F | frame the view |

In edit mode:

| Keys | Action |
|---|---|
| Up / Down | select a parameter |
| Left / Right | change the value (Shift ×10) |
| Alt + Left / Right | change the variation |
| Enter | next enum value / open the colour picker |
| Esc | back to the table |

Mouse: click selects a cell, drag moves a node (snaps to the nearest empty cell), double-click = Enter. Viewport: left drag orbits, right drag pans, wheel zooms. **Help** lists all operators and parameters.

Nodes show their evaluation time, or `cached`. Only nodes downstream of a change rerun.

## Output

**Export GLB** writes glTF 2.0 binary:
- meshes `wood` (material `bark`) and `part:leaves` (material `leaves`, double-sided, one connected component per leaf);
- attributes `POSITION`, `NORMAL`, `TANGENT`, `TEXCOORD_0`, `COLOR_0` (per-leaf tint);
- embedded PNG base colour, normal and metallic-roughness maps;
- units metres, y up, origin at the stem base;
- `extras`: species name, species hash, seeds.

The file name carries the seeds: `<species>_<structure>_<foliage>_<texture>.glb`.

Check an export with the reader the cookers use:

```sh
clang -O1 tools/plant_editor/check_glb.c -o build/check_glb.exe
build/check_glb.exe beech_like_1_1_1.glb
```

## Files

- `species/beech_like.species`: the default species (also built into the page; **Default** restores it).
- `js/ops.js`: the operator table. Each node type is declared once, and the inspector, file format, variation and help come from it. The C port in `src/gen/` (docs/plans/procedural-content-core.md) follows the same table.
- `js/plant.js`: skeleton, branching, tropism, pruning, tube mesher, leaf organ, leaf placement.
- `js/texture.js`: bark and leaf texture generators.
- Others: `js/graph.js` evaluation and cache; `js/species.js` file formats; `js/editor.js` table and inspector; `js/view.js` WebGL2; `js/gltf.js` export.

## Limits (v1)

- **Branching:** level by level (Weber–Penn-like). Light-driven growth (docs/14 §2.1) is a planned node.
- **Leaf colour:** from a picked colour, not yet PROSPECT-D.
- **Not yet:** wind rig in the export, LODs, branch collars, pipe-model radii.
- **Leaves are enlarged clusters:** `Leaves.scale` > 1 by default, to keep the triangle count near 100–150 k; the real leaf count of a mature beech is ~10× higher.
