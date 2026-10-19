# 14 — Procedural generation of plants and their textures

State: October 2026. Goal: every plant of the target biome ([06](06-vegetation.md) §1) generated from code and
parameters, geometry **and** textures, so the repository can ship its own assets (licence, [13](13-reference-maps.md)
"Repository rule") and every asset can be regenerated, varied and calibrated. Different plant classes use different
algorithms; what they share is the organ library, the texture generators and the output format.

## Summary

| Class | Structure algorithm | Organs | Textures |
|---|---|---|---|
| Broadleaf trees | self-organising growth (buds competing for light) in a stand | leaves (outline + venation), twigs | bark generators by bark type; leaf maps from venation, colour from PROSPECT-D |
| Conifers | annual-whorl rules (L-system / shoot model); pine crown by growth | needles, needle sprays, cones | scaly/plated bark; needle colour from PROSPECT-D |
| Shrubs | self-organising growth with basal shoots | as broadleaf | as broadleaf |
| Herbs, forbs, ferns | L-systems (developmental) | leaves, flowers, fronds | venation leaves, petal generator |
| Grasses, cereals | parametric blade/tiller/ear model (runtime blades stay per-frame) | blades, ears, panicles | blade colour ramps, no texture needed |
| Moss, litter, lichen | terrain material layers ([12](12-terrain.md)) | — | procedural terrain layers |

Implemented as operator families of the recipe system ([15](15-procedural-content.md)), deterministic per seed:
growth/structure, organs, textures, LOD + output. No third-party tree generator is used at runtime or in the pipeline; existing tools serve as visual
comparison only.

## 1. Survey

### Structure

| Approach | Idea | Fit for this biome |
|---|---|---|
| L-systems (Prusinkiewicz, Lindenmayer, Hanan 1988; *The Algorithmic Beauty of Plants* 1990) | rewriting rules describe development; turtle interpretation | best for herbs, flowers, regular conifer whorls; weak for crowns shaped by light and neighbours |
| Parametric (Weber & Penn 1995; Blender Sapling) | fixed recursion levels with per-level parameters | generic look, no environment response; reference only |
| Space colonization (Runions, Lane, Prusinkiewicz 2007) | branches grow towards attraction points inside a crown envelope | good crowns if the envelope is right; envelope must come from elsewhere |
| **Self-organising trees** (Palubicki et al. 2009) | buds compete for light (shadow-propagation grid) and resources (Borchert–Honda allocation); tropisms; shedding of weak branches | crown form *emerges*; the same species grows as forest, edge or solitary tree; correct leaf-off structure; **chosen for broadleaf trees and shrubs** |
| Plastic trees (Pirk et al. 2012) | existing tree adapts to light and obstacles | later: adapt pre-grown variants to their real neighbours at placement |
| Inverse procedural modelling (Stava et al. 2014) | MCMC search for generator parameters reproducing given trees | **calibration** against scans (§4) |
| Learned growth: DeepTree (Zhou et al. 2023), Tree-D Fusion (Lee et al. 2024, 600 k genus-conditioned trees grown from street images) | networks replace hand-written growth rules | research; Tree-D Fusion as possible calibration data (licence to check) |
| Functional-structural plant models (LIGNUM; Scots pine parameters for southern Finland) | shoot-level carbon and light model from forestry science | source of real shoot parameters for pine; too slow and detailed as the generator itself |

Botanical rule sets: the Hallé–Oldeman architectural models (23 models, mainly from tropical work) classify how axes
grow and branch. Massart's model (monopodial trunk, regular tiers of plagiotropic branches) is the standard example for
spruce and fir; Troll's model is commonly cited for beech and Rauh's for oak and pine — **to verify per species**
before the parameter sets are written.

### Organs and textures

