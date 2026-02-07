# dx9mt Technical Insights

Hard-won lessons from bringing up the D3D9-to-Metal translation layer.

## Architecture

### PE DLL cannot run Metal code
The d3d9.dll is compiled with `i686-w64-mingw32-gcc` where `__APPLE__` is not defined. All Metal/Cocoa APIs are unavailable. The backend_bridge_stub.c is compiled into BOTH the PE DLL (mingw) and the native dylib (clang) -- Metal code is guarded by `#if defined(__APPLE__) && !defined(DX9MT_NO_METAL)` and stripped from the PE build automatically.

### Shared-memory IPC bridges the PE/native gap
Since the PE DLL can't call Metal, a standalone native Metal viewer process reads frame data from a 16MB memory-mapped file (`/tmp/dx9mt_metal_frame.bin`). The PE DLL writes via Win32 `CreateFileMapping`/`MapViewOfFile` on `Z:\tmp\...` (Wine maps Z: to /). The viewer mmaps the same file with POSIX. They synchronize via an atomic sequence counter with acquire/release semantics.

### The native dylib exists but isn't loaded
`libdx9mt_unixlib.dylib` is built with Metal support but Wine never loads it. The proper solution is Wine's `__wine_unix_call` mechanism, but the IPC approach works without Wine integration. The dylib is kept buildable for future use.

### Upload arena is frontend-only memory
The triple-buffered upload arena (`g_frontend_upload_state.slots[]`) lives in the PE DLL's address space. The backend bridge stub (also in the PE DLL) resolves upload refs to pointers via `dx9mt_frontend_upload_resolve()` and copies data into the IPC bulk region.

## Wine Integration

### Both launcher and game need d3d9=native,builtin
Wine's DLL override must apply to both `FalloutNVLauncher.exe` and `FalloutNV.exe` via per-app registry keys. Global overrides don't work reliably.

### Wine restart required after override changes
Stale wineserver state masks override changes. Always `wineserver --kill` before testing new builds.

### Win32 file paths through Wine
The PE DLL accesses Unix paths via the Z: drive. `CreateFileA("Z:\\tmp\\foo")` maps to `/tmp/foo`. This is the standard Wine path mapping and is used for the IPC shared memory file.

## Packet Protocol

### BEGIN_FRAME is now in the packet stream
Originally `BeginScene` called `begin_frame()` as a direct side-channel. Now it emits a `BEGIN_FRAME` packet through `submit_packets`, making the stream self-describing. The backend parser dispatches `BEGIN_FRAME` packets to the same `begin_frame()` logic.

### Draw packet carries full geometry data
`dx9mt_packet_draw_indexed` includes upload refs for VB data, IB data, vertex declaration, VS constants, and PS constants. The frontend copies all this into the upload arena on every `DrawIndexedPrimitive`. The IPC writer then copies from the arena into the shared memory bulk region.

### Upload arena overflow returns zero-ref, not corrupt data
When a slot runs out of space, `dx9mt_frontend_upload_copy` returns a zero-ref instead of wrapping to offset 0. The backend rejects draws with zero-size constant refs. This prevents silent shader constant corruption.

## Metal Rendering

### WVP matrix extraction works for FNV
D3D9 SM3.0 vertex shaders commonly store the world-view-projection matrix in constants c0-c3. The Metal vertex shader reads these 4 float4s, transposes from D3D9 row-major to Metal column-major, and multiplies the position. FNV's main menu renders correctly with this approach.

### D3D9 vertex declarations map cleanly to Metal vertex descriptors
`D3DVERTEXELEMENT9` (stream, offset, type, usage) translates to `MTLVertexDescriptor` attributes. Type mapping: `D3DDECLTYPE_FLOAT3` -> `MTLVertexFormatFloat3`, `D3DDECLTYPE_D3DCOLOR` -> `MTLVertexFormatUChar4Normalized`, etc. The PSO is cached by vertex stride.

### Metal buffer creation per-draw is wasteful but sufficient for now
Each draw creates `newBufferWithBytes` for VB and IB. At ~19 draws/frame with small buffers this doesn't bottleneck. Optimization path: buffer ring allocator with sub-allocation.

### Standalone NSWindow works alongside Wine
The Metal viewer creates its own `NSWindow` with `CAMetalLayer`. Wine's macOS driver runs its own `NSApplication` event loop. The viewer runs a separate `NSApplication` in its own process, avoiding conflicts. `dispatch_sync(main_queue)` handles window creation from non-main threads.

## Debugging

### Sampled logging prevents log flooding
High-frequency calls (GetDeviceCaps, CheckDeviceFormat, DebugSetMute) use `dx9mt_should_log_method_sample(&counter, first_n, every_n)`. First N calls logged in full, then every Nth call. Backend frames log on frames 0-9 then every 120th.

### Kind-tagged object IDs
Object IDs encode the type: `(kind << 24) | serial`. Kind values: 1=device, 2=swapchain, 3=buffer, 4=texture, 5=surface, 6=VS, 7=PS, 8=state_block, 9=query, 10=vertex_decl. Log line `target=33554433` = `0x02000001` = swapchain serial 1.

### Shader bytecode validation
`dx9mt_shader_dword_count` validates the version token (`0xFFFE` for VS, `0xFFFF` for PS) before scanning for the end marker. Scan limit reduced from 4MB to 256KB (64K DWORDs). Rejects bad input early with a log message.

### Replay hash for frame fingerprinting
Each frame's draw commands are hashed (FNV-1a) into a `replay_hash`. If the hash changes between frames, the draw content changed. If it's stable, the game is rendering the same scene. Visible in logs and in the overlay bar color.
