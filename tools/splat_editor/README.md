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

**Lighting (L)** treats splats as material splats lit by a sun: the normal is the shortest axis, flipped towards the camera. Round splats stay unlit.

## Controls

| Keys | Action |
|---|---|
| LMB | select a splat (Shift: toggle); on empty space: select none |
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
| 1 / 2 / 3 | view splats / ellipsoids (2 sigma) / centres |
| L | lighting on/off |
| F | frame the selection (or everything) |
| Ctrl+Z, Ctrl+Y | undo, redo |
| RMB drag, MMB (Shift+RMB) drag, wheel | orbit, pan, zoom |
| H | help (Esc closes, PgUp/PgDn scroll) |

The right panel shows the selection. With one splat selected: position, sigma (logarithmic bars), rotation as Euler angles, colour and opacity. With several: colour and opacity for all of them. Bars work as in the plant editor: drag inside the frame (Ctrl: fine), double-click to type a value.

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

- **.splats:** text, one splat per line: `x y z  sx sy sz  qw qx qy qz  #rrggbb  opacity` (metres, sigma). It is readable and diffable, the editor's own format.
- **.ply export:** the INRIA 3D Gaussian Splatting format, with SH degree 0 colour, logit opacity, log scales and rotation `w x y z`. It opens in SuperSplat, SIBR and other 3DGS viewers. **Load** reads such files back too.

## Limits (v1)

- **Blending:** sorted alpha blending only. The stochastic path chosen in docs/09 is not here yet.
- **No view-dependent colour:** spherical harmonics beyond degree 0 are not supported.
- **Editing gaps:** no box selection, no numeric input during transforms, no grouping or layers.
- **Sorting:** done on the CPU, fine for tens of thousands of splats.
