# dx9mt Architecture

A Direct3D 9 to Metal translation layer for running Fallout: New Vegas on macOS
via Wine. The frontend captures D3D9 state and bulk payloads inside Wine, a
shared backend bridge validates and serializes that state into IPC, and a
native Metal viewer replays the frame.

## High-Level Architecture

```text
FalloutNV.exe (i686 / Wine)
    |
    v
+--------------------------------------------------+
| d3d9.dll (PE32, MinGW)                           |
| "Frontend"                                       |
| - Implements IDirect3D9 and IDirect3DDevice9     |
| - Captures draws, clears, shaders, textures      |
| - Emits packets and upload-arena refs            |
+----------------------+---------------------------+
                       | packets + upload refs
                       v
+--------------------------------------------------+
| backend_bridge_stub.c                            |
| (compiled twice: PE32 in the DLL, ARM64 in the   |
| dylib)                                           |
| - Validates packets                              |
| - Records draw and StretchRect commands          |
| - Assembles frame IPC on Present()               |
+----------------------+---------------------------+
                       | 256MB IPC file
                       v
+--------------------------------------------------+
| dx9mt_metal_viewer (native ARM64 macOS app)      |
| - Snapshots the shared frame                     |
| - Replays draws and StretchRect                  |
| - Translates D3D9 shader bytecode to MSL         |
| - Renders via Metal into an NSWindow             |
+--------------------------------------------------+
```

## Directory Layout

