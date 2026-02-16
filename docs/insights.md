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

## Threading and Stability

### FNV requests `D3DCREATE_MULTITHREADED` in the gameplay path
During save-load / in-game initialization, FNV creates the D3D9 device with behavior flags `0x00000054`, which includes `D3DCREATE_MULTITHREADED`. This means the runtime must serialize API entry points across threads.

### Missing frontend serialization can crash game code, not just rendering
The observed save-load crash (`falloutnv+0x757aa9`, `movl (%esi), %eax`, `ESI=0`) occurred on a worker thread while the frontend showed heavy concurrent resource creation. Without runtime-style serialization, caller-visible state can be torn across threads and crash in game code before backend validation catches anything.

### Coarse per-device lock is the correct first compatibility move
The frontend now uses a per-device critical section and lock-aware `IDirect3DDevice9` vtbl wrappers, plus guarded resource lock/unlock paths (VB/IB/surface/texture). This mirrors D3D9's coarse `D3DCREATE_MULTITHREADED` semantics and trades performance for correctness.

## Packet Protocol

### BEGIN_FRAME is now in the packet stream
Originally `BeginScene` called `begin_frame()` as a direct side-channel. Now it emits a `BEGIN_FRAME` packet through `submit_packets`, making the stream self-describing. The backend parser dispatches `BEGIN_FRAME` packets to the same `begin_frame()` logic.

### Draw packet carries full geometry data + shader bytecode
`dx9mt_packet_draw_indexed` includes upload refs for VB data, IB data, vertex declaration, VS constants, PS constants, texture data (8 stages), and VS/PS shader bytecode. The frontend copies all this into the upload arena on every `DrawIndexedPrimitive`. The IPC writer then copies from the arena into the shared memory bulk region. Shader bytecode is small (1-10KB per shader) and many draws reuse the same shader, so the same bytecode is uploaded redundantly per-draw but the viewer deduplicates by hash.

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

### D3D9 cull mode winding convention
D3D9 uses left-handed winding by default. D3DCULL_CW means "cull faces with clockwise winding" which are front faces in Metal's right-handed convention. So D3DCULL_CW maps to MTLCullModeFront and D3DCULL_CCW maps to MTLCullModeBack. The default D3D9 cull mode is D3DCULL_CCW (back-face culling), which maps to MTLCullModeBack. This is set per-draw via `[encoder setCullMode:]`, not baked into the PSO.

## Texture Pipeline

### Texture caching with generation tracking
Each `dx9mt_texture` has a `generation` counter incremented on Lock/Unlock, AddDirtyRect, and surface copy operations. The frontend only uploads texture data when the generation changes (or on a periodic 60-frame refresh for cache recovery). The viewer caches `MTLTexture` objects by `(object_id, generation)` and skips re-creation when the generation matches.

### DXT compressed texture support
D3D9 DXT1/DXT3/DXT5 formats map directly to Metal's BC1_RGBA/BC2_RGBA/BC3_RGBA. Block-compressed pitch is calculated as `((width + 3) / 4) * block_bytes` where block_bytes is 8 for DXT1 and 16 for DXT3/DXT5. The frontend computes this correctly for both surface allocation and upload sizing.

### Block-compressed copies must respect 4x4 block geometry
For DXT surfaces, copy rects must be block-aligned (except right/bottom edge at full surface size), and scaling is invalid. `ColorFill` is also invalid for block-compressed surfaces. Enforcing these rules in `UpdateSurface`/`StretchRect`/`UpdateTexture` paths prevents silent memory corruption.

### Render-target texture routing
Draws can target offscreen render targets (not just the swapchain). The viewer tracks per-draw `render_target_id` and creates separate `MTLTexture` objects for each RT. When a later draw samples a texture whose `texture_id` matches a previous RT's `render_target_texture_id`, the viewer substitutes the RT's Metal texture. This enables render-to-texture effects (e.g., UI compositing).

### Multi-texture uses arrays, not duplicated fields
The initial stage-0-only implementation used individual fields (`texture0_id`, `sampler0_min_filter`, etc.) across packets, IPC, and backend structs. For multi-texture (stages 0-7), these were refactored to fixed-size arrays (`tex_id[DX9MT_MAX_PS_SAMPLERS]`, `sampler_min_filter[DX9MT_MAX_PS_SAMPLERS]`). This keeps the code manageable -- copy/hash/validation sites use loops instead of 8x field duplication. The wire format is internal-only so there are no backward compatibility concerns. TSS combiner fields remain stage-0-only since they are irrelevant when a pixel shader is active.

### Multi-texture data budget
With 8 stages, texture metadata per draw grew from ~80 bytes to ~640 bytes. With DX9MT_METAL_IPC_MAX_DRAWS=256 draws and 16MB IPC, each draw has ~64KB budget, so this fits comfortably. Texture upload data is dominated by cache behavior -- most draws transmit 0 bytes for cached textures. Only first-seen or dirty textures upload actual pixel data.

