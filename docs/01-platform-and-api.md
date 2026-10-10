# 01 — Platform, OS and API

## Recommendation

- **Vulkan 1.4 as the only GPU API.** One backend, no rendering hardware interface (RHI) abstraction layer. A second API (D3D12, Metal) would be a second implementation of the same ~30 renderer-backend functions, selected at compile time — not a virtual interface.
- **Develop on Linux; keep Windows building and benchmarked from day one.** Only the platform layer (~2–3k lines)
  differs, so the choice of primary OS stays cheap to reverse.
- **Shaders in Slang**, compiled offline to SPIR-V; runtime compilation only for hot reload in dev builds.
- **clang on both OSes**, unity builds driven by a shell/batch script, no CMake.

## Linux vs Windows with an NVIDIA GPU (state: October 2026)

The "Linux is slow on NVIDIA" reputation comes from running **D3D12 games through vkd3d-proton** (D3D12 → Vulkan
translation): descriptor-heap emulation and ray-tracing paths cost 10–30 %+ on NVIDIA. Khronos answered with
`VK_EXT_descriptor_heap` (Vulkan 1.4.340, Jan 2026); NVIDIA supports it from the R595 Linux driver, and vkd3d-proton
merged a heap path in 2026 (still behind a toggle pending driver fixes). None of this applies to a **native Vulkan**
engine: the proprietary driver shares its core with Windows, and native Vulkan titles are reported within a few
percent of Windows. We will verify this ourselves — identical code makes a clean A/B.

| Topic | Linux (NVIDIA proprietary, R595+) | Windows | Consequence for us |
|---|---|---|---|
| Native Vulkan performance | ~parity (reported) | baseline | A/B benchmark both from M0 |
| New Vulkan extensions | Same release cadence, plus Vulkan beta drivers | same | — |
| Presentation | Wayland default; explicit sync since 555; VRR fine | mature | — |
| HDR output | Wayland color-management protocol; KDE Plasma 6 most mature, GNOME 48+ | mature | Develop HDR path on Windows first if the compositor gets in the way |
| Low latency | `VK_NV_low_latency2` (Reflex); Wayland swapchain fix in 615.71.09 (Sep 2026) | same | — |
| GPU tools | RenderDoc, Nsight Graphics (frame debugger, GPU Trace, shader debugger), Nsight Systems, Aftermath, Tracy | same + PIX (D3D12 only) | No loss for Vulkan |
| CPU debugger | gdb/lldb + frontends; RAD Debugger Linux port early, source-only | Visual Studio, RemedyBG, RAD Debugger | **Main Windows advantage** — debug sessions on Windows when needed |
| NVIDIA SDKs | DLSS SR/DLAA/Ray Reconstruction via NGX on Vulkan; multi-frame generation is D3D12-only | full set incl. Streamline | Frame generation is pointless at a 30 FPS base anyway |
| Other upscalers | FSR 3.1 (open source, Vulkan); FSR 4 officially D3D12-only (FidelityFX SDK 2.0); XeSS has Vulkan | same | Own TAA is the portable baseline |
| Async file I/O | io_uring | IoRing / DirectStorage 1.4 (Zstd, open-source GPU Zstd shader, GACL) | Own path: io_uring/IoRing → staging → GPU or CPU decompression |
| Market reach | SteamOS / Steam Machine (Jun 2026) are AMD-only; SteamOS NVIDIA support not before 2027 | dominant PC gaming OS | Shipping would require Windows; research does not |

Portability bonus: Vulkan also covers macOS via KosmicKrisp (LunarG, Vulkan 1.4-conformant on Apple Silicon, in the Vulkan SDK since Sep 2026, needs macOS 26) or MoltenVK, and Android. Consoles would need their own backends — out of scope.

**macOS (Apple Silicon, e.g. MacBook Air M4) as a secondary dev platform:** the same code and `build.sh` via KosmicKrisp; SDL3 handles the window and Metal surface, the platform layer loads the Vulkan loader through SDL and enables `VK_KHR_portability_enumeration`/`_subset` when present (MoltenVK fallback). Required features are checked at startup and missing ones are named. Not a measurement target (thermal throttling, shared memory). Later milestones need mesh shaders, ray query and 64-bit image atomics; whether KosmicKrisp exposes them decides how far the Mac follows past M0a — beyond that, M1+ paths need a fallback or stay Windows/Linux-only.

**Second test GPU:** an AMD RDNA2+/RDNA3 card under Linux (RADV) is the cheapest way to keep the code vendor-neutral and
covers the Steam Machine class.

## Vulkan feature baseline

### Required (target tier)

| Feature | Use |
|---|---|
| Vulkan 1.4 core | dynamic rendering, synchronization2, timeline semaphores, buffer device address (BDA), descriptor indexing, scalar block layout, 8/16-bit storage and arithmetic, subgroup ops + size control, maintenance4–6, push descriptors |
| `VK_EXT_mesh_shader` | cluster and grass hardware rasterization (Turing+, RDNA2+, Arc) |
| `shaderBufferInt64Atomics`, `fragmentStoresAndAtomics` | 64-bit visibility buffer (depth \| payload via atomic max) in a storage buffer |
| `VK_KHR_acceleration_structure` + `VK_KHR_ray_query` | GI, reflections, optional shadows — inline ray queries from compute |
| `VK_KHR_swapchain`, `VK_EXT_swapchain_colorspace`, `VK_EXT_hdr_metadata` | SDR + HDR10 output |
| `VK_EXT_memory_budget` | live VRAM budget — essential on 8 GB |

