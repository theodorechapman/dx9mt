# dx9mt Status

## What This Is

D3D9-to-Metal translation layer for Wine WoW64 on Apple Silicon. Target: Fallout: New Vegas (FNV). Replaces the DXVK/Vulkan/MoltenVK chain with a direct D3D9-to-Metal path.

## Architecture

```
PE32 Frontend (d3d9.dll, i686-mingw32)
  Implements D3D9 COM interfaces
  Captures all API state + draw calls as packets
  Copies VB/IB/decl/constant/texture(x8)/shader bytecode to upload arena
  Writes per-frame data to shared memory IPC file
      |
      | memory-mapped file (/tmp/dx9mt_metal_frame.bin, 16MB)
      | per-draw: viewport, scissor, VB bytes, IB bytes,
      |           vertex decl, VS/PS constants, texture data (8 stages),
      |           sampler state (8 stages), TSS state, blend/alpha state,
      |           depth/stencil state, cull mode, VS/PS shader bytecode
      v
Native Metal Viewer (dx9mt_metal_viewer, ARM64)
  Polls IPC file for new frames
  Parses vertex declarations -> Metal vertex descriptors
  Creates Metal buffers from VB/IB data
  Translates D3D9 SM2/SM3 shader bytecode -> MSL source
    - Flow control (if/else/endif, rep/endrep, break/breakc)
    - Multi-texture sampling (tex0..tex7)
    - Integer/boolean constant declarations (defi/defb)
  Compiles MSL and caches by bytecode hash
  Falls back to hardcoded WVP/TSS/c0 path on translation failure
  Samples textures (DXT1/DXT3/DXT5/A8R8G8B8/X8R8G8B8/A8)
  Binds up to 8 textures+samplers per draw (stages 0-7)
  Routes draws to per-render-target Metal textures
  Depth testing with per-RT Depth32Float textures
  Cull mode (NONE/CW/CCW -> Metal front/back/none)
  Alpha blending with configurable src/dst blend factors
  Encodes indexed draw calls per frame
  Renders to standalone NSWindow with CAMetalLayer
```

The PE DLL and Metal viewer are separate processes. The PE DLL runs under Wine (mingw, `_WIN32`), the viewer runs natively (`__APPLE__`). They share a 16MB memory-mapped file for frame data. A native backend dylib (`libdx9mt_unixlib.dylib`) is also built but not yet loaded by Wine -- the IPC approach bypasses the need for Wine unix lib integration.

## Current Verified State (2026-02-16)

