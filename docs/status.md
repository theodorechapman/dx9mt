# dx9mt Status

## What This Is

D3D9-to-Metal translation layer for Wine WoW64 on Apple Silicon. Target: Fallout: New Vegas (FNV). Replaces the DXVK/Vulkan/MoltenVK chain with a direct D3D9-to-Metal path.

## Architecture

```
PE32 Frontend (d3d9.dll, i686-mingw32)
  Implements D3D9 COM interfaces
  Captures all API state + draw calls as packets
  Copies VB/IB/decl/constant/texture/shader bytecode to upload arena
  Writes per-frame data to shared memory IPC file
      |
      | memory-mapped file (/tmp/dx9mt_metal_frame.bin, 16MB)
      | per-draw: viewport, scissor, VB bytes, IB bytes,
      |           vertex decl, VS/PS constants, texture data,
      |           sampler state, TSS state, blend/alpha state,
      |           depth/stencil state, VS/PS shader bytecode
      v
Native Metal Viewer (dx9mt_metal_viewer, ARM64)
  Polls IPC file for new frames
  Parses vertex declarations -> Metal vertex descriptors
  Creates Metal buffers from VB/IB data
  Translates D3D9 SM2/SM3 shader bytecode -> MSL source
  Compiles MSL and caches by bytecode hash
  Falls back to hardcoded WVP/TSS/c0 path on translation failure
  Samples textures (DXT1/DXT3/DXT5/A8R8G8B8/X8R8G8B8/A8)
  Routes draws to per-render-target Metal textures
  Depth testing with per-RT Depth32Float textures
  Alpha blending with configurable src/dst blend factors
  Encodes indexed draw calls per frame
  Renders to standalone NSWindow with CAMetalLayer
```

The PE DLL and Metal viewer are separate processes. The PE DLL runs under Wine (mingw, `_WIN32`), the viewer runs natively (`__APPLE__`). They share a 16MB memory-mapped file for frame data. A native backend dylib (`libdx9mt_unixlib.dylib`) is also built but not yet loaded by Wine -- the IPC approach bypasses the need for Wine unix lib integration.

## Current Verified State (2026-02-12)

- FNV main menu renders correctly with proper colors, alpha blending, and depth testing
- Yellow menu text (PS constant c0 tint applied), textured backgrounds, UI atlas elements
- ~19 draws/frame across DXT1, DXT3, DXT5, A8R8G8B8 textures
- Texture caching by (object_id, generation) with periodic 60-frame refresh
- Full D3D9 fixed-function TSS combiner evaluation in Metal fragment shader
- **D3D9 SM2.0/SM3.0 shader bytecode -> MSL transpiler** (RB3 Phase 3):
  - Bytecode parser: dcl, def, arithmetic, texture, matrix multiply instructions
  - MSL emitter: register mapping, swizzle, write mask, source modifiers, _sat
  - Shader function cache (bytecode hash -> MTLFunction) with sticky failure
  - Translated PSO cache (combined key -> MTLRenderPipelineState)
  - Automatic fallback to TSS/c0 hardcoded path on parse/compile failure
  - POSITIONT draws skip translation (use synthetic screen-to-NDC matrix)
  - `DX9MT_SHADER_TRANSLATE=0` env var disables for A/B comparison
