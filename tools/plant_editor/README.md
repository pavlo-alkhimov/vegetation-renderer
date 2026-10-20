# Plant editor

Browser tool for building plants from a node network (docs/14, docs/15): edit the network in a table, see the result immediately, export a textured mesh (`.glb`) for the project.

Open `index.html` directly from disk (Chrome, Edge, Firefox, Safari; WebGL2). No server, no build step, no dependencies. Work in progress is kept in the browser's local storage; use **Save species / Save seeds** for files.

## Species and seeds

| | Species (`.species`) | Instance (seeds) |
|---|---|---|
| What | the node graph + every parameter's **value** and **variation** | 3 numbers: `structure`, `foliage`, `texture` |
| Edited in | the inspector (left) | the seeds panel (right) |
| Stored | once per species (later possibly hard-coded) | per plant in a level, nothing else |

Every node that uses random numbers takes its seed from an **explicit input**: its `seed` must be bound to a value node, otherwise the node reports "seed not connected". The library species bind each node to the seed output of its channel on the Inputs node. Channels:
- `structure`: trunk, branches, tropism, prune, wood;
- `foliage`: leaf shape, leaves, leaf mesh;
- `texture`: the texture nodes.

A newly inserted node is bound to its channel's seed output automatically; the wire is visible and can be changed. The node's stream is `hash(seed input, node name)`, so renaming a node changes its result.

### Input column and value nodes

Column 0 of every library species holds one **Inputs** node with all network inputs as separate outputs: `lod`, the seeds `structure`, `foliage`, `texture`, and the constants `c1`, `c2`, `c3` (trunk vertical segments, trunk radial segments, spare). Selecting it also shows the instance values (seeds, LOD level, kept seed list). C on it first asks which output to connect.

Column 1 holds derived values: `trunk_segments = max(2, round(x*(1-0.17*y)))` and `trunk_sides = max(3, round(x*pow(0.65,y)))`, with x = the constant and y = lod.

| Value node | Output |
|---|---|
| `Inputs` | `lod`, the three seeds and constants `c1`..`c3`, each a separate output (`.species`: `@inputs.c1`) |
| `Const` | a number (per-LOD values possible) |
| `Expr` | an expression of its inputs x, y, z and `lod`: `+ - * / ^`, `min max abs floor ceil round sqrt pow clamp lerp sin cos exp log`, `pi`; e.g. `2*x+1` |

**Binding.** Press **C** on a value node, use the arrows to move to the target, and press Enter. If the target is an Expr, the value goes into a free x/y/z input. Otherwise a list opens: pick the numeric parameter (or `seed`) to bind.
- A bound parameter shows `← source` in the inspector and takes the source's value, clamped and without variation.
- **×** or **U** (edit mode) unbinds; **X** clears all inputs and bindings of a node.
- Bindings are drawn as dashed lines; seed bindings are faint.
- In `.species` files: `segments=@trunk_segments`, `seed=@inputs.structure`, `expr="2*x+1"`.

Trunk has two segment counts: `segments` (vertical, along the stem) and `sides` (radial, tube sides). Wood's `sides` applies to the branches.

A float or integer parameter's effective value is `clamp(value + variation × r, min, max)`, with r in [−1, 1) from the node's seed. The inspector's "now" column shows it. With all variations at 0 the seeds still change branch and leaf placement, but not the parameters.

Effects of the channels:
- `foliage` changes leaves and the leaf texture (it depends on the leaf shape).
- `texture` changes only the images.
- `structure` changes the branching; the leaves follow it.

### LOD level: a network input

The **LOD** field in the seeds panel (0 = full detail … 3) is an input to the whole network, like the seeds, but chosen at run time rather than stored per plant. Nothing is derived from it implicitly:

- **Per-LOD values:** tick **per LOD** on a numeric row to give it four values, edited in the fields L0 to L3. The field of the current LOD input is outlined; the slider and arrow keys edit that one. Key **L** in edit mode toggles it. In `.species` files: `sides=10|7|4|3` (variation after `~` applies to all levels).
- **Branches `max_lod`:** the branch order exists only up to this level, e.g. order 3 up to LOD 1, order 2 up to LOD 2.

The library species set per-LOD values for:
- Trunk `segments`
- Wood `sides` and `min_radius`
- LeafShape `detail`
- Leaves `density` and `scale` (fewer, larger leaves)

Results are cached per level, so switching between levels is instant after the first visit. Export GLB writes all four levels into one file.

### Species library

**Library…** in the toolbar loads a built-in species. The files are also in `species/`.

