# 02 — Engine architecture and data flow

## Principles

1. **The renderer is specific, not generic.** It renders this content (terrain + vegetation + some props/characters)
   on this class of hardware. No plugin system, no RHI, no scene-graph framework.
2. **Data first.** Every subsystem boundary is a block of memory with a documented layout, not an interface. Functions
   transform arrays.
3. **One writer per datum per frame.** The game owns simulation state, the renderer owns GPU state and all
   render-only data. Each consumes the other's output as an immutable snapshot.
4. **Lifetimes by arena, identity by handle.** No per-object heap allocation in the frame loop, no ownership graphs.
   Objects are indices (+ generation) into flat arrays.
5. **The frame is a function.** A fixed, hand-ordered list of passes; barriers written explicitly (helped by a
   ~200-line access tracker). No render-graph compiler.
6. **Numbers decide.** Every pass has a ms budget and every pool a MB budget (see [03](03-hardware-mapping.md)); GPU
   timers are always on.

## Subsystems

```
+--------------------------------------------------------------------------------+
| PLATFORM LAYER (executable, one file per OS)                                   |
|  window/input, timer, threads/jobs, async file I/O, virtual memory,            |
|  game-module hot reload, Vulkan instance + surface                             |
+------------+--------------------------------+----------------------------------+
             | game_update()                  | render_frame()
             v                                v
+-------------------------+  FramePacket  +-------------------------------------+
| GAME (hot-reload .so)   | ------------> | RENDERER                            |
|  simulation, camera,    |  (per-frame   |  scene mirror + handle tables       |
|  characters, physics,   |   arena)      |  streaming (geometry, textures,     |
|  wind sources,          |               |   vegetation cells)                 |
|  editor tools           | <------------ |  frame passes, Vulkan backend       |
+-------------------------+  Readbacks    +------------------+------------------+
             ^                (frame N-2)                    | submits
             |                                               v
+-------------------------+   cooked cells,  +-----------------------------------+
| ASSET / IO              |   mesh pages,    | GPU: graphics, async compute,     |
|  pack files, io_uring   | ---------------> | transfer queues                   |
+-------------------------+   textures       +-----------------------------------+
```

Both the game and the renderer read the **same cooked world-cell data** (tree positions, terrain heights, density
maps): the game for collision/AI/queries, the renderer for drawing. The data is read-only, so nothing has to be
synchronized at runtime — only events ("tree 123456 cut down") travel through the packet.

## Module boundaries (all plain C functions over structs)

```c
// Exported by the game module (hot-reloadable). Platform passes memory and input; game fills the packet.
void game_update(GameMemory* mem, const Input* input, const Readbacks* rb, FramePacket* out);

// Renderer API, called only by the platform main loop. The game never calls the renderer.
Renderer* render_init(PlatformApi* p, Arena* perm, const RenderConfig* cfg);
void      render_frame(Renderer* r, const FramePacket* fp, Readbacks* out);
void      render_resize(Renderer* r, u32 width, u32 height);
```

`PlatformApi` is a struct of function pointers only because it crosses the hot-reload boundary (Handmade Hero
pattern) — not to abstract anything. Render-object handles are allocated by the **game** (its own index + generation
free list); `SCMD_CREATE` carries the handle and the renderer maps handle → GPU slot. Nothing crosses the hot-reload
boundary from game to renderer.

## Threads and frame timeline

| Thread | Work |
|---|---|
| Main | input → `game_update` → `render_frame` (prepare uploads, record ~200 commands, submit, present) |
| Workers (cores − 2) | job system: `parallel_for` over arrays (sim, packet building, streaming decisions), Zstd decompression |
| I/O | io_uring/IoRing submission + completion, transfer-queue submits; no decompression (Zstd runs 1–2 GB/s per core, a 3 GB/s burst needs 2–3 workers) |

**No render thread.** With GPU-driven rendering the CPU records a few hundred commands per frame (< 0.5 ms); a
dedicated render thread would add a frame of latency for nothing. **2 frames in flight.**

