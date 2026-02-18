# dx9mt Architecture

A DirectX 9 to Metal translation layer for running Fallout: New Vegas on macOS via Wine. Intercepts D3D9 API calls, captures all rendering state into packets, and replays them through Apple Metal for native GPU rendering.

## High-Level Architecture

```
FalloutNV.exe (i686/Wine)
    │
    ▼
┌─────────────────────────────────────────────┐
│  d3d9.dll  (PE32, cross-compiled MinGW)     │
│  "Frontend" — implements IDirect3D9 +       │
│  IDirect3DDevice9 COM interfaces            │
│                                             │
│  Captures all D3D9 state into packets,      │
│  uploads geometry/textures/constants to a   │
│  triple-buffered upload arena               │
└──────────────┬──────────────────────────────┘
               │ packets + upload refs
               ▼
┌─────────────────────────────────────────────┐
│  backend_bridge_stub.c  (compiled twice:    │
│  once as PE32 for the DLL, once as ARM64    │
│  for libdx9mt_unixlib.dylib)               │
│                                             │
│  Receives packets, validates, records draw  │
│  commands, assembles frame data into IPC    │
│  shared memory on present()                 │
└──────────────┬──────────────────────────────┘
               │ 256MB mmap'd file (/tmp/dx9mt_metal_frame.bin)
               ▼
┌─────────────────────────────────────────────┐
│  dx9mt_metal_viewer  (native ARM64 macOS)   │
│                                             │
│  Polls IPC sequence number, reads frame     │
│  draws, translates D3D9 shaders to MSL,     │
│  renders via Metal into a native NSWindow   │
└─────────────────────────────────────────────┘
```

## Directory Layout

```
dx9mt/                      # Root
├── Makefile                # Top-level: build, install DLL into Wine prefix, run FNV
├── CLAUDE_soon.md          # Will become CLAUDE.md — workflow instructions
├── docs/
│   ├── architecture.md     # This file
│   └── insights.md         # Lessons learned, common mistakes
├── dx9mt/                  # All source code
│   ├── Makefile            # Inner build: d3d9.dll + libdx9mt_unixlib.dylib + viewer + tests
│   ├── include/dx9mt/      # Shared headers (frontend ↔ backend contract)
│   ├── src/
│   │   ├── common/         # Shared code (logging)
│   │   ├── frontend/       # PE32 DLL sources (D3D9 API implementation)
│   │   ├── backend/        # Bridge + Metal presenter
│   │   └── tools/          # Shader parser, MSL emitter, Metal viewer
│   ├── tests/              # Contract tests
│   └── build/              # Build artifacts (gitignored)
├── dx9mt-output/           # Runtime logs + frame dumps (gitignored)
│   └── session-*/
│       ├── dx9mt_runtime.log
│       ├── dx9mt_frame_dump_NNNN.txt
│       └── dx9mt_tex_*.raw
└── wineprefix/             # Wine prefix with FNV installed (gitignored)
```

## Build System

Two Makefiles:

**Root Makefile** — orchestration:
- `make run` (default): builds DLL → installs into Wine prefix → sets DLL override → launches FNV via NVSE, plus starts the Metal viewer
- `make test`: runs native contract tests
- `make clear`: kill viewer + wineserver

**dx9mt/Makefile** — compilation:
- Frontend: `i686-w64-mingw32-gcc` → `build/d3d9.dll` (PE32, links log.c + backend_bridge_stub.c + all frontend sources)
- Backend: `clang` → `build/libdx9mt_unixlib.dylib` (ARM64, links log.c + backend_bridge_stub.c + metal_presenter.m)
- Viewer: `clang` → `build/dx9mt_metal_viewer` (ARM64 Obj-C app, links metal_viewer.m + shader parser/emitter)
- Tests: `clang -DDX9MT_NO_METAL` → `build/backend_bridge_contract_test`

Note: `backend_bridge_stub.c` is compiled into BOTH the frontend DLL and the backend dylib. This is the bridge — same code, different compilers/ABIs.

## Frontend (d3d9.dll)

### Entry Points

