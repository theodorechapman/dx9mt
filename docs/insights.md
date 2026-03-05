# Insights

Hard-won lessons, common mistakes, and non-obvious behaviors. Read this before
changing packet formats, replay code, or shader translation.

## Build System

- `backend_bridge_stub.c` is compiled twice: once by
  `i686-w64-mingw32-gcc` into `d3d9.dll`, and once by `clang` into
  `libdx9mt_unixlib.dylib`. Any shared struct it touches must keep identical
  layout across PE32 and ARM64.
- The viewer (`dx9mt_metal_viewer`) is a completely separate process. It is not
  linked into the DLL or dylib and only sees the world through IPC.
- Tests compile with `-DDX9MT_NO_METAL`. If you add behavior in the shared
  backend path, keep the non-Metal test build working.
- Frontend exports still depend on `-Wl,--kill-at` so Wine loads stdcall
  symbols with undecorated names.

## Cross-ABI And Wire-Format Gotchas

- `dx9mt_upload_ref` keeps explicit padding for ABI stability. Do not "clean it
  up."
- `packets.h` and `metal_ipc.h` define binary contracts. Adding a field means
  updating:
  - the packet writer in the frontend
  - the backend validation and serialization path
  - the viewer-side IPC reader
- The latest commit added `DX9MT_PACKET_STRETCH_RECT` and corresponding IPC draw
  command metadata. That is the model to follow when introducing another replay
  operation.

## D3D9 API Behavior

- `Direct3DCreate9()` must reject any `sdk_version != D3D_SDK_VERSION`.
- The reported caps are intentionally GTX 280-like. Changing them changes how
  FNV configures shaders, formats, and render paths.
- Startup probe calls happen in bulk. Keep probe logging sampled unless you are
  explicitly investigating capability negotiation.
- `GetAdapterIdentifier()` returns NVIDIA-like vendor and device IDs because FNV
  keys behavior off them.

## Upload Arena

- The arena is now `3 x 256 MB = 768 MB`, allocated lazily on first use.
- Slot rotation is still `slot_index = frame_id % 3`. Present rotates the slot,
  so old refs become stale immediately after a successful frame boundary.
- A failed upload copy is still represented as a zero ref. The backend treats
  that as "missing data," not as a special packet type.
- Increasing the arena reduced early in-game breakage, but heavy frames can
  still overflow and later manifest as missing constants, geometry, or textures.

## Texture Management

- Texture `generation` still increments on Lock/Unlock. That remains the primary
  dirtiness signal.
- The frontend now re-uploads more aggressively:
  - generation changed
  - first upload for the texture
  - stale `last_upload_frame_id`
  - at least 8 frames since the last upload
- `upload_size == 0` is not automatically a bug:
  - it can mean "viewer should already have this cached"
  - it becomes a real failure only if the viewer has no cache or RT override
- `dx9mt/texdiag` is now the first place to look for unresolved textures. It
  distinguishes unsupported type, missing metadata, missing level surface, no
  sysmem copy, zero upload size, not-dirty, and upload-copy failure.

## Render Targets And Blits

- Offscreen render targets are now part of the normal path, not just a future
  goal. The viewer materializes them as Metal textures and can later expose
  them through `render_target_texture_id`.
- `D3DFMT_A16B16G16R16F` is critical for FNV. If support for it regresses, the
  project falls back toward the old blank-world symptom.
- `StretchRect` matters. It is not just a utility call; it participates in the
  scene-composite chain and can directly affect whether the final world image is
  visible or corrupted.
- Render-target pixel format must participate in the translated-PSO cache key.
  The same shader pair may need different Metal pipelines for drawable and HDR
  targets.

## Shader Translation

- The viewer now expects valid translated bytecode for most world draws. The old
  hardcoded path is no longer the normal renderer.
- A narrow compatibility fallback still exists for a few known translated-PSO
  failures, but treat it as a stopgap, not a design target.
