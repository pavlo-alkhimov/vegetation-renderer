# Splat editor

Browser tool for building Gaussian splat models **by hand**, with no capture data: every splat is placed, shaped and coloured in the editor, directly or through construction tools. Open `index.html` from disk (WebGL2; no server, no build step). The model is autosaved in the browser's local storage; use **Save .splats** for files.

## What a splat is

A 3D Gaussian with:
- a position;
- three standard deviations (sigma) along its own axes;
- a rotation;
- a colour and an opacity.

It is drawn as a soft ellipse facing the camera, using EWA projection. The visible edge is at about 2 sigma, and the quad is cut at 3 sigma.

Splats are sorted back to front and alpha-blended, the correctness reference of docs/09. Typical shapes:

| Sigmas | Looks like | Use |
|---|---|---|
| one small | soft disc | leaf patch, petal, blade segment |
| two small | soft line | needle, thin stem |
| all similar | soft ball | blob, flower head, haze |

## Edge modes

Each splat has an **edge mode** and a **hardness** from 0 to 1 (both in the panel; defaults Gaussian and 0.5): a model is made for a renderer. The **edge** list in the view settings (**V**, or **E** to cycle) shows every splat in its own mode (**per splat**, the default) or forces one mode on all splats for checking.

**Compare** replaces the model with four copies side by side, left to right Gaussian, saturated, generalized, cut-off, each adapted to its mode. Flat and thin splats (leaves, blades, stems) get hard edges, round ones (flower heads) softer ones. In the saturated copy the sigmas shrink by 2 / sqrt(4 + 2 ln g) for gain g, so the visible size stays the same. In the three hard copies every chain (blade, stem) is re-sampled: each segment becomes 4 shorter, overlapping splats on a smooth curve through its neighbours, with width and colour following the curve and narrowing at the chain ends. Hard edges would otherwise show the kinks between straight segments that soft Gaussians hide. This costs splats: the example grows from 209 to 659 per hard copy. It is one undo step.

| Mode | Falloff | Hardness | Cost |
|---|---|---|---|
| Gaussian | exp(−r²/2) | ignored | baseline |
| Saturated | opacity × (1 + 7h), clamped: flat top, steeper rim, tail beyond 2σ | gain 1…8 | same; quad grows to ~3.9σ at h = 1 |
| Generalized | exp(−2 (r/2)^(2β)), β = 2^(4h) = 1…16: flat top, sharp rim **at the 2σ contour** | β | a pow per pixel; quad shrinks towards 2σ, so hard splats are cheaper |
| Cut-off | solid inside the 2σ contour, smoothstep rim of width (1 − h) | rim width | a smoothstep; quad shrinks to ~2σ |

r is in sigma units. Rims stay at least about 1–2 px wide: the generalized β is limited by the splat's size on screen, and the cut-off rim by fwidth. h = 0 gives the plain Gaussian in all modes except cut-off. The Gaussian, generalized and cut-off modes all keep the edge at the 2σ contour (presentation 2); saturated pushes it outwards.

**Lighting (L)** treats splats as material splats lit by a sun: the normal is the shortest axis. Splats are two-sided: the face towards the sun gets direct light, the other face ambient light plus some light coming through. When the camera crosses a splat's plane, the light changes smoothly, over an angle that depends on how flat the splat is. A gizmo in the lower left corner shows the world axes (X red, Y green, Z blue), the horizon and the sun direction (yellow). Round splats stay unlit.

## Controls

