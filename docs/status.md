# dx9mt Status

## What This Is

D3D9-to-Metal translation layer for Wine WoW64 on Apple Silicon. Target: Fallout: New Vegas (FNV). Replaces the DXVK/Vulkan/MoltenVK chain with a direct D3D9-to-Metal path.

## Architecture

```
PE32 Frontend (d3d9.dll, i686-mingw32)
  Implements D3D9 COM interfaces
  Captures all API state + draw calls as packets
  Copies VB/IB/decl/constant data to upload arena
  Writes per-frame data to shared memory IPC file
      |
      | memory-mapped file (/tmp/dx9mt_metal_frame.bin, 16MB)
      | per-draw: viewport, scissor, VB bytes, IB bytes,
      |           vertex decl, VS/PS constants
      v
Native Metal Viewer (dx9mt_metal_viewer, ARM64)
  Polls IPC file for new frames
  Parses vertex declarations -> Metal vertex descriptors
  Creates Metal buffers from VB/IB data
  Applies WVP matrix from VS constants c0-c3
  Encodes indexed draw calls per frame
  Renders to standalone NSWindow with CAMetalLayer
```

The PE DLL and Metal viewer are separate processes. The PE DLL runs under Wine (mingw, `_WIN32`), the viewer runs natively (`__APPLE__`). They share a 16MB memory-mapped file for frame data. A native backend dylib (`libdx9mt_unixlib.dylib`) is also built but not yet loaded by Wine -- the IPC approach bypasses the need for Wine unix lib integration.

## Current Verified State (2026-02-07)

- FNV launches through Steam, passes launcher, reaches main menu
- Active frame loop: ~19 draws/frame, 1 clear, consistent packet flow
- Metal viewer shows FNV main menu with geometry in correct screen positions
- WVP matrix extracted from VS constants c0-c3, applied in Metal vertex shader
- Vertex colors visible (D3DCOLOR from vertex declaration)
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
| RB3 Phase 2 | Next | Textures, depth/stencil, blend state, D3D9 shader translation |
| RB4 | Pending | State fidelity: render state, pass structure, clear-pass fusion |
| RB5 | Pending | Performance: state dedup, buffer recycling, pipeline cache, async compile |
| RB6 | Pending | Compatibility hardening: missing stubs, edge cases |

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
    d3d9_device.c          IDirect3DDevice9 + all COM objects (~5000 lines)
    d3d9_perf.c            D3DPERF_* stubs + legacy exports
    dllmain.c              DLL entry point
    runtime.c              Singleton init, backend bridge setup, packet sequencing
    d3d9.def               DLL export definitions
  src/backend/
    backend_bridge_stub.c  Packet parser, frame replay state, IPC writer, Metal wiring
    metal_presenter.h      C-callable Metal presenter API (unused in IPC mode)
    metal_presenter.m      Objective-C Metal implementation (unused in IPC mode)
  src/common/
    log.c                  Timestamped file/stderr logging
  src/tools/
    metal_viewer.m         Standalone native Metal viewer (reads IPC, renders geometry)
  tests/
    backend_bridge_contract_test.c   10 contract tests
```