- Attribute index mapping is still hardcoded on both sides:
  - POSITION/POSITIONT -> `v0`
  - COLOR0 -> `v1`
  - TEXCOORD0 -> `v2`
  - NORMAL -> `v3`
  - and so on
- Recent translator fixes came from real bugs:
  - width-aware source emission is required or `select()`, `cmp`, `mova`, and
    similar ops produce invalid scalar/vector combinations
  - PS 3.0 `[[user(...)]]` semantic names must be unique and meaningful
  - VS output POSITION must map cleanly to `[[position]]`
  - input register widths must follow declaration write masks or later
    components read garbage
- Shader failures are now worth preserving. The emitted artifacts contain the
  parsed shader summary, generated MSL, compiler error text, and raw bytecode.

## IPC And Viewer Snapshot Safety

- The IPC file is still 256 MB at `/tmp/dx9mt_metal_frame.bin`.
- Release/acquire ordering alone was not enough for stable diagnostics while the
  viewer was hashing and replaying a live frame.
- The backend now writes `sequence = 0` before mutating IPC-visible data.
- The viewer now:
  - loads the sequence
  - copies and validates the header
  - snapshots the entire frame payload
  - re-checks the sequence
  - renders from the snapshot instead of the live shared mapping
- If you add new diagnostics that touch IPC bulk offsets, validate ranges
  before dereferencing anything.

## Metal Replay State

- Scissor state must always be explicitly reset. In Metal, "not setting a new
  scissor" does not disable the previous one.
- Blit and overlay passes explicitly force `MTLCullModeNone`. Reusing draw-path
  cull state there can create very confusing fullscreen failures.
- Draw routing now depends on the recorded present target. The viewer no longer
  guesses a drawable fallback if `present_render_target_id` is missing.
- Texture resolution order matters:
  - RT override first
  - cached texture second
  - IPC upload third
  - cache miss last

## FNV-Specific Runtime Patches

- The BSShader factory NULL crash workaround in `dllmain.c` is still required.
- The exception handler remains an important source of crash forensics: register
  state, code bytes, stack frames, module information, shader table entries, and
  TLS data.

## Debugging

- `DX9MT_LOG_PATH` controls where frontend and backend logs go.
- `DX9MT_TRACE_PROBES=1` enables full capability-probe logging.
- `DX9MT_BACKEND_TRACE_PACKETS=1` logs each packet received by the backend.
- The viewer still supports frame dumps with the `D` key.
- The most useful runtime outputs right now are:
  - `dx9mt_runtime.log` for `rttrace` and `texdiag`
  - `dx9mt_viewer.log` for draw skips, texture resolution, RT linkage, and
    cohort summaries
  - `dx9mt_shader_fail_*.txt` and `dx9mt_pso_fail_*.txt` for per-hash failures

## Current Investigation Results

- The project is past the old blank-world stage. The game world is now visible
  in the Metal viewer.
- The biggest remaining issues are correctness bugs, not total visibility
  failure:
  - scene-composite or blit corruption
  - translated shader and PSO mismatches
  - billboard or foliage geometry corruption
  - texture cache misses
  - heavy-frame upload pressure
- A prior diagnostics-only viewer crash came from hashing shader bytecode
  without validating IPC bulk ranges first. That exact mistake has already
  happened once; do not repeat it.

## Common Mistakes

- **Adding packet fields without updating all three hops**: frontend packet,
  backend IPC serialization, and viewer IPC reader must all change together.
- **Forgetting slot rotation semantics**: after `Present()`, stale upload refs
  are invalid even if the underlying object did not change.
- **Breaking semantic mapping parity**: the shader emitter and the viewer's
  vertex descriptor builder must agree exactly on attribute indices and
  interpolant names.
- **Treating `upload_size == 0` as always-bad or always-fine**: it depends on
  whether the viewer already has the texture cached.
- **Dereferencing IPC bulk offsets without validation**: every diagnostics path
  must respect the same range checks as the render path.
- **Letting Metal state leak across draws**: scissor, viewport, depth, cull, and
  target routing all need explicit per-draw resets to match D3D9 semantics.
