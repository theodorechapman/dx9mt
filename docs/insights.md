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

### Draw packet carries full geometry data + shader bytecode
`dx9mt_packet_draw_indexed` includes upload refs for VB data, IB data, vertex declaration, VS constants, PS constants, texture data, and VS/PS shader bytecode. The frontend copies all this into the upload arena on every `DrawIndexedPrimitive`. The IPC writer then copies from the arena into the shared memory bulk region. Shader bytecode is small (1-10KB per shader) and many draws reuse the same shader, so the same bytecode is uploaded redundantly per-draw but the viewer deduplicates by hash.

### Upload arena overflow returns zero-ref, not corrupt data
When a slot runs out of space, `dx9mt_frontend_upload_copy` returns a zero-ref instead of wrapping to offset 0. The backend rejects draws with zero-size constant refs. This prevents silent shader constant corruption.

## Metal Rendering

### WVP matrix extraction works for FNV menu
D3D9 SM3.0 vertex shaders commonly store the world-view-projection matrix in constants c0-c3. The Metal vertex shader reads these 4 float4s, transposes from D3D9 row-major to Metal column-major, and multiplies the position. FNV's main menu renders correctly with this approach. This is a temporary approximation until proper shader translation.

### D3D9 vertex declarations map cleanly to Metal vertex descriptors
`D3DVERTEXELEMENT9` (stream, offset, type, usage) translates to `MTLVertexDescriptor` attributes. Type mapping: `D3DDECLTYPE_FLOAT3` -> `MTLVertexFormatFloat3`, `D3DDECLTYPE_D3DCOLOR` -> `MTLVertexFormatUChar4Normalized`, etc. The PSO is cached by (vertex layout + blend state) hash.

### FVF-to-vertex-declaration conversion
Games using `SetFVF` instead of `SetVertexDeclaration` get their FVF bitmask converted to an equivalent `D3DVERTEXELEMENT9` array in the frontend's `DrawIndexedPrimitive`. This handles position type variants (XYZ, XYZRHW, XYZW, XYZBn), normals, diffuse/specular colors, and texture coordinates with per-coordinate format bits.

### Standalone NSWindow works alongside Wine
The Metal viewer creates its own `NSWindow` with `CAMetalLayer`. Wine's macOS driver runs its own `NSApplication` event loop. The viewer runs a separate `NSApplication` in its own process, avoiding conflicts. `dispatch_sync(main_queue)` handles window creation from non-main threads.

## Texture Pipeline

### Texture caching with generation tracking
Each `dx9mt_texture` has a `generation` counter incremented on Lock/Unlock, AddDirtyRect, and surface copy operations. The frontend only uploads texture data when the generation changes (or on a periodic 60-frame refresh for cache recovery). The viewer caches `MTLTexture` objects by `(object_id, generation)` and skips re-creation when the generation matches.

### DXT compressed texture support
D3D9 DXT1/DXT3/DXT5 formats map directly to Metal's BC1_RGBA/BC2_RGBA/BC3_RGBA. Block-compressed pitch is calculated as `((width + 3) / 4) * block_bytes` where block_bytes is 8 for DXT1 and 16 for DXT3/DXT5. The frontend computes this correctly for both surface allocation and upload sizing.

### Render-target texture routing
Draws can target offscreen render targets (not just the swapchain). The viewer tracks per-draw `render_target_id` and creates separate `MTLTexture` objects for each RT. When a later draw samples a texture whose `texture_id` matches a previous RT's `render_target_texture_id`, the viewer substitutes the RT's Metal texture. This enables render-to-texture effects (e.g., UI compositing).

## Shader Translation (RB3 Phase 3)

### D3D9 SM2.0/SM3.0 bytecode format
The bytecode is a stream of 32-bit DWORD tokens. Token 0 is the version (`0xFFFE03xx` for VS 3.x, `0xFFFF03xx` for PS 3.x). Each instruction is an opcode token followed by destination and source register tokens. The stream ends with `0x0000FFFF`. Register tokens encode type (5 bits split across bits [30:28] and [12:11]), number (bits [10:0]), and for sources: swizzle (bits [23:16], 2 bits per component) and modifier (bits [27:24]). For destinations: write mask (bits [19:16]) and result modifier (bits [23:20]).

### Transpiler architecture: parse → IR → MSL source → compile → cache
The bytecode is parsed into a flat IR (`dx9mt_sm_program` with instruction/dcl/def arrays), then emitted as MSL source text, compiled with `[MTLDevice newLibraryWithSource:]`, and the resulting `MTLFunction` cached by bytecode hash. Compilation happens once per unique shader; subsequent draws just look up the cache. Failures are sticky (cached as NSNull) to avoid retrying every frame.

