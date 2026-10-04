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

// Renderer API used by the platform main loop and (for handles) by the game.
Renderer* render_init(PlatformApi* p, Arena* perm, const RenderConfig* cfg);
RHandle   render_alloc_handle(Renderer* r);               // CPU free list; GPU slot filled by the next upload
void      render_frame(Renderer* r, const FramePacket* fp, Readbacks* out);
void      render_resize(Renderer* r, u32 width, u32 height);
```

`PlatformApi` is a struct of function pointers only because it crosses the hot-reload boundary (Handmade Hero
pattern) — not to abstract anything.

## Threads and frame timeline

| Thread | Work |
|---|---|
| Main | input → `game_update` → `render_frame` (prepare uploads, record ~200 commands, submit, present) |
| Workers (cores − 2) | job system: `parallel_for` over arrays (sim, packet building, streaming decisions) |
| I/O | io_uring/IoRing submission + completion, CPU decompression jobs, transfer-queue submits |

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
free (Reflex-style; `VK_NV_low_latency2` or own estimate from present timing) → ~1.3–1.6 frames (45–55 ms) to scanout.
The camera is written to the upload ring last, immediately before submit ("late latch" at CPU level).

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
| Scene buffers | `DEVICE_LOCAL` | linear at init | instance arrays, vegetation cells, wind/bone buffers, radiance cache, VSM physical pool |
| Render targets | `DEVICE_LOCAL` | static aliasing plan from pass lifetimes (recomputed on resize) | transient + history targets |
| Upload ring | `DEVICE_LOCAL \| HOST_VISIBLE` (ReBAR) | ring per frame slot | packet constants, dynamic transforms, scene-command payloads, debug/UI geometry |
| Streaming staging | `HOST_VISIBLE` (system RAM) | ring | compressed pages/mips in flight → transfer queue |
| Readback | `HOST_VISIBLE \| HOST_CACHED` | per frame slot | picking, stats, streaming feedback |

CPU writes to the ReBAR ring are sequential and write-only (write-combined memory). Without ReBAR (256 MB BAR) the
same ring fits; the large pools never need CPU mapping. Sizes: [03](03-hardware-mapping.md).

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
    RHandle h;
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
    ViewDesc views[4];      // [0] = main camera
    Environment env;
    WindGlobal wind;
    Interactor*   interactors;  u32 interactor_count;
    SceneCmd*     cmds;         u32 cmd_count;
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
| `r_scene` | handle tables, instance arrays, cell-origin table, scene-command application |
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