| Species | Notes |
|---|---|
| `beech_like` | European beech: broad dome, distichous twigs, smooth grey bark (the default) |
| `spruce` | Norway spruce: narrow cone, whorled branches with hanging side shoots, needle sprays (`LeafShape` outline `spray`), scaly red-brown bark |
| `scots_pine` | Scots pine: tall bare bole, high flat crown, needle bunches at the shoot ends (`tuft`), orange upper bark |
| `larch` | European larch: open cone, drooping side shoots, light-green short-shoot needle bunches (`tuft`) |
| `silver_birch` | silver birch: slender, pendulous twigs, small serrate leaves, white bark with dark lenticels |
| `oak` | pedunculate oak: short massive bole, wide crooked crown, lobed leaves, deeply furrowed bark |
| `hazel` | common hazel: multi-stemmed shrub (a short trunk whose first branch order forms the stems) |

Conifer needles are aggregated: a "needle" triangle is wider than a real needle and stands for a bundle. Otherwise the needles are sub-pixel and the crown looks bare at tree scale (docs/09 §1).

### Wireframe

The **Wireframe** node (between Merge and Output in every library species, `enabled=off`) adds the triangle edges as a line part:
- In the viewport: drawn over the shaded mesh.
- In the export: a separate line mesh `lodN/debug:wire` with material `debug:wire` and `extras.debug = true`, so the game viewer can switch it on while walking over the terrain. `parts` limits it to wood or leaves.

**all ›** / **all ⟳** (in the Inputs node panel) step or randomise the unlocked seeds. **+ keep in list** collects seed vectors; Save seeds writes them as a `.seeds` file (one `structure foliage texture` line per plant).

## Screen layout

- **Top two thirds:** the node table. Texture nodes show their result in the right third of the cell.
- **Bottom left:** the selected node, its parameters in newspaper-style columns: the first column fills to the bottom of the panel, then the next (reflowing when the window or the border changes; more columns scroll sideways; dotted lines between columns), grouped by kind: numbers, choices, colours, text, with a rule between groups; a group that fits into one column moves to the next column as a whole instead of splitting, unless that would need more columns than the panel shows; then groups split. Numbers are slider bars: drag inside the frame (Ctrl: fine), double-click to type a value. Per row: variation (±), per-LOD checkbox, the effective value. The panel is hidden when the cursor is on an empty cell. Selecting the Inputs node also shows the instance values: the three seeds, the LOD level and the kept seed list.
- **Bottom right:** the preview (1/8 of the width); **Space** shows it full screen.
- **Border:** drag the line between the table and the bottom part (it turns thick blue under the mouse); the height is remembered.
- **Bottom edge:** the key bar for the current mode.
- **Help:** lists all keys and operators. Esc closes it; PgUp/PgDn, Home/End scroll it.

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
| P | preview this node: skeleton as lines, leaf shape, placements, mesh, texture maps; P again returns to the output |
| Space | full-screen preview on / off (Esc also leaves it) |
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

**Export GLB** writes glTF 2.0 binary with all LOD levels:
- root nodes `lod0` … `lod3` (`extras.lod`), each with meshes `lodN/wood` (material `bark`) and `lodN/part:leaves` (material `leaves`, double-sided, one connected component per leaf), plus `lodN/debug:wire` (lines) if the Wireframe node is enabled;
- attributes `POSITION`, `NORMAL`, `TANGENT`, `TEXCOORD_0`, `COLOR_0` (per-leaf tint);
- embedded PNG base colour, normal and metallic-roughness maps;
- units metres, y up, origin at the stem base;
- `extras`: species name, species hash, seeds, triangles per LOD.

The file name carries the seeds: `<species>_<structure>_<foliage>_<texture>.glb`.

Check an export with the reader the cookers use:

```sh
clang -O1 tools/plant_editor/check_glb.c -o build/check_glb.exe
build/check_glb.exe beech_like_1_1_1.glb
```

## Files

- `species/*.species`: the library species (also built into the page, **Library…**).
- `js/ops.js`: the operator table. Each node type is declared once, and the inspector, file format, variation and help come from it. The C port in `src/gen/` (docs/plans/procedural-content-core.md) follows the same table.
- `js/plant.js`: skeleton, branching, tropism, pruning, tube mesher, leaf organ, leaf placement.
- `js/texture.js`: bark and leaf texture generators.
- Others: `js/graph.js` evaluation and cache; `js/species.js` file formats; `js/editor.js` table and inspector; `js/view.js` WebGL2; `js/gltf.js` export.

## Limits (v1)

- **Branching:** level by level (Weber–Penn-like). Light-driven growth (docs/14 §2.1) is a planned node.
- **Leaf colour:** from a picked colour, not yet PROSPECT-D.
- **Not yet:** wind rig in the export, aggregate representations for far LODs (impostors), branch collars, pipe-model radii.
- **Leaves are enlarged clusters:** `Leaves.scale` > 1 by default, to keep the triangle count near 100–150 k; the real leaf count of a mature beech is ~10× higher.