### VS/PS interface uses a "fat" interpolant struct
The emitted VS output struct and PS input struct must have matching field names and types. Rather than dynamically matching VS output semantics to PS input semantics (complex), the emitter generates per-shader structs based on dcl declarations. The VS writes its declared outputs; the PS reads its declared inputs. As long as the same VS/PS pair is used together, the structs match.

### Register mapping is straightforward for SM3.0
- `r#` → `float4 r#` (local variable, initialized to 0)
- `v#` → `in.v#` (from `[[stage_in]]` struct)
- `c#` → `c[#]` (constant buffer at buffer index 1 for VS, 0 for PS)
- `s#` + `texld` → `tex#.sample(samp#, coord.xy)` (texture + sampler pair)
- `oPos` (rastout 0) → `out.position` with `[[position]]`
- `oC0` → return value of fragment function

### D3D9 swizzle maps directly to MSL component access
D3D9 swizzle encodes 4 component indices (0=x, 1=y, 2=z, 3=w). Identity swizzle `.xyzw` is omitted. Replicate swizzle `.xxxx` emits `.x`. The emitter handles all modifiers: negate `(-expr)`, abs `abs(expr)`, complement `(1.0 - expr)`, x2 `(expr * 2.0)`, bias `(expr - 0.5)`.

### POSITIONT draws skip translation entirely
Pre-transformed vertices (D3DFVF_XYZRHW / `dcl_positiont`) are already in screen space. The existing hardcoded `geo_vertex` shader with a synthetic screen-to-NDC matrix handles these correctly. When the vertex declaration has POSITIONT, the viewer uses the fallback path and only the PS could potentially be translated (currently skipped -- both VS and PS are needed for the translated path).

## Fragment Shader Strategy

### Four-tier fragment shader approach (with translation)
The Metal viewer now has four tiers for fragment shading, tried in order:

1. **Translated shader** (when bytecode is available and compiles): Full D3D9 shader translated to MSL. Reads from constant buffer, samples textures, executes all arithmetic. Uses the full VS/PS constant arrays at buffer indices 1 and 0 respectively.

2. **TSS fixed-function combiner** (`use_stage0_combiner=1`, when `pixel_shader_id==0`): Full D3D9 texture stage state evaluation. Supports all D3DTOP operations (MODULATE, SELECTARG, ADD, BLEND*, etc.) with configurable arg sources (TEXTURE, CURRENT, DIFFUSE, TFACTOR).

3. **PS constant c0 tint** (`has_pixel_shader=1`, translation failed): When a D3D9 pixel shader is active but translation failed, the viewer reads PS constant register c0 and multiplies it by the texture sample. This approximation works for FNV's menu shaders.

4. **Raw passthrough** (no PS, no TSS): `output = diffuse * texture` (textured) or `output = diffuse` (non-textured).

### D3D9 TSS state is irrelevant when a pixel shader is active
When a pixel shader is bound, D3D9 completely ignores texture stage state. Games may set TSS to anything (including DISABLE) while a pixel shader handles all texturing. The viewer mirrors this: `use_stage0_combiner` is only set when `pixel_shader_id == 0`.

## Debugging

### Frame dump for per-draw diagnosis
The Metal viewer can dump per-draw state to `/tmp/dx9mt_frame_dump.txt`. Each draw shows: primitive type/count, vertex format, texture info (id, generation, format, size, upload status), TSS state, sampler state, blend state, viewport, and vertex data samples. Key field: `upload=0` means the texture is in the viewer's cache (no upload needed this frame), NOT that data is missing.

### Sampled logging prevents log flooding
High-frequency calls (GetDeviceCaps, CheckDeviceFormat, DebugSetMute) use `dx9mt_should_log_method_sample(&counter, first_n, every_n)`. First N calls logged in full, then every Nth call. Backend frames log on frames 0-9 then every 120th.

### Kind-tagged object IDs
Object IDs encode the type: `(kind << 24) | serial`. Kind values: 1=device, 2=swapchain, 3=buffer, 4=texture, 5=surface, 6=VS, 7=PS, 8=state_block, 9=query, 10=vertex_decl. Log line `target=33554433` = `0x02000001` = swapchain serial 1.

### Shader bytecode validation
`dx9mt_shader_dword_count` validates the version token (`0xFFFE` for VS, `0xFFFF` for PS) before scanning for the end marker. Scan limit reduced from 4MB to 256KB (64K DWORDs). Rejects bad input early with a log message.

### Replay hash for frame fingerprinting
Each frame's draw commands are hashed (FNV-1a) into a `replay_hash`. If the hash changes between frames, the draw content changed. If it's stable, the game is rendering the same scene. Visible in logs and in the overlay bar color.