- FNV main menu renders correctly with proper colors, alpha blending, and depth testing
- ~19 draws/frame across DXT1, DXT3, DXT5, A8R8G8B8 textures
- Texture caching by (object_id, generation) with periodic 60-frame refresh
- Full D3D9 fixed-function TSS combiner evaluation in Metal fragment shader
- **D3D9 SM2.0/SM3.0 shader bytecode -> MSL transpiler**:
  - Bytecode parser: dcl, def, defi, defb, arithmetic, texture, matrix multiply instructions
  - Flow control: if/ifc/else/endif, rep/endrep, break/breakc with comparison operators
  - Multi-texture: arbitrary PS sampler indices (s0..s7) with per-stage texture/sampler binding
  - MSL emitter: register mapping, swizzle, write mask, source modifiers, _sat
  - Integer constant registers (defi -> float4 i#) for rep loop counts
  - Boolean constant registers (defb -> float4 b#) for if predicates
  - Shader function cache (bytecode hash -> MTLFunction) with sticky failure
  - Translated PSO cache (combined key -> MTLRenderPipelineState)
  - Automatic fallback to TSS/c0 hardcoded path on parse/compile failure
  - POSITIONT draws skip translation (use synthetic screen-to-NDC matrix)
  - `DX9MT_SHADER_TRANSLATE=0` env var disables for A/B comparison
- **Multi-texture support** (8 stages):
  - Array-based texture/sampler pipeline (tex_id[8], sampler_*[8]) across all layers
  - Per-stage texture data upload in IPC bulk region
  - Per-stage texture caching and Metal texture creation
  - Translated shaders bind all active texture stages at matching [[texture(N)]]/[[sampler(N)]]
  - Fixed-function TSS fallback remains stage-0-only
- **Depth/stencil support**:
  - Depth/stencil render state transmission through full pipeline
  - Per-render-target Depth32Float texture cache
  - MTLDepthStencilState cache keyed by (zenable, zwriteenable, zfunc)
  - Stencil state fields transmitted for future use
- **Cull mode**: D3DRS_CULLMODE plumbed through full pipeline, D3DCULL_CW/CCW/NONE -> MTLCullMode
- Render-target texture routing: draws to offscreen RTs available as shader inputs
- FVF-to-vertex-declaration conversion for legacy FVF draws
- Alpha blending (SRCALPHA/INVSRCALPHA) and alpha test with configurable function
- D3D9 default state initialization (TSS, sampler, render states, depth/stencil, cull=CCW)
- Mouse interaction audible (game audio works, cursor movement detected)
- No contract violations in packet stream
- Frontend block-compressed surface copy hardening landed:
  - block-aligned rect validation for DXT1/DXT3/DXT5 copies
  - scaling disallowed for block-compressed surface copies
  - `ColorFill` rejected for block-compressed surfaces
- `D3DCREATE_MULTITHREADED` synchronization hardening landed in frontend:
  - per-device critical-section guard initialized at device creation
  - full `IDirect3DDevice9` vtbl now routes through lock-aware wrappers
  - VB/IB/surface/texture lock/unlock and dirty-marking paths now also honor the same device guard
  - targets save-load crash path where FNV creates device with `behavior=0x00000054`
- `test-native` passes:
  - backend contract tests
  - frontend Wine regression tests (`frontend_surface_copy_test`)

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
| RB5 | Active | In-game rendering + stability: cull/flow/multi-texture done; multithread guard landed; fog/stencil ops remain |
| RB6 | Pending | Performance: state dedup, buffer recycling, pipeline cache, async compile |
| RB7 | Pending | Compatibility hardening: missing stubs, edge cases, Wine unix lib |

## Build & Run

```bash
make -C dx9mt                # Build frontend DLL + backend dylib + Metal viewer
make -C dx9mt test-native    # Backend contract + frontend Wine regression tests
make run                     # Kill old viewer, create IPC file, launch viewer + Wine + FNV
make run-wine                # Launch FNV with builtin Wine d3d9 (native wined3d sanity path)
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
- Per-draw: primitive type/count, vertex format, blend state, cull mode
- Per-stage texture info: id, generation, format, size, upload status (for all active stages 0-7)
- Per-stage sampler state: min/mag/mip filter, address modes
- TSS state (stage 0), depth state, stencil state
- Viewport, vertex data samples
- Shader bytecode: VS/PS IDs, bytecode size, version token, first 4 DWORDs
- `upload=0` means texture is cached (not missing), `upload=N` means N bytes uploaded this frame

## File Map

```
dx9mt/
  include/dx9mt/
    packets.h              Wire protocol + DX9MT_MAX_PS_SAMPLERS (8 texture stages)
    backend_bridge.h       Backend bridge API (init, submit_packets, present, shutdown)
    upload_arena.h         Triple-buffered upload arena for shader constants + geometry
    metal_ipc.h            Shared memory IPC format (header + per-draw entries + bulk data)
    object_ids.h           Kind-tagged object IDs ((kind << 24) | serial)
    d3d9_device.h          Device creation entry point
    runtime.h              Frontend runtime init + packet sequencing
    log.h                  Centralized logging
  src/frontend/
    d3d9.c                 IDirect3D9 factory (caps, format checks, device creation)
    d3d9_device.c          IDirect3DDevice9 + all COM objects (~7000 lines, includes multithread wrappers)
    d3d9_perf.c            D3DPERF_* stubs + legacy exports
    dllmain.c              DLL entry point
    runtime.c              Singleton init, backend bridge setup, packet sequencing
    d3d9.def               DLL export definitions
  src/backend/
    backend_bridge_stub.c  Packet parser, frame replay state, IPC writer (~1350 lines)
    metal_presenter.h      C-callable Metal presenter API (unused in IPC mode)
    metal_presenter.m      Objective-C Metal implementation (unused in IPC mode)
  src/common/
    log.c                  Timestamped file/stderr logging
  src/tools/
    metal_viewer.m         Standalone native Metal viewer (~2700 lines, reads IPC, renders)
    d3d9_shader_parse.h    SM2/SM3 bytecode parser: IR structs, comparison enum, API
    d3d9_shader_parse.c    Bytecode parser implementation (~500 lines, flow control + multi-tex)
    d3d9_shader_emit_msl.h MSL emitter API
    d3d9_shader_emit_msl.c D3D9 IR -> MSL source emitter (~590 lines, flow control + defi/defb)
  tests/
    backend_bridge_contract_test.c   Backend packet contract suite
    frontend_surface_copy_test.c     Frontend Wine regression checks for DXT/linear copies
```