`dllmain.c` — DLL lifecycle:
- `DLL_PROCESS_ATTACH`: init logging, register vectored exception handler (crash handler with BSShader NULL-return patch for FNV-specific crash)
- `DLL_PROCESS_DETACH`: shutdown runtime

`d3d9.c` — IDirect3D9 implementation:
- `Direct3DCreate9()`: creates `dx9mt_d3d9` object, returns vtable
- Implements capability queries (GetDeviceCaps, CheckDeviceFormat, etc.) with hardcoded GTX 280–like caps
- `CreateDevice()` → delegates to `dx9mt_device_create()`

`d3d9_perf.c` — stub exports (D3DPERF_*, DebugSetMute, etc.) required by the DLL export table (`d3d9.def`)

`d3d9_device_methods.inc` — X-macro listing all 119 IDirect3DDevice9 vtable methods

### Device Implementation (d3d9_device.c) — ~5500 lines

The core of the frontend. Implements every IDirect3DDevice9 method.

**Resource types** (each has COM vtable, refcount, object_id, device backpointer):

| Type | Struct | Key Fields |
|------|--------|------------|
| Surface | `dx9mt_surface` | desc, lockable, sysmem buffer, pitch |
| Texture | `dx9mt_texture` | width/height/format/levels, generation counter, surface array |
| Vertex Buffer | `dx9mt_vertex_buffer` | desc, HeapAlloc'd data |
| Index Buffer | `dx9mt_index_buffer` | desc, HeapAlloc'd data |
| Vertex Decl | `dx9mt_vertex_decl` | D3DVERTEXELEMENT9 array, count |
| Vertex Shader | `dx9mt_vertex_shader` | bytecode (DWORD*), dword_count |
| Pixel Shader | `dx9mt_pixel_shader` | bytecode (DWORD*), dword_count |
| Swap Chain | `dx9mt_swapchain` | params, backbuffer surface, present_count |
| Query | `dx9mt_query` | type, data_size, issue_flags |

**Device state tracking** (`dx9mt_device` struct):
- `render_states[256]` — indexed by D3DRENDERSTATETYPE
- `sampler_states[20][16]` — per-sampler state
- `tex_stage_states[16][32]` — per-stage texture stage state
- `render_targets[4]` + `depth_stencil` — MRT support
- `textures[16]` — bound texture per stage
- `streams[16]` + offsets/strides + `indices` — input assembly
- `vertex_decl`, `vertex_shader`, `pixel_shader` — pipeline state
- `vs_const_f[256][4]`, `ps_const_f[256][4]` — float shader constants (+ int/bool)
- `vs_const_dirty`, `ps_const_dirty` — dirty flags to avoid redundant uploads
- `transforms[512]` — world/view/projection matrices
- `viewport`, `scissor_rect`, `fvf`, `frame_id`

**Object ID allocation**: atomic per-kind counters, format `(kind << 24) | serial`. Kinds defined in `object_ids.h`.

### DrawIndexedPrimitive Flow

1. Build `dx9mt_packet_draw_indexed` (~800 bytes)
2. Snapshot all current device state into the packet:
   - Render target/depth-stencil IDs, dimensions, format
   - Buffer/shader/decl IDs, FVF, stream config
   - FNV-1a hashes of viewport, scissor, texture stages, samplers, streams
   - Upload shader constants to arena (if dirty)
   - Upload shader bytecodes to arena
   - Upload VB/IB data to arena (full buffer each draw)
   - Upload vertex declaration elements to arena
   - Fill 8 texture stages: IDs, metadata, sampler states, pixel data
   - Copy all render states (blend, alpha test, z/stencil, cull, scissor, fog)
3. Compute state_block_hash (FNV-1a over entire packet)
4. Submit packet to backend bridge

### Present Flow

1. Build `dx9mt_packet_present` with frame_id + render_target_id
2. Submit packet
3. Call `dx9mt_backend_bridge_present(frame_id)` — synchronous
4. Soft-present (Win32 GDI blit to window)
5. Increment `frame_id`, rotate upload arena slot, mark constants dirty

### Upload Arena

Triple-buffered memory region for passing bulk data (VB, IB, textures, constants, bytecode) from frontend to backend.

