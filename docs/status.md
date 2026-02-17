# dx9mt Status

## What This Is

D3D9-to-Metal translation layer for Wine WoW64 on Apple Silicon. Target: Fallout: New Vegas (FNV). Replaces the DXVK/Vulkan/MoltenVK chain with a direct D3D9-to-Metal path.

## Architecture

```
PE32 Frontend (d3d9.dll, i686-mingw32)
  Implements D3D9 COM interfaces
  Reports as NVIDIA GeForce GTX 280 (VendorId 0x10DE)
  SM3.0 D3DCAPS9 matching DXVK reference values
  Captures all API state + draw calls as packets
  Copies VB/IB/decl/constant/texture(x8)/shader bytecode to upload arena
  Writes per-frame data to shared memory IPC file
  VEH crash handler with BSShader factory NULL-return patches
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

## Current Verified State (2026-02-17)

### Working
- **Main menu**: renders correctly with proper colors, alpha blending, depth testing
- **Save game loading**: no crash (previously crashed on BSShader factory NULL return)
- **In-game entry**: can enter gameplay, ESC overlay texture visible
- ~19 draws/frame on menu across DXT1, DXT3, DXT5, A8R8G8B8 textures
- Texture caching by (object_id, generation) with periodic 60-frame refresh
- Full D3D9 fixed-function TSS combiner evaluation in Metal fragment shader
- **D3D9 SM2.0/SM3.0 shader bytecode -> MSL transpiler** with caching
- **Adapter identity**: NVIDIA GeForce GTX 280 (VendorId 0x10DE, DeviceId 0x0611)
  - Critical for FNV shader package selection (game matches adapter name to GPU table)
- **D3DCAPS9**: comprehensive SM3.0 caps audited against DXVK reference
- **Multi-texture support** (8 stages): array-based pipeline, per-stage upload/cache
- **Depth/stencil support**: per-RT Depth32Float, MTLDepthStencilState cache
- **Cull mode**: D3DCULL_CW/CCW/NONE -> MTLCullMode per-draw
- Render-target texture routing (offscreen RT -> shader-readable)
- FVF-to-vertex-declaration conversion for legacy FVF draws
- Alpha blending and alpha test with configurable function
- D3D9 default state initialization
- VEH crash handler with BSShader factory patches (safety net)
- Mouse interaction audible (game audio works, cursor movement detected)
- No contract violations in packet stream
- 10/10 backend contract tests passing

### Known Issues
- **Black viewport in-game**: ESC overlay renders but 3D world is black
- **Loading screen hang**: loading bar doesn't animate, requires Cmd+Tab to continue
- **Cursor clipping**: cursor disappears when not hovering main menu buttons
- **Save thumbnail**: preview image not shown when hovering over save files
- **STUB logging unconditional**: all stub methods now log every call (was sampled)

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
| RB5 | Active | In-game rendering: save loading fixed, viewport black, loading hang |
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

# VEH patches (BSShader factory NULL skip)
grep "PATCH" /tmp/dx9mt_runtime.log

# Contract errors (should be empty)
grep -E "packet parse error|sequence out of order|draw packet missing" /tmp/dx9mt_runtime.log

# Stub methods hit
grep "STUB" /tmp/dx9mt_runtime.log | sort | uniq -c | sort -rn
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
    d3d9.c                 IDirect3D9 factory (caps, format checks, adapter ID, device creation)
    d3d9_device.c          IDirect3DDevice9 + all COM objects (~5700 lines)
    d3d9_perf.c            D3DPERF_* stubs + legacy exports
    dllmain.c              DLL entry point, VEH crash handler + BSShader patches
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
    backend_bridge_contract_test.c   10 contract tests
```