### Optional (detected at startup)

| Extension | Use |
|---|---|
| `VK_KHR_unified_image_layouts` (2025) | `GENERAL` layout everywhere → most layout transitions disappear |
| `VK_EXT_descriptor_heap` (Jan 2026) | later replacement for the single bindless descriptor set (isolated in one file) |
| `VK_EXT_opacity_micromap` | alpha-tested foliage in the BVH without any-hit shaders |
| `VK_KHR_ray_tracing_pipeline` + `VK_EXT_ray_tracing_invocation_reorder` (SER, Vulkan 1.4.333) | reference path tracer |
| `VK_NV_cluster_acceleration_structure`, `VK_NV_partitioned_acceleration_structure` | RTX Mega Geometry path: BLAS from clusters, animated foliage in RT (NVIDIA only, experimental tier) |
| `VK_NV_cooperative_vector`, `VK_KHR_cooperative_matrix` | neural experiments (neural texture compression, neural materials) |
| `VK_NV_low_latency2`, `VK_AMD_anti_lag` | latency reduction |
| `VK_EXT_present_timing`, `VK_KHR_present_wait2` | frame pacing |
| `VK_KHR_shader_clock` | in-shader cycle profiling (part of Roadmap 2026) |
| `VK_EXT_memory_priority`, `VK_EXT_pageable_device_local_memory` | residency behaviour under VRAM pressure |

### Deliberately not used

| Feature | Why not |
|---|---|
| Vertex input / index buffers | vertex pulling from BDA pointers; formats are ours |
| Render passes / subpasses | dynamic rendering only |
| Descriptor set per draw/material | one global bindless set; per-dispatch data = BDA pointers in push constants |
| `VK_EXT_device_generated_commands` | mesh shaders + indirect dispatch cover GPU-driven rendering |
| `VK_AMDX_shader_enqueue` (work graphs) | AMD-only, provisional; D3D12 itself drops work graphs from Shader Model 6.10 |
| Geometry / tessellation shaders | mesh shaders and compute replace them |
| Sparse residency | texture mips streamed by reallocation; geometry streamed in pages (see 04) |

### Descriptor model

One descriptor set, bound once per command buffer:
`sampled_images[]`, `storage_images[]`, a handful of immutable samplers, the TLAS. **Buffers never appear in
descriptors** — every buffer is a `u64` device address passed in push constants or stored in other buffers.
Materials reference textures by `u32` index into `sampled_images[]`.

## Shader language

| Option | + | − |
|---|---|---|
| **Slang (chosen)** | Khronos-hosted since Nov 2024; HLSL-like; SPIR-V/HLSL/Metal/WGSL/CUDA targets; NVIDIA samples use it, so new features (cooperative vectors, cluster AS) arrive early; modules; reflection; autodiff for offline fitting tools (impostors, neural compression); Khronos 2026 survey: ~34 % adoption vs HLSL ~41 % | larger, younger compiler; occasional codegen bugs → diff SPIR-V disassembly on compiler updates |
| HLSL via DXC | most widespread; mature SPIR-V backend | DXC in maintenance as Microsoft moves to clang-HLSL; new Vulkan features lag |
| GLSL | immediate access to all GL_EXT/GL_NV extensions | weak language (no modules/generics), shrinking ecosystem |

Rules:
- Write "C for the GPU": structs + functions. Generics/interfaces only where they remove real duplication.
- One header `gpu_shared.h` is included by both C++ and Slang (type aliases per language) — single definition of every
  GPU-visible struct. Scalar block layout on the Vulkan side, `static_assert(sizeof)` on the C++ side.
- Shaders compiled at build time to SPIR-V blobs; dev builds watch files and recompile + recreate pipelines.

## Toolchain

| Item | Choice |
|---|---|
| Compiler | clang ≥ 19 on Linux, clang/clang-cl on Windows (one compiler family, same warnings and codegen); MSVC optional |
| Language | C++20 subset: designated initializers, `constexpr`, `static_assert`, operator overloading for vector math only. `-fno-exceptions -fno-rtti`, no STL in runtime code |
| Build | `build.sh` / `build.bat`; unity build = one translation unit per binary (platform executable, game module, cooker). Goal: < 3 s debug build |
| ISA | x86-64-v3 (AVX2, FMA, BMI2); ARM64 later if needed |
| Runtime dependencies (vendored source) | volk (optional loader), SDL3 (window/input on Linux, optional), zstd, Tracy (dev), Dear ImGui (dev UI), DLSS SDK (optional) |
| Cooker dependencies | meshoptimizer 1.x (incl. `clusterlod.h`), cgltf, stb_image, tinyexr, a BC7/BC6H encoder (bc7enc_rdo or ISPC texcomp) |
| Not used | VMA (we allocate a few large blocks and sub-allocate ourselves), STL, Boost, CMake |

## Platform layer

One file per OS: `platform_linux.cpp`, `platform_win32.cpp`. It owns the process and calls into game and renderer
(Handmade Hero structure). Services: window + input, high-resolution timer, threads and job-system primitives, async
file I/O (io_uring / IoRing), virtual memory reserve/commit, hot reload of the game module, Vulkan instance extensions
and surface creation.

Windowing on Linux: Wayland is the default session in 2026; a native client (xdg-shell, decorations, xkbcommon,
pointer constraints) is ~2–3k lines, X11 works through XWayland but is legacy. **Start with SDL3 for window/input on
Linux behind our own platform API**; replace with native Wayland if presentation control (HDR, timing) requires it.
Win32 is written natively from the start (small and well understood).