- 3 slots × 128MB each, allocated via VirtualAlloc
- Slot rotates each frame: `slot_index = frame_id % 3`
- `dx9mt_frontend_upload_copy()`: memcpy data, return `dx9mt_upload_ref {arena_index, offset, size}`
- Refs are 12-byte structs with explicit padding for cross-ABI compatibility (PE32 ↔ ARM64)
- Backend resolves refs via `dx9mt_frontend_upload_resolve()`

### Texture Upload Strategy

- Each texture has a `generation` counter (incremented on Lock/Unlock)
- Upload occurs when: generation changed OR periodic refresh every 60 frames
- Refresh is deterministic: `(frame_id + object_id) % 60 == 0`

## Backend Bridge (backend_bridge_stub.c) — ~1400 lines

Compiled into both DLL and dylib. Receives packets, records draw commands, writes IPC.

### Packet Processing

`dx9mt_backend_bridge_submit_packets()`:
- Linear parse of packet buffer
- Validates: size bounds, sequence monotonicity, state IDs, upload refs
- Per packet type:
  - `DRAW_INDEXED`: validates state, records into `dx9mt_backend_draw_command` array (up to 8192 per frame)
  - `CLEAR`: stores clear color/flags/z/stencil
  - `BEGIN_FRAME`: resets frame counters
  - `PRESENT`: stores frame_id + render_target_id

### Present (IPC Assembly)

`dx9mt_backend_bridge_present()`:
1. Capture frame snapshot (stats, hashes)
2. If Metal available (macOS): call `dx9mt_metal_present()` for overlay rendering
3. If soft-present enabled (Win32): draw debug visualization via GDI
4. If IPC mapped: serialize all draw commands into shared memory:
   - Write header (magic, dimensions, clear state, draw count, replay hash)
   - Write `dx9mt_metal_ipc_draw` array
   - Pack bulk data region (VB, IB, decl, constants, textures, bytecodes) with 16-byte alignment
   - Atomic store sequence number with release semantics (viewer polls with acquire)

### Frame Replay Hash

FNV-1a hash over all draw commands' hashed state. Used by viewer to detect frame changes and for debugging.

### Environment Variables
- `DX9MT_BACKEND_TRACE_PACKETS`: log every packet
- `DX9MT_BACKEND_SOFT_PRESENT`: enable Win32 debug window
- `DX9MT_BACKEND_METAL_PRESENT`: enable/disable direct Metal (default on)

## Metal Presenter (metal_presenter.m)

Minimal Metal rendering surface used by the backend dylib directly. Creates NSWindow + CAMetalLayer, renders clear color + draw-count overlay bar. This is the "in-process" Metal path, separate from the standalone viewer.

## Metal Viewer (metal_viewer.m) — ~2800 lines

Standalone native macOS app that reads IPC shared memory and renders D3D9 frames via Metal.

### Rendering Pipeline

1. **Poll**: atomic-acquire read of IPC sequence number
2. **Per draw** (up to 2048):
   - Load VB/IB from bulk data
   - Build Metal vertex descriptor from D3D9 vertex declaration
   - Route to correct render target (drawable or offscreen texture)
   - Set viewport, scissor, cull, depth/stencil, blend state
   - Select rendering path:
     - **Hardcoded shaders**: embedded geo_vertex/geo_fragment with TSS combiner (stage 0 only)
     - **Translated shaders**: parse D3D9 bytecode → MSL → compile → cache by hash
3. **Present**: commit command buffer

### Caches

- Texture cache: keyed by (id, generation, format, width, height)
- Sampler cache: keyed by (min/mag/mip filter, address modes)
- Render target cache: keyed by (id, width, height, format)
- Depth/stencil state cache: keyed by state bits
- Shader function cache: keyed by bytecode hash
- PSO cache: keyed by (VS hash, PS hash, vertex descriptor, pixel format, blend/depth state)

### Frame Dumps

Keyboard-triggered ('D' key): writes frame_dump txt + raw texture files to session output directory.

## Shader Translation Pipeline

### Parser (d3d9_shader_parse.c)