| Topic | Source | Use |
|---|---|---|
| Leaf venation | Runions et al. 2005: veins grow towards auxin sources in the blade, open and closed patterns | vein mask → albedo, translucency and normal maps; vein lines also drive leaf folding |
| Leaf outline and margin | Runions thesis 2014 (margin as a propagating boundary); Peyrat et al. 2008 (L-system leaves, ageing texture) | lobed (oak), serrate (hornbeam, birch), palmate (maple), entire-wavy (beech) |
| Leaf colour, transmission, seasons | PROSPECT-D (Féret et al. 2017): reflectance and transmittance 400–2500 nm from leaf structure, chlorophyll, carotenoids, anthocyanins, brown pigments, water, dry matter | physically grounded albedo and backlit transmission; seasons = pigment trajectories (chlorophyll falls, carotenoids stay, anthocyanins rise in maple/cherry, brown pigments at senescence) → the season LUT of [06](06-vegetation.md) §5.7 |
| Furrowed bark | Lefebvre & Neyret 2002 (fractures on circular strips, propagating and widening with growth); Kratt et al. 2015 (cambial growth drives the fracture simulation, "woodification") | oak, ash, old pine, birch base, poplar, old willow |
| Smooth, scaly, plated bark | node-based feature generators: cracks, furrows, scales, lenticels (Zhou et al. 2025, 7 features, 5 species) | beech, hornbeam (smooth + lenticels), birch (white, lenticel bands, black fissures), pine upper crown (orange flakes), spruce (thin scales) |
| Fitting texture parameters to photos | inverse procedural materials: MATch (2022), DiffProxy (Hu et al. 2022), VLMaterial (2025) | calibration only: the shipped textures stay generated |

### Existing tools (comparison only)

| Tool | Approach | Why not the pipeline |
|---|---|---|
| SpeedTree | rule + growth based, commercial | licence; output not reproducible from our parameters |
| The Grove 2.3 (Blender, Houdini) | year-by-year growth simulation; European broadleaf presets | commercial; useful as a visual reference for growth-based forms |
| Infinigen (Princeton, BSD-3) | randomised procedural rules in Blender, trees and plants | permissive licence: rules may be read and ported; Python/Blender pipeline, not ours |
| Blender Sapling | Weber–Penn | generic look |

## 2. Chosen approach per class

### 2.1 Broadleaf trees and shrubs: self-organising growth

Simulated year by year on a skeleton of metamers (internode + leaf + axillary bud):

1. **Light:** shadow-propagation voxel grid (Palubicki 2009). Every bud adds shadow to a pyramid of cells below
   it; a bud's exposure Q comes from its cell. Neighbouring trees in the same grid shade each other.
2. **Resources:** Borchert–Honda allocation — exposure accumulated towards the base, distributed back towards the tips
   with an apical-control parameter λ (main axis vs laterals).
3. **Growth:** each bud grows a shoot whose metamer count follows its resource share; direction = weighted sum of
   previous direction, optimal (least-shadow) direction and tropisms (gravitropism, phototropism; negative
   gravitropism of the leader).
4. **Shedding:** branches whose exposure stays below a threshold for several years are dropped → self-pruning and a
   clean bole in forest stands, low branches on solitary trees.
5. **Radii:** pipe model / Leonardo's rule (`r_parent^a = Σ r_child^a`, a ≈ 2–2.5) from the tips down.

Species = an architecture rule set (branching order, phyllotaxis: distichous for beech and hornbeam, spiral for oak
and birch; flowering/apical behaviour) + numeric parameters (λ, tropism weights, internode length, branch angle,
shedding threshold, leaf size and density). **Stand simulation:** a plot of several trees grows in one grid, so
variants come in three habitats: forest-grown (tall bole, high narrow crown), edge (one-sided) and solitary (wide,
low crown). Age is a parameter (young, mature, old).

### 2.2 Conifers: rule-based whorls

Spruce, fir and larch follow Massart-like rules: one leader shoot per year, a whorl of N branches at its base,
laterals with their own annual whorls, branch angle and droop by order and age (spruce "comb" and "brush" branching
forms as a parameter). Written as a parametric L-system or a direct annual-shoot loop; light from the same shadow
grid only drives shedding (crown base rises in stands). Scots pine uses the growth model of §2.1 for its irregular,
flat upper crown, with LIGNUM-derived shoot parameters as the starting point. Needles: geometric, spiral on the shoot
(spruce, fir), fascicles of two (pine), clusters on short shoots (larch).

### 2.3 Herbs, forbs, ferns: L-systems

Developmental L-systems with organ templates: rosette and stem leaves, inflorescence types (raceme, umbel, capitulum
for daisy-like flowers, cyme), fronds for bracken. Growth stage is a parameter (spring rosette → flowering →
seed heads). These replace or complement the scanned ground cover of [06](06-vegetation.md) §5.14 where scans are
missing (cornflower, poppy, chamomile, Stipa).

### 2.4 Grasses and cereals: parametric

Runtime blades stay procedural per frame (§5.5 of [06](06-vegetation.md)). The generator provides per-species blade
parameters (length, width profile, curvature, stiffness, colour ramp along the blade) and the near-field meshes of
tillers, ears and panicles (wheat, rye, barley awns, maize) by growth stage (BBCH scale), used where blades alone are
not enough (crop rows close up, flowering grasses).