```
            frame N-1                    frame N                      frame N+1
CPU main  | sim | prep | rec+sub |~pace~| sim | prep | rec+sub |~pace~| sim | ...
GPU gfx         |========== GPU N-1 ==========|========== GPU N ==========|
GPU async            |== GI/denoise N-1 ==|        |== GI/denoise N ==|
GPU copy  ..streaming uploads, continuous, timeline-semaphore signalled........
Readback                                  results of N-2 read here ^
```

Latency at 30 FPS: input is sampled after a pacing wait chosen so that the submit lands just before the GPU becomes
free (Reflex-style; `VK_NV_low_latency2` or own estimate from present timing). With FIFO presentation on a 60 Hz
display: CPU ~4 ms + GPU ~27 ms + vblank wait 0–16 ms + scanout ~16 ms ≈ **50–65 ms** (1.5–2 frames). VRR displays and
`VK_EXT_present_timing` remove most of the vblank term. The camera is written to the upload ring last, immediately
before submit ("late latch" at CPU level).

Simulation timestep is the game's business; the renderer only needs a monotonic `time` and `dt`. Wind and vegetation
animation are stateless functions of time (see [06](06-vegetation.md)), so variable `dt` is harmless.

## Memory

### CPU

| Arena | Lifetime | Contents |
|---|---|---|
| `perm` | process | renderer tables, asset registry, GPU pool bookkeeping |
| `level` | loaded world | cell directory, species/material tables (CPU copies) |
| `frame[2]` | one frame slot, reset when slot is reused | `FramePacket` and all its arrays, per-frame renderer scratch |
| `scratch[thread]` | one job | temporaries |

Arena = large virtual reservation, commit on demand, bump allocation, reset as a whole. `malloc` only inside
third-party code and offline tools.

### GPU (Vulkan memory types)

| Pool | Memory type | Allocator | Contents |
|---|---|---|---|
| Geometry pages | `DEVICE_LOCAL` | fixed 64 KB page pool | cluster pages (streamed), hierarchy nodes, mesh headers |
| Textures | `DEVICE_LOCAL` (images) | TLSF over a few large blocks | material textures, mip-streamed by reallocation |
| Ray tracing | `DEVICE_LOCAL` | TLSF + per-frame scratch ring | BLAS, TLAS, opacity micromaps, build scratch |
| Scene buffers | `DEVICE_LOCAL` | linear at init | instance arrays (persistent + per-frame transient part instances), vegetation cells, wind/bone buffers, radiance cache, VSM physical pool |
| Render targets | `DEVICE_LOCAL` | aliasing plan derived from the passes' declared resource lists for the active configuration (RT on/off, DLSS, 60 FPS tier, single queue, debug views); recomputed on resize or configuration change, never hand-written | transient + history targets |
| Upload ring | `DEVICE_LOCAL \| HOST_VISIBLE` (ReBAR) | ring per frame slot | packet constants, dynamic transforms, scene-command payloads, debug/UI geometry |
| Streaming staging | `HOST_VISIBLE` (system RAM) | ring | compressed pages/mips in flight → transfer queue |
| Readback | `HOST_VISIBLE \| HOST_CACHED` | per frame slot | picking, stats, streaming feedback |

CPU writes to the ReBAR ring are sequential and write-only (write-combined memory). Without ReBAR (256 MB BAR) the
same ring fits; the large pools never need CPU mapping. Sizes: [03](03-hardware-mapping.md).

### Residency and lifetime rules

Each rule closes a class of GPU hangs or corruption:

1. Residency tables (geometry pages, texture mips, RVT pages, cells) are changed **only in pass 0** (Begin), and only
   for transfers whose timeline value the graphics queue has waited on in this frame.
2. Every GPU resource release — evicted page, reallocated texture, pipeline replaced by hot reload, reused bindless
   slot — goes through the **deferred-free list of the current frame slot** and executes when that slot's fence (two
   frames later) has signalled.
3. When culling wants to descend into a non-resident page it draws the coarsest resident ancestor (error > τ
   accepted) and appends a **page request**; requests are read back together with texture and RVT feedback.
4. Bindless descriptors use `updateAfterBind` + `partiallyBound`; a slot is rewritten only after its deferred free
   has run.

## Coordinates: the render origin

