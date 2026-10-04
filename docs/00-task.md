# 00 — Task description (as understood)

My reading of the request (2026-10-04). Correct anything that is wrong; the design documents follow from this.

## Goal

Find the best real-time rendering techniques available today (October 2026) — above all for **vegetation and grass**
of the **Central and Eastern European temperate biome** (not tropical) — and design, later build, a renderer that
uses them.

## What the work must cover

1. **Engine structure:** the renderer's pipeline and how it exchanges data with the rest of the simulation ("game").
2. **Data representation:** assets in editors and DCC-tool exporters (read "CSM exporters" in the request as DCC),
   and their runtime form.
3. **Techniques:** the best visual quality achievable at the performance target, vegetation first.
4. **Hardware mapping:** how data and work map onto the CPU and GPU.

## Targets and constraints

- **Performance:** ≥ 30 FPS at 1920×1080. This is deliberately below market norms (60 FPS, 4K) so that better
  techniques fit; the target is negotiable.
- **Hardware:** an RTX 4060 (8 GB) is the strongest machine available, weaker ones exist. They are for testing and
  benchmarking, not the reference target.
- **OS:** Linux preferred, Windows acceptable; the result should be as OS-independent as possible. Advice on Linux
  with an NVIDIA GPU was requested.
- **Style:** simplicity, hardware alignment, data-oriented design, no unnecessary abstraction (Casey Muratori,
  Jonathan Blow).
- **Language:** C++ written in C style; C++ features only where they clearly pay off.

## Current phase: design draft (done)

- A draft of the rendering system with **no implementation**, defining how data passes between subsystems and how
  everything maps to hardware.
- Includes the OS/API recommendation: Linux + Vulkan 1.4, Windows kept building.
- Delivered as [README](../README.md), documents 01–08 and [references](references.md).

## Out of scope for now

- Implementation code.
- Consoles, mobile, shipping as a product.
- A general-purpose engine: the renderer serves this content on this hardware class.

## Next

Confirm the assumptions below, then implement in milestones M0–M8 ([08](08-validation-roadmap.md)).

## Assumptions to confirm

- Photoreal style.
- Time of day and seasons change at runtime, so no baked lighting.
- Ray-tracing hardware is required for the main quality tier; a non-RT tier comes later.
- World up to 16 × 16 km, vistas to 5–10 km.
- Native 1080p rendering, no upscaling by default.
- Interaction: trampling, grass cutting, tree removal.
- Authoring in Blender and Houdini; SpeedTree optional.
- A viewer/editor with one character as interactor, not a full game.
