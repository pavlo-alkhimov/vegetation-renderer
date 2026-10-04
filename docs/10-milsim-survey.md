# 10 — Grass and vegetation in modern mil-sims

State: October 2026. None of these studios has given a technical talk on its grass rendering; the sources are
patch notes, official wikis, developer statements and settings documentation. **Confirmed** = stated by the
developer or official documentation; **inferred** = standard path of the engine, not confirmed by the studio.

## Arma Reforger (Bohemia Interactive, Enfusion engine)

| Aspect | Technique | Status |
|---|---|---|
| Placement | Grass "clutter" is spawned from the terrain **surface materials**; surfaces are painted with grayscale masks that blend 0–1 (Arma 3 allowed only one surface per point) | confirmed |
| Rendering | Clutter models rendered around the camera up to a configurable distance, blending smoothly into the terrain texture; Bohemia mentions both "2D or 3D" grass clutter; per-material settings Distance / Lod / Quality; renderer subsystem "Terrain grass" | confirmed |
| Geometry | Small alpha-tested grass clump models with LODs (Arma lineage; vegetation modelled in Blender, textures in Substance Designer, custom foliage shader) | inferred / partly confirmed |
| Distance | User setting; "High" preset reduced from 175 m to 100 m in 1.7 (Apr 2026); grass quality and grass shadows are among the most expensive settings | confirmed |
| Interaction | "Grass flatten" components: tripod-mounted weapons flatten grass (1.2, 2024); helicopter rotor downwash flattens grass (fixed in 1.1 to stop it from altitude); thick bushes slow characters | confirmed |
| Trees | Regular entities with LODs, ~1 M per world, destructible | confirmed |
| Concealment beyond grass distance | **Far Hide** (1.8, Aug 2026): beyond the grass render distance the terrain's grass texture is applied to the character model up to the grass height, so prone players stay concealed up to ~750 m at no extra cost; grass and camo nets now also block **AI** vision | confirmed |
| Predecessor (Arma 3) | Distant units **sink ~20 cm into the ground** on surfaces with a `grassCover` coefficient; AI concealment comes from the surface parameter, not from rendered grass. A community proposal to alpha-blend soldiers into the terrain was closed as "won't fix" — Far Hide is its texture-based successor | confirmed |

## Five other photoreal mil-sims (ranked by graphics quality, 2024–2026)

| Game | Engine | Vegetation and grass | Status |
|---|---|---|---|
| **Gray Zone Warfare** (Madfinger, EA 2024) | UE5: Nanite, Lumen (optional HW-RT Lumen), DLSS 3 | SpeedTree vegetation, RealityCapture photogrammetry; "Foliage quality" sets instance density, LOD transitions and the distance where **billboard impostors** switch to 3D meshes; bushes and trees react to wind and to bodies moving through them (with sound cues; CPU-heavy foliage physics); Lumen + dense jungle is the main GPU cost; low settings cause visible impostor pop-in. Tropical biome | confirmed |
| **Hell Let Loose: Vietnam** (Expression Games, Aug 2026) | UE5 | Dense canopy and elephant grass; TSR reconstruction smears overlapping foliage when the internal resolution drops; foliage quality changes what players can see (lower = easier spotting); heavy GPU load. Tropical biome | confirmed |
| **Squad** 9.x (Offworld, UE5 since 2025) | UE 5.5: Nanite, new GI system, frame generation, Chaos | Trees via Nanite: distant foliage is now crisp instead of the blurry UE4 impostor look; foliage reworked on several maps; longer view distances. Object draw distance is the same for all settings (only LODs change), but **grass distance is client-adjustable** → long-running fairness debate. Several Eastern European maps | confirmed; grass via UE landscape-grass instancing inferred |
| **Delta Force** (Team Jade, 2024) | UE4 multiplayer, UE5 campaign; Warfare mode moving to UE5 (dynamic GI, Nanite, Sep 2026) | Large outdoor maps; standard UE foliage/grass instancing | engine confirmed, grass inferred |
| **Escape from Tarkov** (Battlestate, 1.0 Nov 2025) | Unity (Arena moving to Unity 6) | Grass rendered to ~100–200 m, then disappears; developers: longer grass would need much shader work and causes transparency problems; "grass shadows" setting adds noise; prone players beyond the grass radius are visible → long-standing complaint. Eastern European forests (Woods) | confirmed |

## Lessons for this project

1. **Nobody ships procedural blade grass in this genre.** All use instanced, alpha-tested clump meshes with wind in
   the vertex/WPO shader, cut off at 100–200 m and continued as terrain texture. Our procedural blades
   ([06](06-vegetation.md) §5.5) are ahead of the genre, not behind it.
2. **Grass radius is a gameplay variable.** Every one of these games lets the client change grass distance or
   quality, and every one has the resulting fairness problem: prone players visible beyond the grass radius,
   lower settings see more. Fixes so far: sinking units (Arma 3), painting the grass texture on characters (Reforger
   Far Hide), server-fixed settings (requested in Squad and Tarkov communities).
3. **AI must not depend on render state:** Arma and Reforger take concealment from surface data, not pixels —
   matching our rule that gameplay queries use the shared deterministic placement data ([02](02-architecture.md)).
4. **Impostor pop-in and TAA/TSR smearing of foliage** are the visible weaknesses of the UE titles → continuous LOD
   (DAG + aggregates) and native-resolution TAA tuned for foliage stay the right priorities.
5. **Interaction is gameplay:** bushes that move and make noise (GZW), flattening by downwash and tripods (Reforger),
   movement slowed by thick bushes — our interactor channel must carry these events both ways (visual bending in
   the renderer, audio and movement in the game).

## Design addition: statistical grass occlusion beyond the blade radius

A physically based version of Far Hide, valid for every object (characters, vehicles, props), identical for
rendering and for AI:

- Treat grass beyond the blade radius as a **turbid layer** of height `H` with blade-area density `ρ` (from the
  ground-cover density map and `GrassType`).
- For a surface point at height `z < H` above the ground, seen at elevation angle `θ` above the horizon, the
  transmittance is

  `T(z, θ) = exp(−G · ρ · (H − z) / sin θ)`, with `G` ≈ 0.5 (mean projected blade area).

- **Rendering:** in the material resolve, pixels of objects beyond the blade radius are replaced by the grass-layer
  colour (terrain RVT + grass BRDF) with probability `1 − T` (stochastic, resolved by TAA). Inside the radius the real
  blades occlude; across the transition the blades fade out while `T` fades in, so the occlusion statistics match.
- **Gameplay/AI:** the same function on the CPU (shared header) answers "how visible is this character from there".
- Unlike Arma's sinking and Reforger's height-based texture, it depends on the view angle: a prone player in
  50 cm grass is hidden at grazing angles and becomes visible from a hilltop — as in reality.
- Quality settings change only the blade radius and density, never the information shown: beyond the radius the
  same analytic occlusion applies on every setting.