- **Depth/stencil support** (RB4):
  - Depth/stencil render state transmission through full pipeline (packet -> backend -> IPC)
  - Per-render-target Depth32Float texture cache (drawable + offscreen RTs)
  - MTLDepthStencilState cache keyed by (zenable, zwriteenable, zfunc)
  - Depth attachment on every render pass (Clear with game's clear_z on first use, Load on subsequent)
  - Per-draw depth stencil state binding
  - All PSO pipelines (geometry, translated shader, overlay) include depth format
  - Stencil state fields transmitted (enable, func, ref, mask, writemask) for future use
- Render-target texture routing: draws to offscreen RTs available as shader inputs
- FVF-to-vertex-declaration conversion for legacy FVF draws
- Alpha blending (SRCALPHA/INVSRCALPHA) and alpha test with configurable function
- D3D9 default state initialization (TSS, sampler, render states, depth/stencil)
- Texture generation tracking: dirty on Lock/Unlock, AddDirtyRect, surface copy
- Mouse interaction audible (game audio works, cursor movement detected)
- No contract violations in packet stream
- 10/10 backend contract tests passing

## Milestones

| Milestone | Status | Description |
|-----------|--------|-------------|
| M0-M3 | Done | Docs, scaffold, object model, FNV compatibility |
| RB0 | Done | Packet contract: monotonic sequence, present-target metadata, draw-state IDs, upload refs |
| RB1 | Done | First visible Metal output: clear color + draw-count overlay bar |
| RB2 | Done | Structured per-draw IPC: viewport, scissor, VB/IB data, vertex decl, constants |
| RB3 Phase 1 | Done | Metal geometry pipeline: indexed draws with WVP transform, vertex colors |
| RB3 Phase 2A | Done | Texture data transmission, caching, DXT/ARGB format support |
| RB3 Phase 2B | Done | Sampler state, TSS fixed-function combiners, blend state, alpha test |
| RB3 Phase 2C | Done | PS constant c0 tint for pixel shader draws, render-target routing |
| RB3 Phase 3 | Done | D3D9 SM2/SM3 bytecode -> MSL transpiler, shader cache, PSO cache |
| RB4 | Done | Depth/stencil state, depth textures, per-draw depth testing |
| RB5 | Next | In-game rendering: cull mode, fog, multi-texture, shader hardening |
| RB6 | Pending | Performance: state dedup, buffer recycling, pipeline cache, async compile |
| RB7 | Pending | Compatibility hardening: missing stubs, edge cases, Wine unix lib |

## Build & Run

```bash
make -C dx9mt                # Build frontend DLL + backend dylib + Metal viewer
make -C dx9mt test-native    # Run 10 contract tests (Metal excluded via DX9MT_NO_METAL)
make run                     # Kill old viewer, create IPC file, launch viewer + Wine + FNV
make clear                   # Kill viewer + wineserver, remove IPC file
make show-logs               # Display runtime logs
```

## Runtime Logs

The PE DLL logs to `DX9MT_LOG_PATH` (default `/tmp/dx9mt_runtime.log`). Key patterns:

```bash
# Frame progression
grep "present frame=" /tmp/dx9mt_runtime.log | head

# Metal IPC status
grep "metal IPC" /tmp/dx9mt_runtime.log

# Contract errors (should be empty)
grep -E "packet parse error|sequence out of order|draw packet missing" /tmp/dx9mt_runtime.log
```

Present frames show `(metal-ipc)` when the IPC path is active.

## Shader Translation Diagnostics

The viewer logs shader compilation results to stderr:
```bash
# Successful compilations
# dx9mt: VS 0xABCD1234 compiled OK (12 instructions)
# dx9mt: PS 0xEF567890 compiled OK (8 instructions)

# Failures (include full MSL source + parsed IR)
# dx9mt: VS 0x... compile failed: <Metal error>
# --- VS MSL source ---
# ...
# --- end ---
```

Disable translation for A/B comparison:
```bash
DX9MT_SHADER_TRANSLATE=0 make run
```

## Frame Dump

Press 'D' in the viewer window to write per-draw state to `/tmp/dx9mt_frame_dump.txt`. Shows:
- Resolution, draw count, clear color, present render target
- Per-draw: primitive type/count, vertex format, texture info, TSS state, blend state
- Depth state: enable, write enable, compare function
- Stencil state: enable, function, ref, mask, write mask
- Sampler state, viewport, vertex data samples
- Shader bytecode: VS/PS IDs, bytecode size, version token, first 4 DWORDs
- `upload=0` means texture is cached (not missing), `upload=N` means N bytes uploaded this frame

## File Map

```
dx9mt/
  include/dx9mt/
    packets.h              Wire protocol (INIT, BEGIN_FRAME, DRAW_INDEXED, PRESENT, CLEAR)
    backend_bridge.h       Backend bridge API (init, submit_packets, present, shutdown)
    upload_arena.h         Triple-buffered upload arena for shader constants + geometry
    metal_ipc.h            Shared memory IPC format (header + per-draw entries + bulk data)
    object_ids.h           Kind-tagged object IDs ((kind << 24) | serial)
    d3d9_device.h          Device creation entry point
    runtime.h              Frontend runtime init + packet sequencing
    log.h                  Centralized logging
  src/frontend/
    d3d9.c                 IDirect3D9 factory (caps, format checks, device creation)
    d3d9_device.c          IDirect3DDevice9 + all COM objects (~5600 lines)
    d3d9_perf.c            D3DPERF_* stubs + legacy exports
    dllmain.c              DLL entry point
    runtime.c              Singleton init, backend bridge setup, packet sequencing
    d3d9.def               DLL export definitions
  src/backend/
    backend_bridge_stub.c  Packet parser, frame replay state, IPC writer (~1300 lines)
    metal_presenter.h      C-callable Metal presenter API (unused in IPC mode)
    metal_presenter.m      Objective-C Metal implementation (unused in IPC mode)
  src/common/
    log.c                  Timestamped file/stderr logging
  src/tools/
    metal_viewer.m         Standalone native Metal viewer (~2600 lines, reads IPC, renders)
    d3d9_shader_parse.h    SM2/SM3 bytecode parser: IR structs + API
    d3d9_shader_parse.c    Bytecode parser implementation (~470 lines)
    d3d9_shader_emit_msl.h MSL emitter API
    d3d9_shader_emit_msl.c D3D9 IR -> MSL source emitter (~530 lines)
  tests/
    backend_bridge_contract_test.c   10 contract tests
```