| Keys | Action |
|---|---|
| LMB | select a splat (Shift: toggle, Ctrl: remove); on empty space: select none |
| (hover) | the splat a click would pick is outlined (its sections) on top: blue = select, green = add, red = remove, following the held modifiers. It stays while the mouse is over the splat; after the mouse leaves an unselected splat it fades out over 3 s, a selected one goes straight back to its selection look. A click picks the splat whose 2σ ellipsoid the mouse ray enters first (the nearest along the ray; tiny splats count as at least 4 px wide), not the densest. The status line shows "click: select #i" |
| LMB drag | box select: every splat the box touches (its 2σ ellipse on screen); **Alt** held: only splats completely inside. **Shift** adds, **Ctrl** removes, otherwise the box replaces the selection. Dashed box = touching, solid = enclosed; green = add, red = remove. Esc cancels. The status line shows the current selection mode and highlights held modifiers |
| A / Alt+A | select all / none |
| G, R, S | move / rotate / scale the selection with the mouse (Blender-style) |
| … then X, Y, Z | lock to an axis: move and rotate use world axes; scale changes only that **local** sigma (without an axis: all sigmas and the spacing) |
| … then Shift | fine movement |
| … then LMB / Enter, RMB / Esc | confirm / cancel |
| N | new splat at the 3D cursor |
| Shift+D | duplicate and move (Esc keeps the copies in place) |
| Del (or X) | delete |
| C | put the 3D cursor where the mouse points (on a splat, else on the ground) |
| T | apply the current tool |
| 1 / 2 / 3 / 4 | splats presentation: splats / ellipses / centres / lines. Lines: one segment per splat along its longest axis, out to where the splat's own alpha (edge rendering mode, hardness, opacity) falls to the **outline close** level, so the line ends at the close contour of a lone splat; a splat fainter than that level gets no line. In the splat's colour (the new one is shown large at the bottom of the viewport, fading out over 2 s). Ellipses: the 2σ ellipse in each of the splat's three local planes, in the splat's colour. An ellipse is shown only when both its axes are at least 6 px across; a splat with only one such axis is drawn as a line along it (±2σ), smaller splats not at all. So a flat splat shows only its outline and a needle a line. With **slices** on (**P**, off by default) each ellipse also gets parallel sections: one every **slice distance** pixels (view settings, default 20 px) on both sides of the central one out to the 2σ edge, fixed on screen, dimmer than the central one; at most 200 per axis and 150 000 rings per frame as a safety limit. The same rules apply to the selection and hover ellipses (a tiny selected or hovered splat still gets a line along its longest axis). The transparency contours (O) are separate from this presentation. |
| E | next edge rendering mode: per splat, then all Gaussian / saturated / generalized / cut-off (also in the view settings); shown large at the bottom of the viewport like 1 / 2 / 3 |
| (selection) | selected splats are always drawn on top, with two independent toggles (keys or the view settings, both on by default): **ellipses** (I: each splat as in presentation 2, in its own colour brightened, at 50 % opacity; also the hover highlight) and **contours** (O: outlines around all selected splats, in the colour of the splats they surround, brightened). The outlines trace the transparency of the selection: they are iso-lines of the coverage (final alpha) of the selected splats exactly as drawn, with their edge rendering mode, hardness and opacity, accumulated as 1 − Π(1 − αᵢ) so overlapping splats merge. Three outlines are drawn, fading outwards: **close** and **far** coverage levels set in the view settings (defaults 45 % and 4 %, about 1.2σ and 2.5σ of a Gaussian splat at opacity 0.9; the sliders run towards lower levels on a log scale, so moving right makes the contour larger: close 95 % → 2 %, far 50 % → 0.3 %), and one **in-between** at their geometric mean (13 %). A hard-edged splat gets its lines bunched at its rim; a faint splat that never reaches a level has no line there. In view 1 the selected splats themselves are faded to 20 % opacity while their ellipses are shown. While an outline slider is being dragged, the selected splats are drawn normally (full opacity, no sections) with only the outlines, so their placement can be judged; on release the usual look returns |
| L | lighting on/off (on by default) |
| (notes) | every view toggle (1 / 2 / 3 / 4, E, L, I, O, P, and the lists and buttons in the view settings) and undo / redo (with the number of further steps) shows its new state in large bold dark red at the bottom middle of the viewport, fading out over 2 s |
| I | ellipses of the selection and the hover highlight on/off (contours not affected) |
| O | contours (outlines) of the selection on/off |
| P | slices (parallel sections of the ellipses) on/off, off by default |
| V | view settings: a panel over the viewport with rendering (1/2/3), edge rendering mode, lighting, ground grid, ellipses and contours toggles, outline levels and slice distance; hidden by default, V or Esc closes it |
| F | frame the selection (or everything) |
| Ctrl+Z, Ctrl+Y | undo, redo |
| RMB drag, MMB (Shift+RMB) drag, wheel | orbit, pan, zoom |
| H | help (Esc closes, PgUp/PgDn scroll) |

The right panel shows the selection. With one splat selected: position, sigma (logarithmic bars), rotation as Euler angles, colour, opacity, hardness and edge mode. With several: colour, opacity, hardness and edge mode for all of them. Bars work as in the plant editor: drag inside the frame (Ctrl: fine), double-click to type a value.

## Construction tools

Each tool has its parameters in the panel and runs with **Apply** or **T**. Its result becomes the selection, so tools chain naturally: build a blade, then make radial copies.

| Tool | Works on | Builds |
|---|---|---|
| Chain | cursor | blade, stem or twig: elongated splats along a curved line (length, bend, azimuth, tilt, count, width, thickness, taper, base and tip colour) |
| Leaf | cursor | flat splats along a midrib with an ovate outline (length, width, rows, azimuth, tilt, curl) |
| Blob | cursor | flower head or bud: flat splats in a sphere facing outwards (radius, count, size, flatness, seed) |
| Radial copies | selection | copies around the vertical axis through the cursor, with seeded jitter of angle, scale and tilt |
| Mirror | selection | copies mirrored across x = cursor x |
| Colour variation | selection | seeded brightness and hue variation |

The **Example** model (a grass tuft, a leaf rosette and a flower on a stalk, 209 splats) is built only with these tools; see `default_model()` in `js/splats.js`.

## Files

- **.splats:** text, one splat per line: `x y z  sx sy sz  qw qx qy qz  #rrggbb  opacity  hardness  mode` (metres, sigma; mode 0 Gaussian, 1 saturated, 2 generalized, 3 cut-off; files without hardness load with 0.5, without mode as Gaussian). It is readable and diffable, the editor's own format.
- **.ply export:** the INRIA 3D Gaussian Splatting format, with SH degree 0 colour, logit opacity, log scales and rotation `w x y z`, plus extra `hardness` and `edge_mode` properties that other viewers ignore. It opens in SuperSplat, SIBR and other 3DGS viewers. **Load** reads such files back too.

## Limits (v1)

- **Blending:** sorted alpha blending only. The stochastic path chosen in docs/09 is not here yet.
- **No view-dependent colour:** spherical harmonics beyond degree 0 are not supported.
- **Editing gaps:** no box selection, no numeric input during transforms, no grouping or layers.
- **Sorting:** done on the CPU, fine for tens of thousands of splats.