## Shader Translation (RB3 Phase 3 + RB5)

### D3D9 SM2.0/SM3.0 bytecode format
The bytecode is a stream of 32-bit DWORD tokens. Token 0 is the version (`0xFFFE03xx` for VS 3.x, `0xFFFF03xx` for PS 3.x). Each instruction is an opcode token followed by destination and source register tokens. The stream ends with `0x0000FFFF`. Register tokens encode type (5 bits split across bits [30:28] and [12:11]), number (bits [10:0]), and for sources: swizzle (bits [23:16], 2 bits per component) and modifier (bits [27:24]). For destinations: write mask (bits [19:16]) and result modifier (bits [23:20]).

### Transpiler architecture: parse -> IR -> MSL source -> compile -> cache
The bytecode is parsed into a flat IR (`dx9mt_sm_program` with instruction/dcl/def arrays), then emitted as MSL source text, compiled with `[MTLDevice newLibraryWithSource:]`, and the resulting `MTLFunction` cached by bytecode hash. Compilation happens once per unique shader; subsequent draws just look up the cache. Failures are sticky (cached as NSNull) to avoid retrying every frame.

### VS/PS interface uses a "fat" interpolant struct
The emitted VS output struct and PS input struct must have matching field names and types. Rather than dynamically matching VS output semantics to PS input semantics (complex), the emitter generates per-shader structs based on dcl declarations. The VS writes its declared outputs; the PS reads its declared inputs. As long as the same VS/PS pair is used together, the structs match.

### Register mapping is straightforward for SM3.0
- `r#` -> `float4 r#` (local variable, initialized to 0)
- `v#` -> `in.v#` (from `[[stage_in]]` struct)
- `c#` -> `c[#]` (constant buffer at buffer index 1 for VS, 0 for PS)
- `i#` -> `float4 i#` (from `defi`, used as loop bounds via `int(i#.x)`)
- `b#` -> `float4 b#` (from `defb`, used as boolean predicate via `b#.x != 0.0`)
- `s#` + `texld` -> `tex#.sample(samp#, coord.xy)` (texture + sampler pair)
- `oPos` (rastout 0) -> `out.position` with `[[position]]`
- `oC0` -> return value of fragment function

### D3D9 swizzle maps directly to MSL component access
D3D9 swizzle encodes 4 component indices (0=x, 1=y, 2=z, 3=w). Identity swizzle `.xyzw` is omitted. Replicate swizzle `.xxxx` emits `.x`. The emitter handles all modifiers: negate `(-expr)`, abs `abs(expr)`, complement `(1.0 - expr)`, x2 `(expr * 2.0)`, bias `(expr - 0.5)`.

### Flow control token format differs from arithmetic instructions
Arithmetic instructions follow the pattern: opcode token, destination token, source tokens. Flow control opcodes have different layouts:
- `ifc`/`breakc`: opcode token (comparison in bits 18-20) + 2 source tokens, no destination. The comparison (GT=1, EQ=2, GE=3, LT=4, NE=5, LE=6) must be extracted before consuming source tokens.
- `rep`/`if`: opcode token + 1 source token (integer or boolean register), no destination.
- `else`/`endif`/`endrep`/`break`: opcode token only, no additional tokens.

The parser previously used `opcode_src_count()` returning -2 for flow control (special marker), which caused the hard-fail path. Now each opcode is handled individually before the generic arithmetic parsing.

### ifc/breakc compare scalar .x components
D3D9 `ifc` compares the .x component of two source registers. The emitter generates `if (s0.x > s1.x) {` rather than vector comparison. This matches D3D9 spec behavior where the comparison operates on the first component.

### rep loops use integer constant registers coerced to float4
D3D9's `defi i0, 4, 0, 0, 0` defines an integer constant. The emitter stores these as `float4 i0 = float4(4.0, 0.0, 0.0, 0.0)` and the `rep` loop uses `int(i0.x)` for the bound. This avoids a separate integer register type system while maintaining correct loop counts (integer constants are always small whole numbers).

### Multi-texture emitter was already correct
The MSL emitter generates `tex%u.sample(samp%u, coord.xy)` using the sampler register number from bytecode, and the PS function signature declares `texture2d<float> tex%u [[texture(%u)]]` / `sampler samp%u [[sampler(%u)]]` for each declared sampler. This naturally supports any sampler index 0-7. The only blocker was the data pipeline (individual stage-0 fields) and the parser's explicit rejection of PS sampler index > 0.

### POSITIONT draws skip translation entirely
Pre-transformed vertices (D3DFVF_XYZRHW / `dcl_positiont`) are already in screen space. The existing hardcoded `geo_vertex` shader with a synthetic screen-to-NDC matrix handles these correctly. When the vertex declaration has POSITIONT, the viewer uses the fallback path and only the PS could potentially be translated (currently skipped -- both VS and PS are needed for the translated path).

## Fragment Shader Strategy