## 3. Textures

All maps are generated in the same tool and baked at cook time (deterministic, BC-compressed by the cooker later).

| Map set | Contents | Generator |
|---|---|---|
| Bark (per species, per age band) | albedo, height → normal, roughness, AO; cylindrical layout (u = arc length, v = height) with a constant texel density | furrowed: fracture strips with widening by radial growth; smooth: noise + lenticels + branch scars ("eyes" in beech); plated/scaly: Voronoi plates with peel layers; colour from per-species ramps (moss and algae on the weather side as a layer) |
| Leaf (per species, few variants) | albedo, transmission colour, normal, thickness; **no alpha** — the outline is mesh geometry | venation mask (Runions) → vein colour and relief; blade colour from PROSPECT-D; adaxial/abaxial sides differ (lighter underside) |
| Needle, blade | none (per-vertex colour from PROSPECT-D + variation) | — |
| Flower, petal | albedo, transmission | radial vein pattern + per-species colour ramp |
| Season LUT ([06](06-vegetation.md) §5.7) | leaf colour, transmission tint, leaf density over 64 time-of-year columns | pigment trajectories through PROSPECT-D |

PROSPECT-D gives spectra; the generator integrates them with the CIE colour-matching functions under D65 to linear
sRGB reflectance and transmittance. The specific absorption coefficient tables must be checked for licence before they
are committed; the derived RGB values can be committed in any case.

## 4. Calibration

Procedural is only useful if it is measurably right for the region:

| Quantity | Target data |
|---|---|
| Height, stem diameter, crown radius and crown base by age and habitat | forestry allometry for the species |
| Branch structure (branching angles, length ratios, radii) | terrestrial laser scans as quantitative structure models (TreeQSM; TreeScanPL10K, Polish scans of 10 k trees, licence to confirm in [13](13-reference-maps.md)) |
| Leaf area index of stands | 5–7 for beech stands (to be confirmed from literature), lower for oak and birch |
| Silhouette, crown density, colour | photographs and orthophotos as visual targets (look-dev, never as asset input) |

Fitting: inverse procedural modelling (Stava 2014) over the generator's parameters against QSM statistics; for
textures, optional MATch-style fitting of generator parameters to bark photographs.

## 5. Output

The generator is a procedural DCC. Its target is the vegetation convention of [04](04-data-representation.md):
`wood` mesh skinned to a wind rig (bones from the skeleton's branch orders), part meshes (twigs with leaves, needle
sprays, cones, flowers) placed as `PartInstance`s on bones, per-leaf buckets for seasonal thinning, `*.species`
side-car data. Until the M1 cooker and the assembly runtime exist, it writes the baseline runtime format directly
(meshlets per LOD, parts flattened); see the plan [plans/procedural-plants.md](plans/procedural-plants.md).

Species are recipes of the procedural content system ([15](15-procedural-content.md)): `assets/recipes/<name>.recipe`,
versioned in the repository, the generators above as operators; seeds make every variant reproducible bit for bit.

## 6. Validation

- **Gallery** ([06](06-vegetation.md) §5.14 tools): LEVELS layout per species, leaf-on / leaf-off, three habitats,
  compared with the baseline trees and with reference photos.
- **Numbers printed by the generator:** height, crown radius, crown base, DBH, leaf count and leaf area, LAI of a
  stand plot, triangle counts per LOD — against §4 targets.
- **Determinism:** the same seed produces identical output files.
- **Runtime cost:** per-LOD triangle counts and measured ms in the world view against the vegetation budget
  ([06](06-vegetation.md) §5.12).

## 7. Risks

| Risk | Mitigation |
|---|---|
| Growth parameters hard to tune per species | start with two archetypes (beech, spruce), calibrate against data before adding species |
| Full-detail trees too heavy for the current runtime (no cluster DAG, no per-meshlet culling) | generator emits explicit LODs; the full leaf geometry is shown in the gallery and close up only until the M1/M2 geometry pipeline exists |
| Procedural bark looks synthetic | bark types split by generator; photo-fitted parameters; moss/lichen layer; weathering by height and exposure |
| Leaf colour data licence (PROSPECT tables) | commit only derived RGB values if the tables cannot be redistributed |
| Botanical rules wrong for a species | architecture models checked against botanical literature per species before parameter work (§1) |

## References

See [references.md](references.md), section "Procedural plants and textures".