Parses D3D9 shader model 1.0–3.0 bytecode into `dx9mt_sm_program` IR:
- Version token → shader type (VS/PS) + version
- Instructions (up to 512): opcode, dst register, up to 4 source registers
- Declarations: input/output semantics, sampler types
- Inline constants: DEF/DEFI/DEFB
- Register usage analysis: max temp/const, sampler/input/output masks

Supported: 43 opcodes (arithmetic, matrix, texture, flow control).

### MSL Emitter (d3d9_shader_emit_msl.c)

Translates parsed program to Metal Shading Language source (up to 32KB):
- Generates input/output structs with `[[attribute(N)]]` / `[[user(...)]]` annotations
- Maps D3D9 registers to MSL variables (r# → float4 locals, c[] → constant buffer, v# → input, o# → output)
- Handles swizzle, write mask, source modifiers (negate, abs, bias, x2, complement), saturate
- Special instruction handling: LIT (lighting calc), NRM (normalize), matrix ops (unrolled dots), TEXLD/TEXKILL
- Attribute index mapping: POSITION→0, COLOR0→1, TEXCOORD0→2, NORMAL→3, TEXCOORD1→4, COLOR1→5, etc.

## Shared Headers (include/dx9mt/)

| Header | Purpose |
|--------|---------|
| `packets.h` | Packet structs: init, begin_frame, draw_indexed, present, clear. Static asserts on size. |
| `upload_arena.h` | Upload ref struct + arena descriptor. Triple-buffer constants (3 slots, 128MB each). |
| `metal_ipc.h` | IPC layout: header + draw array + bulk data. Magic 0xDEAD9001, 256MB region, max 2048 draws. |
| `object_ids.h` | Object kind enum (device, swapchain, buffer, texture, surface, shaders, state_block, query, vertex_decl). |
| `backend_bridge.h` | Bridge API: init, update_present_target, submit_packets, begin_frame, present, shutdown. |
| `d3d9_device.h` | `dx9mt_device_create()` prototype. |
| `runtime.h` | Runtime init/shutdown + packet sequence counter. |
| `log.h` | `dx9mt_logf(tag, fmt, ...)` — writes to DX9MT_LOG_PATH or stderr. |

## Tests

`backend_bridge_contract_test.c` — 10 test cases validating the backend contract:
- Valid packet stream acceptance
- Truncated/wrong-size packet rejection
- Non-monotonic sequence rejection
- Missing state ID rejection
- Present without target metadata rejection
- Frame ID mismatch rejection
- Draw capture overflow handling (8256 draws)
- Replay hash sensitivity to draw payload changes
- BEGIN_FRAME via packet stream

Run: `make test` (from root) or `make test-native` (from dx9mt/).

## Runtime Flow Summary

1. FNV loads `d3d9.dll` → `DllMain` inits logging + crash handler
2. Game calls `Direct3DCreate9()` → runtime init → backend bridge init → IPC file open
3. Game calls `CreateDevice()` → device state initialized, present target registered
4. Per frame:
   - `BeginScene()` / `EndScene()` — tracked but no-op
   - `Clear()` → CLEAR packet
   - `SetRenderState/Texture/Shader/etc.` → state stored in device arrays
   - `DrawIndexedPrimitive()` → full state snapshot → upload arena → DRAW_INDEXED packet
   - `Present()` → PRESENT packet → backend assembles IPC → viewer renders via Metal
5. Frame ID increments, upload slot rotates, constants marked dirty
6. `DLL_PROCESS_DETACH` → shutdown

## Key Design Decisions

- **Full-state packets**: each draw carries ALL state (no incremental deltas). Simplifies backend at cost of bandwidth.
- **Upload arena triple-buffering**: prevents frontend/backend data races without locks.
- **FNV-1a hashing**: fast content-addressable state deduplication (PSO cache, replay hash).
- **Cross-ABI structs**: explicit padding in upload_ref for PE32 ↔ ARM64 layout agreement.
- **Generation tracking**: textures only re-uploaded when modified (or periodic refresh every 60 frames).
- **Crash patching**: vectored exception handler catches BSShader factory NULL in FNV, patches EIP to skip vtable calls.
- **Hardcoded + translated shader paths**: hardcoded shaders for fallback; bytecode→MSL translation for full fidelity.