```text
dx9mt/                      # Repository root
|-- Makefile                # Top-level orchestration
|-- README.md               # Project overview and current status
|-- docs/
|   |-- architecture.md     # This file
|   |-- insights.md         # Gotchas and debugging notes
|   `-- rendering_findings.md # Current visual state and bug buckets
|-- assets/
|   `-- images/             # Current and historical screenshots
|-- dx9mt/                  # Source tree
|   |-- Makefile            # Inner build for DLL, dylib, viewer, tests
|   |-- include/dx9mt/      # Shared headers and binary contracts
|   |-- src/
|   |   |-- common/         # Shared logging support
|   |   |-- frontend/       # PE32 D3D9 implementation
|   |   |-- backend/        # Shared bridge + in-process Metal presenter
|   |   `-- tools/          # Shader parser/emitter + standalone viewer
|   `-- tests/              # Native contract tests
|-- dx9mt-output/           # Runtime logs and dump artifacts (gitignored)
|   `-- session-*/
|       |-- dx9mt_runtime.log
|       |-- dx9mt_viewer.log
|       |-- dx9mt_frame_dump*.txt
|       |-- dx9mt_shader_fail_*.txt
|       |-- dx9mt_pso_fail_*.txt
|       `-- dx9mt_tex_*.raw
`-- wineprefix/             # Wine prefix with the game installed (gitignored)
```

## Build System

Two Makefiles matter:

- Root `Makefile`
  - `make run`: build, install the DLL into the Wine prefix, launch FNV, and
    launch the Metal viewer
  - `make test`: run native contract tests
  - `make clear`: stop the viewer and Wine runtime
- `dx9mt/Makefile`
  - Frontend: `i686-w64-mingw32-gcc` -> `build/d3d9.dll`
  - Backend dylib: `clang` -> `build/libdx9mt_unixlib.dylib`
  - Viewer: `clang` -> `build/dx9mt_metal_viewer`
  - Tests: `clang -DDX9MT_NO_METAL` -> `build/backend_bridge_contract_test`

Important detail: `backend_bridge_stub.c` is compiled into both the PE32 DLL
and the ARM64 dylib. It is shared bridge logic, not a shim with two unrelated
implementations.

## Frontend (`dx9mt/src/frontend`)

### Entry Points

- `dllmain.c`
  - runtime init and shutdown
  - logging init
  - vectored exception handler for the FNV BSShader NULL crash
- `d3d9.c`
  - `Direct3DCreate9()`
  - adapter and capability probes
  - `CreateDevice()`
- `d3d9_device.c`
  - full `IDirect3DDevice9` implementation
  - resource lifetime
  - state tracking
  - packet emission

### Device State Model

`dx9mt_device` tracks enough state to snapshot a draw without relying on
incremental deltas:

- render states
- sampler states
- texture-stage states
- render targets and depth-stencil surface
- bound textures
- stream bindings and strides
- vertex declaration, shaders, and FVF
- shader constants
- transforms, viewport, scissor, frame ID

### `DrawIndexedPrimitive()` Flow

Each indexed draw emits a full-state packet plus upload refs for bulk payloads.
The packet captures:

- primitive topology and indices
- render target and depth-stencil IDs
- render target dimensions, format, and linked texture ID
- vertex declaration or FVF-derived layout
- VB and IB upload refs
- VS and PS bytecode upload refs
- VS and PS constant upload refs
- up to 8 texture stages with metadata, sampler state, and optional upload refs
- current render states such as depth, stencil, blend, fog, cull, viewport, and
  scissor

### `StretchRect()` Flow

`StretchRect()` now emits its own packet instead of being treated as a purely
local helper. The packet includes:

- source surface and linked texture IDs
- source dimensions and format
- source rect
- destination surface and linked texture IDs
- destination dimensions and format
- destination rect
- filter mode

This matters because FNV uses blits as part of the scene-composite chain.

### `Present()` Flow

`Present()`:

1. emits a `PRESENT` packet with the current frame ID and present target
2. calls the backend bridge synchronously
3. advances the frame ID
4. rotates the upload-arena slot
5. marks shader constants dirty for the next frame

The frontend also samples render-target lifecycle through `dx9mt/rttrace`, which
is now one of the fastest ways to verify whether the game is actually presenting
the target you think it is.

### Upload Arena

Bulk data is staged through a rotating upload arena:

- 3 slots
- 256 MB per slot
- 768 MB total
- frame N writes to slot `N % 3`

Upload refs are small ABI-stable structs shared between the PE32 frontend and
the ARM64 backend. If a copy fails because a slot is exhausted, the ref is zero
and downstream code must treat it as missing data.

### Texture Upload Strategy

The current policy is:

- upload immediately when the texture generation changes
- upload if there has never been a successful upload
- upload if frame tracking wrapped or regressed
- otherwise refresh if at least 8 frames have passed since the last upload

This is intentionally more aggressive than the earlier "every 60 frames"
behavior because it reduces stale-texture failures during replay.

## Backend Bridge (`dx9mt/src/backend/backend_bridge_stub.c`)

The backend bridge owns packet validation, frame recording, and IPC assembly.

### Packet Types

Current packet types include:

- `BEGIN_FRAME`
- `DRAW_INDEXED`
- `PRESENT`
- `CLEAR`
- `STRETCH_RECT`

### Packet Processing

`dx9mt_backend_bridge_submit_packets()` performs linear packet parsing and
validates:

- packet sizes
- packet sequence monotonicity
- upload refs for geometry, constants, declarations, bytecode, and textures
- present target metadata

Validated draw and blit packets are stored as backend replay commands for the
current frame.

### Frame Snapshot And Replay Hash

At `Present()`, the backend captures a small frame summary:

- frame ID
- packet count
- draw count
- clear count
- last clear color, flags, depth, stencil
- last draw state hash
- replay hash over the stored replay commands

The replay hash is useful both for debugging and for the viewer overlay.

### IPC Assembly

The backend writes:

1. IPC header
2. fixed-size draw-command array
3. packed bulk-data region

The IPC file is 256 MB and lives at `/tmp/dx9mt_metal_frame.bin`.

Important synchronization detail:

- before mutating any IPC-visible data, the backend writes `sequence = 0`
- after the frame is fully assembled, it publishes the real sequence with
  release semantics

That lets the viewer ignore in-progress frames.

## Metal Presenter (`dx9mt/src/backend/metal_presenter.m`)

The backend still has a minimal in-process Metal presenter used for debug
overlay rendering. It is not the main replay path. The standalone viewer is the
authoritative renderer for current bring-up work.

## Metal Viewer (`dx9mt/src/tools/metal_viewer.m`)

The standalone viewer is currently the most important runtime component. It is
about 3900 lines and handles:

- IPC snapshotting
- render-target materialization
- texture resolution
- shader translation and Metal compilation
- draw replay
- `StretchRect` replay
- diagnostics and artifact dumping

### Frame Snapshot

The viewer no longer renders directly from the live shared mapping. For each new
sequence number it:

1. loads the sequence with acquire semantics
2. copies the header
3. validates header bounds and bulk layout
4. copies the full frame into a private snapshot buffer
5. re-checks the sequence
6. renders from the snapshot

This prevents diagnostics and replay from racing a writer mutating the shared
buffer underneath them.

### Replay Command Types

The viewer consumes two IPC command types:

- draw replay
- `StretchRect` replay

`StretchRect` is implemented as a small full-screen style draw using a blit
vertex and fragment pair plus a sampler derived from the D3D9 filter mode.

### Render-Target Routing

Each replay command carries:

- destination render target ID
- linked destination texture ID
- render-target dimensions
- render-target format

The viewer routes a command either to:

- the drawable texture, when the command target matches
  `present_render_target_id`
- or a cached offscreen Metal texture for that RT description

Current important format support includes:

- `A8R8G8B8`
- `X8R8G8B8`
- `A8`
- `R32F`
- `A16B16G16R16F`
- `DXT1`, `DXT3`, `DXT5` for sampled textures

`D3DFMT_A16B16G16R16F` support is what moved the project out of the earlier
blank-world phase.

### RT-To-Texture Linking

When an offscreen target finishes drawing, the viewer records the resulting
Metal texture under `render_target_texture_id`. Later passes can resolve stage
textures from:

1. RT override
2. existing texture cache
3. fresh IPC upload

That is the mechanism that lets a later composite pass sample a previously
rendered offscreen target.

### Shader Translation Pipeline

The viewer parses D3D9 bytecode, emits MSL, compiles Metal functions, and caches
the results by bytecode hash.

Current translator features and expectations:

- shader model 1.x through 3.0 bytecode parsing
- width-aware source expression emission
- semantic-aware `[[user(...)]]` naming
- vertex POSITION output mapped to `[[position]]`
- translated PSOs keyed by shader hashes, declaration layout, blend state, and
  target pixel format

The viewer now treats translated bytecode as the main path. If required bytecode
is missing or invalid, the draw is skipped. There is still a narrow compatibility
fallback for a few known hashes, but it is intentionally limited.

### State Translation

For each draw, the viewer sets:

- viewport
- scissor, with explicit full-target reset when D3D9 scissor is disabled
- depth and stencil state
- blend state through the PSO
- cull mode
- vertex and fragment constants
- stage textures and samplers

Blit and overlay passes explicitly force `MTLCullModeNone`.

### Caches

Current viewer caches include:

- texture cache
- texture generation table
- RT override table
- render-target cache
- depth texture cache
- sampler cache
- shader-function caches for VS and PS
- translated PSO cache
- blit PSO cache

### Diagnostics And Artifacts

The current investigation relies on three output groups:

- `dx9mt_runtime.log`
  - frontend and backend logging
  - `rttrace`
  - `texdiag`
- `dx9mt_viewer.log`
  - draw skips
  - unsupported or missing target logs
  - texture resolution logs
  - RT-link establishment logs
  - per-frame diagnostics totals
  - cohort summaries grouped by RT, shader hashes, and texture-stage mask
- artifacts
  - `dx9mt_shader_fail_vs_<hash>.txt`
  - `dx9mt_shader_fail_ps_<hash>.txt`
  - `dx9mt_pso_fail_<key>.txt`
  - `dx9mt_frame_dump*.txt`

Each shader artifact includes the parsed shader summary, generated MSL, compile
error text, and raw bytecode. Each PSO artifact includes the declaration summary
and the VS/PS interface context used during pipeline creation.

### Frame Dumps

The viewer still supports keyboard-triggered dumps with the `D` key. Dumps now
include enough information to reason about both translated draws and
`StretchRect`-driven composite steps.

## Shared Headers (`dx9mt/include/dx9mt`)

| Header | Purpose |
|--------|---------|
| `packets.h` | Frontend packet structs including `DRAW_INDEXED`, `CLEAR`, `PRESENT`, and `STRETCH_RECT` |
| `upload_arena.h` | Upload-ref layout and upload-arena sizing constants |
| `metal_ipc.h` | IPC wire format for header and replay commands |
| `backend_bridge.h` | Shared backend bridge API |
| `object_ids.h` | Object kind encoding for stable IDs |
| `runtime.h` | Frontend runtime init and packet sequence control |
| `log.h` | Shared logging API |

## Tests

`backend_bridge_contract_test.c` validates the shared bridge contract, including:

- valid packet stream acceptance
- bad size rejection
- non-monotonic sequence rejection
- missing target metadata rejection
- draw overflow handling
- replay-hash sensitivity

Run:

- `make test`
- `make -C dx9mt test-native`

## Runtime Flow Summary

1. FNV loads `d3d9.dll`.
2. `DllMain` initializes logging, runtime, and the crash handler.
3. `Direct3DCreate9()` and `CreateDevice()` initialize the frontend device and
   backend bridge.
4. During gameplay:
   - `Clear()` emits a clear packet
   - indexed draws emit full-state draw packets
   - `StretchRect()` emits explicit blit packets
   - `Present()` triggers IPC assembly
5. The viewer snapshots the latest frame and replays it into Metal.
6. The frame ID advances, the upload slot rotates, and constants are marked
   dirty for the next frame.

## Current Rendering State

As of March 5, 2026:

- main menu rendering is stable
- in-game 3D world rendering is now visible
- HUD, weapon, and broad scene structure survive replay
- large planar corruption, billboard or foliage spikes, and remaining composite
  mistakes still prevent visually correct output

For the current evidence and active bug buckets, see
[`docs/rendering_findings.md`](rendering_findings.md).
