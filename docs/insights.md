# Insights

Hard-won lessons, common mistakes, and non-obvious behaviors. Read this before making changes.

## Build System

- `backend_bridge_stub.c` is compiled TWICE: once by `i686-w64-mingw32-gcc` (PE32, into d3d9.dll) and once by `clang` (ARM64, into libdx9mt_unixlib.dylib). Any struct it touches must have identical layout across both ABIs. The `dx9mt_upload_ref` struct has explicit `_pad0` for this reason.
- The viewer (`dx9mt_metal_viewer`) is a completely separate binary — it's NOT linked into the DLL or dylib. It reads IPC shared memory independently.
- Tests compile with `-DDX9MT_NO_METAL` to stub out Metal calls. The backend bridge code has `#ifdef` guards for this.
- Frontend uses `-Wl,--kill-at` to export stdcall functions without name decoration (required for Wine DLL loading).

## Cross-ABI Gotchas

- `dx9mt_packet_draw_indexed` is ~800 bytes and must pack identically between PE32 (i686) and ARM64 (clang). The `_Static_assert` in packets.h catches size overflow but NOT alignment mismatches between compilers. If you add fields, verify layout manually.
- `dx9mt_upload_ref` has a `uint16_t _pad0` between `arena_index` and `offset`. Without it, natural alignment differs between ABIs. Never remove this padding.
- The IPC structures in `metal_ipc.h` are read by a separate native process (the viewer). These structs define a binary wire format — changing them requires updating both the DLL-side writer and the viewer-side reader.

## D3D9 API Behavior

- `Direct3DCreate9()` rejects any `sdk_version != D3D_SDK_VERSION`. This is correct D3D9 behavior — the game must pass the exact version.
- The device caps in `GetDeviceCaps()` are hardcoded to match an NVIDIA GTX 280. This affects what the game thinks is supported (shader model, texture sizes, blend modes, etc.).
- Many probe functions (CheckDeviceType, CheckDeviceFormat, etc.) are called hundreds of times during startup. The `dx9mt_should_log_probe()` rate-limiter prevents log flooding — first N calls logged, then every Mth call.
- `GetAdapterIdentifier()` returns fake NVIDIA vendor/device IDs. FNV uses these to select code paths.

## Upload Arena

- The arena is 3 × 128MB = 384MB VirtualAlloc'd. This is allocated lazily on first use.
- Slot rotation: `slot_index = frame_id % 3`. Frame N writes to slot N%3 while the backend may still be reading slots (N-1)%3 and (N-2)%3.
- If a single frame's uploads exceed 128MB, `dx9mt_frontend_upload_copy()` returns a zero-ref and the draw silently drops data. This is a known limitation — FNV typically uses ~10-20MB per frame.
- Constants are only re-uploaded when dirty OR on the first draw of a frame (because the slot rotated). The dirty flag check `ref.size == 0` catches the first-draw case.

## Texture Management

- `generation` counter on textures increments on every Lock/Unlock cycle. The frontend compares `generation` vs `last_upload_generation` to decide whether to re-upload texture data.
- Even unchanged textures get periodically re-uploaded every 60 frames via `(frame_id + object_id) % 60 == 0`. This handles cases where GPU memory pressure or Wine translation might corrupt texture data.
- Texture `sysmem` is the Lock target — it's a CPU-side copy. The upload arena copy is what the backend/viewer actually reads.
- Only stage 0 is fully supported in the fixed-function TSS combiner path in the viewer. Stages 1-7 have texture data forwarded but the combiner ignores them.

## Shader Translation

- Shader bytecodes are uploaded every draw (not cached in the upload arena). The viewer caches compiled Metal functions by bytecode hash, so the upload cost is acceptable.
- The MSL emitter produces up to 32KB of source per shader. If exceeded, it silently truncates — this hasn't been hit with FNV shaders but could be an issue with modded content.
- Attribute index mapping (POSITION→0, COLOR0→1, TEXCOORD0→2, etc.) must match between the MSL emitter and the viewer's vertex descriptor builder. These are hardcoded in both places.
- `dx9mt_sm_parse()` handles SM 1.0 through 3.0 but some SM 3.0 features (predication, gradient instructions) are parsed but not fully translated to MSL.

## IPC Shared Memory

- The IPC file is 256MB at `/tmp/dx9mt_metal_frame.bin`. The Makefile pre-creates it with `dd` before launching the viewer.
- The sequence number uses `__ATOMIC_RELEASE` on write (backend) and `__ATOMIC_ACQUIRE` on read (viewer) to ensure all frame data is visible before the viewer starts reading.
- Maximum 2048 draws per frame in IPC. Draws beyond this are dropped with a log warning. FNV typically has 200-800 draws per frame.
- The Wine path to the IPC file is `Z:\tmp\dx9mt_metal_frame.bin` (Wine maps Z: to the root filesystem).

## FNV-Specific Patches

- The BSShader factory NULL crash at 0x00B57AA9 is a known FNV bug when `shader_tbl[29]` is empty and TLS slot 0 is uninitialized on IO loading threads. The vectored exception handler in `dllmain.c` patches EIP to skip all vtable calls when ESI=0. There's also a fallback pattern matcher for nearby `mov eax,[esi]` crashes.
- The crash handler dumps extensive diagnostics: register state, code bytes, stack frames, module info, shader table entries, TLS data. This is invaluable for diagnosing new crashes.

## Debugging

- `DX9MT_LOG_PATH` env var controls where logs go (set by Makefile to session dir). Without it, logs go to stderr.
- `DX9MT_TRACE_PROBES=1` enables full logging of all capability probe calls (extremely verbose).
- `DX9MT_BACKEND_TRACE_PACKETS=1` logs every packet received by the backend.
- The viewer supports keyboard-triggered frame dumps ('D' key) that write draw state + raw texture data to the session output directory.
- The replay hash (visible as the overlay bar color in the viewer) changes whenever frame content changes. If the hash is stable, the game is rendering the same content.

## Common Mistakes

- **Adding packet fields without updating both sides**: packets.h is shared, but the viewer reads IPC draws (metal_ipc.h), not packets directly. If you add a render state to the packet, you must also add it to `dx9mt_metal_ipc_draw` AND update the backend's IPC serialization loop AND the viewer's draw processing.
- **Forgetting to mark constants dirty after Present**: Present rotates the upload slot, so previously uploaded constant refs point to stale memory. The device code marks `vs_const_dirty = TRUE` after each Present for this reason.
- **Mismatched attribute indices**: the MSL emitter and the viewer's vertex descriptor builder both hardcode the same attribute index mapping. If you change one, change the other.
- **Ignoring upload ref validation**: the backend validates every upload ref before use. A ref with `size == 0` is treated as "no data" and skipped cleanly. Don't treat zero-refs as errors.
- **Metal encoder state leaking between draws**: Metal has no concept of "disabling" the scissor test — `setScissorRect:` is always active once set. D3D9's `RS_SCISSORTESTENABLE=FALSE` must be translated as "set scissor to full render target", not "skip setting scissor". Any per-draw Metal encoder state that D3D9 treats as toggleable (scissor, viewport, etc.) must be explicitly reset when the D3D9 flag is off, or the previous draw's state leaks through.
