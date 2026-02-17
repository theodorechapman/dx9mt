# dx9mt Architecture And Navigation

This document is for fast codebase navigation. It points to the exact files and call paths used by the active D3D9-to-Metal pipeline.

## 1) System Overview

```text
Wine process (PE32 d3d9.dll)
  -> captures D3D9 state + draws
  -> serializes packets + upload refs
  -> backend bridge validates + records frame
  -> copies per-draw data into /tmp/dx9mt_metal_frame.bin (256 MB)

Native process (ARM64 dx9mt_metal_viewer)
  -> polls IPC sequence
  -> rebuilds Metal state from draw entries
  -> translates D3D9 shader bytecode to MSL when available
  -> renders to CAMetalLayer window
```

## 2) Source Map By Responsibility

### Frontend (Wine / PE32)

- `dx9mt/src/frontend/d3d9.c`
  - `Direct3DCreate9`, adapter identity, `GetDeviceCaps`, format checks.
- `dx9mt/src/frontend/d3d9_device.c`
  - Main `IDirect3DDevice9` implementation.
  - Captures render/sampler/texture/shader state.
  - Emits `BEGIN_FRAME`, `DRAW_INDEXED`, `CLEAR`, `PRESENT` packets.
  - Owns upload arena (`3 x 128 MB` slots) and upload refs.
- `dx9mt/src/frontend/runtime.c`
  - Runtime init, backend bridge init, packet sequence source.
- `dx9mt/src/frontend/dllmain.c`
  - DLL lifecycle + VEH crash handler/patch path.

### Backend Bridge (shared C core)

- `dx9mt/src/backend/backend_bridge_stub.c`
  - Packet parser and validation.
  - Frame replay capture (up to 8192 draw commands).
  - IPC writer to `dx9mt_metal_frame.bin` (`DX9MT_METAL_IPC_MAX_DRAWS = 2048`).
  - Present-time logging and replay hashing.
- `dx9mt/include/dx9mt/packets.h`
  - Packet wire structs (draw state, textures, shader bytecode, fog/stencil/etc).
- `dx9mt/include/dx9mt/metal_ipc.h`
  - Shared-memory layout used by viewer.

### Native Metal Viewer

- `dx9mt/src/tools/metal_viewer.m`
  - Poll loop (`pollAndRender`).
  - RT routing and render-pass assembly.
  - Depth/stencil/cull/scissor/blend setup.
  - Fallback shader path + translated shader path.
  - Frame dump tooling (`D`/`F` keys).

### Shader Translation Toolchain

- `dx9mt/src/tools/d3d9_shader_parse.c`
  - Parses SM2/SM3 bytecode to IR.
  - Handles flow-control opcodes and relative addressing tokens.
- `dx9mt/src/tools/d3d9_shader_emit_msl.c`
  - Emits MSL source from parsed IR.
  - Supports `a0`-relative constant indexing, multi-sampler bindings, flow control.

### Logging And Tests

- `dx9mt/src/common/log.c`: runtime log sink (`DX9MT_LOG_PATH`).
- `dx9mt/tests/backend_bridge_contract_test.c`: backend parser/present contract tests.

## 3) Hot Call Path (Frame Lifecycle)

1. `IDirect3DDevice9::BeginScene`
   - `dx9mt_device_BeginScene` emits `DX9MT_PACKET_BEGIN_FRAME`.
2. Draw calls
   - `dx9mt_device_DrawIndexedPrimitive` fills `dx9mt_packet_draw_indexed`:
     - VB/IB bytes, declaration/FVF data.
     - VS/PS constants + shader bytecode.
     - texture/sampler arrays (stages 0..7).
     - depth/stencil/fog/cull/blend/scissor state.
3. `IDirect3DDevice9::Clear`
   - `dx9mt_device_Clear` emits `DX9MT_PACKET_CLEAR`.
4. `IDirect3DDevice9::Present`
   - emits `DX9MT_PACKET_PRESENT` then calls `dx9mt_backend_bridge_present`.
5. Backend present
   - validates frame/present coherence.
   - writes IPC header + draw array + bulk region.
   - atomically bumps IPC sequence.
6. Viewer render
   - sees new sequence, reads header/draws/bulk.
   - routes draws by `render_target_id` + `present_render_target_id`.
   - compiles/caches translated shaders as needed.
   - submits Metal command buffer.

## 4) Symptom -> File Lookup

### In-game viewport is flat blue, HUD still updates

- `dx9mt/src/frontend/d3d9_device.c`
  - upload arena overflow handling (`dx9mt_frontend_upload_copy`).
- `dx9mt/src/backend/backend_bridge_stub.c`
  - "draw packet missing constants_* payload" rejection path.
  - IPC packing bounds checks.
- `dx9mt/src/tools/metal_viewer.m`
  - draw skip conditions (`expects_texture && !draw_texture`, invalid bytecode gate).
  - render-target routing and clear behavior.

### Cursor disappears except while hovering menu items

- `dx9mt/src/frontend/d3d9_device.c`
  - cursor methods are not custom-overridden and fall back to default stubs.
  - check default stub logs (`dx9mt/STUB`) for cursor API calls.

### Save thumbnail or UI copy path issues

- `dx9mt/src/frontend/d3d9_device.c`
  - `GetRenderTargetData`, `StretchRect`, `UpdateSurface`, `UpdateTexture`.
  - surface copy logic (`dx9mt_surface_copy_rect`).

### Hitching/choppy menu transitions

- `dx9mt/src/tools/metal_viewer.m`
  - per-draw `newBufferWithBytes` allocations.
  - on-demand shader/PSO compilation.

## 5) Data Contracts To Respect

- Packet sequence must be strictly monotonic (`runtime.c` -> `backend_bridge_stub.c`).
- `dx9mt_upload_ref` ABI layout is cross-compiler sensitive (`upload_arena.h` padding).
- Upload overflow intentionally returns zero-ref, and backend rejects required missing refs.
- IPC consumer uses `sequence` as release/acquire synchronization boundary.

## 6) Runtime Controls

- `DX9MT_LOG_PATH`: frontend/backend log file.
- `DX9MT_BACKEND_TRACE_PACKETS`: log every parsed packet.
- `DX9MT_BACKEND_SOFT_PRESENT`: Win32 GDI soft-present visualization.
- `DX9MT_BACKEND_METAL_PRESENT`: native backend Metal-present toggle (non-IPC path).
- `DX9MT_FRONTEND_SOFT_PRESENT`: frontend side software present.
- `DX9MT_TRACE_PROBES`: unsampled capability/probe logging in `d3d9.c`.

## 7) Fast Debug Checklist

1. Confirm frame progression:
   - `rg "present frame=" dx9mt-output/dx9mt_runtime.log | tail`
2. Check for data loss:
   - `rg "slot overflow|draw packet missing constants" dx9mt-output/dx9mt_runtime.log`
3. Dump a problematic frame:
   - press `D` in viewer, inspect `dx9mt-output/dx9mt_frame_dump.txt`.
4. Verify active RT and draw count:
   - frame dump header (`draws`, `present_rt`) and per-draw `rt_id`.
5. Check translated shader failures:
   - viewer stderr lines `compile failed` / `emit failed` / `parse failed`.