The GPU never sees absolute world coordinates, but camera-relative coordinates that change every frame would
invalidate every persistent world-space structure (radiance cache, cached VSM pages, trample/cut map, wind field,
RVT). Therefore:

- The **render origin** is the corner of the camera's 256 m cell (`i32[3]`); it changes only when the camera crosses a
  cell boundary. The renderer derives it from `views[0].eye`; the game never sees it.
- All GPU world-space data is relative to the render origin in `f32`: ≤ ±512 m for content near the camera (sub-mm
  precision), larger for far cells where the precision loss is invisible.
- The per-cell offset table ("cell offset from render origin") is constant between crossings.
- On a crossing: radiance-cache keys include the origin cell, so stale entries age out; VSM clipmap page tables, the
  trample map, the wind field and the RVT indirection shift by integer pages/texels; motion vectors use the previous
  frame's origin for the previous transforms.

## Game ↔ renderer contract

Three channels, nothing else:

1. **Scene commands** — create/destroy/flag persistent render objects, remove cooked trees. Rare.
2. **Per-frame state** — views, environment, wind, interactors, dynamic transforms (all of them, every frame), skin
   palettes, debug primitives, UI. Plain structs and arrays in the frame arena.
3. **Readbacks** — answers to requests made two frames earlier (picking, stats), matched by request id.

Rules:
- The packet contains no pointers into game state; all arrays live in the frame arena.
- The renderer copies what it needs into the upload ring during `render_frame`; the packet dies with its arena slot.
- Static objects cost nothing per frame. Dynamic objects send all transforms every frame (dense array; 10k objects ×
  48 B = 480 KB) — cheaper and simpler than dirty tracking.