### Four-tier fragment shader approach (with translation)
The Metal viewer now has four tiers for fragment shading, tried in order:

1. **Translated shader** (when bytecode is available and compiles): Full D3D9 shader translated to MSL. Reads from constant buffer, samples textures (up to 8 stages), executes all arithmetic including flow control. Uses the full VS/PS constant arrays at buffer indices 1 and 0 respectively.

2. **TSS fixed-function combiner** (`use_stage0_combiner=1`, when `pixel_shader_id==0`): Full D3D9 texture stage state evaluation. Supports all D3DTOP operations (MODULATE, SELECTARG, ADD, BLEND*, etc.) with configurable arg sources (TEXTURE, CURRENT, DIFFUSE, TFACTOR). Stage 0 only.

3. **PS constant c0 tint** (`has_pixel_shader=1`, translation failed): When a D3D9 pixel shader is active but translation failed, the viewer reads PS constant register c0 and multiplies it by the texture sample. This approximation works for FNV's menu shaders.

4. **Raw passthrough** (no PS, no TSS): `output = diffuse * texture` (textured) or `output = diffuse` (non-textured).

### D3D9 TSS state is irrelevant when a pixel shader is active
When a pixel shader is bound, D3D9 completely ignores texture stage state. Games may set TSS to anything (including DISABLE) while a pixel shader handles all texturing. The viewer mirrors this: `use_stage0_combiner` is only set when `pixel_shader_id == 0`.

## Depth/Stencil (RB4)

### Metal requires depth format on PSO even if depth test is disabled
All PSOs that render into a pass with a depth attachment must declare `depthAttachmentPixelFormat`. Even the overlay PSO (which doesn't care about depth) needs this set to `Depth32Float` because the render pass has a depth attachment. Without it, Metal will fail PSO creation with a format mismatch.

### Depth textures are per-render-target, not per-frame
Each render target (including the drawable) needs its own `Depth32Float` texture. The viewer caches these in `s_depth_texture_cache` keyed by RT ID, and separately caches the drawable depth texture. When a render target changes size, the depth texture is recreated. Depth textures use `MTLStorageModePrivate` since they never need CPU access.

### Depth clear follows the game's Clear() flags
D3D9's `Clear()` has separate flags for color (0x1) and depth (0x2). The depth clear value (`clear_z`) is transmitted through IPC in the frame header. On first use of a render target, if the game issued a depth clear, the render pass uses `MTLLoadActionClear` with the game's clear_z value. On subsequent uses (when the target was already rendered to this frame), the pass uses `MTLLoadActionLoad` to preserve the depth buffer.

### 2D menu rendering is depth-transparent
FNV's main menu draws 2D quads (POSITIONT) with default depth state (zenable=1, zwrite=1, zfunc=LESSEQUAL). Since all geometry is at the same depth, depth testing passes for everything and the result is identical to no depth test. This means RB4 can be validated by checking that the menu still looks correct -- any regression would indicate a PSO or render pass configuration bug.

### Stencil state fields are transmitted but not yet consumed
The 5 stencil fields (enable, func, ref, mask, writemask) are transmitted through the full pipeline and visible in frame dumps, but the Metal viewer doesn't create stencil textures or configure stencil operations on the MTLDepthStencilDescriptor yet. This is deferred to when FNV gameplay actually uses stencil (shadow volumes, UI masking).

## Debugging

### Frame dump for per-draw diagnosis
The Metal viewer can dump per-draw state to `/tmp/dx9mt_frame_dump.txt`. Each draw shows: primitive type/count, vertex format, per-stage texture info (id, generation, format, size, upload status for all active stages 0-7), per-stage sampler state, TSS state, blend state, depth state, cull mode, viewport, and vertex data samples. Key field: `upload=0` means the texture is in the viewer's cache (no upload needed this frame), NOT that data is missing.

### Sampled logging prevents log flooding
High-frequency calls (GetDeviceCaps, CheckDeviceFormat, DebugSetMute) use `dx9mt_should_log_method_sample(&counter, first_n, every_n)`. First N calls logged in full, then every Nth call. Backend frames log on frames 0-9 then every 120th.

### Kind-tagged object IDs
Object IDs encode the type: `(kind << 24) | serial`. Kind values: 1=device, 2=swapchain, 3=buffer, 4=texture, 5=surface, 6=VS, 7=PS, 8=state_block, 9=query, 10=vertex_decl. Log line `target=33554433` = `0x02000001` = swapchain serial 1.

### Shader bytecode validation
`dx9mt_shader_dword_count` validates the version token (`0xFFFE` for VS, `0xFFFF` for PS) before scanning for the end marker. Scan limit reduced from 4MB to 256KB (64K DWORDs). Rejects bad input early with a log message.

### Replay hash for frame fingerprinting
Each frame's draw commands are hashed (FNV-1a) into a `replay_hash`. If the hash changes between frames, the draw content changed. If it's stable, the game is rendering the same scene. Visible in logs and in the overlay bar color.