- Gameplay never waits for the GPU. Vegetation queries the game needs ("grass height here", "tree positions in this
  cell") use the cooked data and the same deterministic placement functions the GPU uses (shared header).
- **Information fairness:** local quality settings may change rendering cost, never what a player can see of other
  players. Concealment by vegetation is computed by shared functions (e.g. statistical grass occlusion,
  [10](10-milsim-survey.md)) used both by the renderer and by AI.
- **Views:** `views[0]` is the main camera. `views[1..3]` are optional secondary views (picture-in-picture scopes,
  mirrors): each is a reduced-resolution visibility + resolve + lighting chain with its own `vis64` and depth,
  sharing scene state but not the culling output, composited into the main view before TAA. Not before M8; whether
  PiP scopes are needed at all is a gameplay decision ([08](08-validation-roadmap.md)).
- **Editor edits** (height sculpting, density painting, tree placement) do not go through the pak files: the editor
  (game module) applies an edit to its in-memory cell data and sends a **cell patch** (cell, section, byte range)
  with the frame packet; the renderer uploads it through the streaming path. Terrain height edits need no re-cook
  ([12](12-terrain.md)); the pak is rewritten only on save.

### Draft layouts (sizes compile-checked)

```c
typedef struct { u32 v; } RHandle;                        // index:24 | generation:8, 0 = null

// Absolute position without float precision loss. Cell = 256 m. Converted to camera-relative on upload.
typedef struct { i32 cell[3]; f32 local[3]; } WorldPos;   // 24 B

typedef struct {                                          // 56 B
    WorldPos eye;
    f32 orient[4];          // quaternion, world-from-view
    f32 fov_y;              // radians
    f32 near_z;             // reversed-Z, infinite far plane
    u32 flags;              // VIEW_CAMERA_CUT (reset temporal history), ...
    u32 pad;
} ViewDesc;

typedef struct {                                          // 48 B
    f32 sun_dir[3];         // unit vector towards the sun
    f32 sun_illuminance;    // lux outside the atmosphere
    f32 sun_disk_radius;    // radians
    f32 cloud_coverage;     // 0..1 (overcast skies are the norm in the target biome)
    f32 cloud_density;
    f32 fog_density;        // ground/valley fog scale
    f32 wetness;            // 0..1 → wet bark, leaves, ground
    f32 snow;               // 0..1 → snow cover on ground and branches
    f32 season;             // 0..1 over the year → species colour ramps, leaf density
    u32 atmosphere_preset;  // index into cooked atmosphere parameters
} Environment;

typedef struct {                                          // 32 B
    f32 dir[2];             // unit, horizontal
    f32 speed;              // m/s at 10 m height
    f32 gust_amplitude;     // relative to speed
    f32 gust_wavelength;    // m, size of gust fronts travelling with the wind
    f32 turbulence;         // 0..1, small-scale noise
    f32 pad[2];
} WindGlobal;

typedef struct {                                          // 56 B
    WorldPos p0;            // capsule start / impulse centre
    f32 p1_rel[3];          // capsule end relative to p0
    f32 radius;
    f32 velocity[3];        // pushes grass/shrubs; impulse strength for wind
    u32 kind;               // INTERACTOR_CAPSULE | INTERACTOR_WIND_IMPULSE | INTERACTOR_CUT
} Interactor;

typedef struct {                                          // 64 B
    u32 kind;               // SCMD_CREATE | SCMD_DESTROY | SCMD_SET_FLAGS | SCMD_REMOVE_TREE
    RHandle h;              // allocated by the game
    u32 asset;              // mesh asset (CREATE) or cooked tree id (REMOVE_TREE)
    u32 flags;
    WorldPos pos;
    f32 orient[4];
    f32 scale;
    u32 pad;
} SceneCmd;

typedef struct { RHandle h; f32 orient[4]; WorldPos pos; f32 scale; } DynamicXform;   // 48 B

typedef struct {
    u64 frame_index;
    f64 time;               // seconds, monotonic (wind and vegetation animation)
    f32 dt;
    u32 view_count;
    ViewDesc views[4];      // [0] = main camera, [1..3] = optional secondary views (PiP scopes, mirrors)
    Environment env;
    WindGlobal wind;
    Interactor*   interactors;  u32 interactor_count;
    SceneCmd*     cmds;         u32 cmd_count;
    CellPatch*    patches;      u32 patch_count;     // editor only: { cell, section, offset, size, data }
    DynamicXform* dyn;          u32 dyn_count;
    SkinPalette*  skins;        u32 skin_count;      // 3x4 bone matrices per skinned object
    DebugPrim*    debug;        u32 debug_count;
    UiBatch*      ui;           u32 ui_count;
    ReadbackReq*  requests;     u32 request_count;   // { u32 id; u32 kind; u16 px[2]; } 12 B
} FramePacket;
```

Readbacks returned to the game: `PickResult { id, frame, RHandle object, u32 tree_id, WorldPos hit }` and
`RenderStats { frame, gpu_ms per pass, vram_used, vram_budget, visible_clusters, visible_trees, grass_blades }`.

## Renderer internals

`Renderer` is one struct of sub-structs; each file is a group of functions over its sub-struct.

| File | Responsibility |
|---|---|
| `r_device` | instance, device, queues, swapchain, memory pools |
| `r_upload` | upload ring, staging ring, transfer submissions, timeline values |
| `r_stream` | residency of geometry pages, texture mips, world cells; GPU feedback processing |
| `r_scene` | handle tables, instance arrays, render origin + cell offsets, scene-command application |
| `r_veg` | species/grass tables, vegetation cells, wind field, bone evaluation inputs |
| `r_frame` | the frame: pass order, dispatches, barriers, queue hand-offs |
| `r_pipelines` | pipeline creation, shader hot reload |
| `r_debug` | GPU timers, debug views of every buffer, debug drawing |

Optional later: renderer hot reload (requires all renderer state inside one persistent memory block; Vulkan function
pointers re-fetched after reload).

## Code conventions (C-style C++)

- Types `u8..u64`, `i8..i64`, `f32`, `f64`, `b32`; `f16` stored as `u16` with conversion helpers.
- No exceptions, RTTI, STL, iostreams, virtual functions, smart pointers, or constructors/destructors with side
  effects. Lifetimes are arenas.
- C++ features allowed where they pay: designated initializers, `constexpr`, `static_assert`, operator overloading for
  vector math, function overloading for math.
- Errors: boolean/enum results at boundaries, asserts inside; Vulkan errors are fatal in dev builds.
- CPU hot loops use SoA; GPU structs are AoS in 16/32/64-byte records (one or two 32-byte memory sectors).
- Every GPU-visible struct is defined once in `gpu_shared.h` with a `static_assert` on its size.
