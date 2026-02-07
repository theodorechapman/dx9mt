# dx9mt Next Steps

## Where We Are

RB3 Phase 1 complete. The Metal viewer renders FNV's main menu geometry with correct screen-space positions (WVP matrix from VS constants c0-c3) and vertex colors. No textures, no depth testing, no blend state, no real shader translation.

## Immediate Next: RB3 Phase 2 -- Textured Geometry

### 1. Texture data transmission

The frontend has texture surface data (`dx9mt_texture.surfaces[]` with sysmem pixel bytes) but doesn't transmit it. Needs:

- New packet field or IPC mechanism for texture data per bound texture stage
- Texture metadata: width, height, format, mip count per texture object ID
- Texture data: raw pixel bytes for at least mip level 0
- Sampler state: filter mode, address mode (actual values, not hashes)

The challenge: textures can be large and are set per-stage, not per-draw. Need a texture upload mechanism that avoids re-sending unchanged textures every frame.

Approach: **texture registry** -- the frontend sends texture data once when the texture is first used (or when Lock/Unlock modifies it). The backend/viewer caches textures by object ID. Per-draw, only the texture ID bindings change.

### 2. Sampler state

Currently only hashed. Need actual values for:
- `D3DSAMP_MINFILTER`, `D3DSAMP_MAGFILTER`, `D3DSAMP_MIPFILTER`
- `D3DSAMP_ADDRESSU`, `D3DSAMP_ADDRESSV`, `D3DSAMP_ADDRESSW`

Map to `MTLSamplerDescriptor` fields. Create `MTLSamplerState` objects and bind per draw.

### 3. Depth/stencil state

Need actual render state values (not hashes) for:
- `D3DRS_ZENABLE`, `D3DRS_ZWRITEENABLE`, `D3DRS_ZFUNC`
- `D3DRS_STENCILENABLE`, `D3DRS_STENCILFUNC`, `D3DRS_STENCILPASS`, etc.

Maps to `MTLDepthStencilDescriptor` -> `MTLDepthStencilState`. Also needs a depth texture attachment on the render pass.

### 4. Blend state

Need actual render state values for:
- `D3DRS_ALPHABLENDENABLE`, `D3DRS_SRCBLEND`, `D3DRS_DESTBLEND`
- `D3DRS_BLENDOP`, `D3DRS_SEPARATEALPHABLENDENABLE`
- `D3DRS_COLORWRITEENABLE`

Maps to `MTLRenderPipelineColorAttachmentDescriptor` blend fields. Changes the PSO, so needs PSO caching by blend state.

## Medium Term: Shader Translation

### The problem

The Metal vertex shader currently hardcodes "multiply position by WVP from c0-c3". This works for FNV's main menu but will break for:
- Shaders that store the WVP matrix at different constant registers
- Shaders that do skinning, lighting, or texture coordinate generation
- Pixel shaders (currently no PS translation at all)

### Approaches

**A. D3D9 bytecode interpreter (fastest to prototype)**
Parse DXBC SM3.0 instruction stream and emit equivalent MSL source. SM3.0 has ~70 instruction types but FNV likely uses <20. Generate MSL per unique shader, compile with `newLibraryWithSource`, cache the PSO.

Pros: Direct, no external dependencies
Cons: Must handle every instruction FNV uses, fragile

**B. SPIRV-Cross pipeline (more robust)**
Use `spirv-cross` to convert SPIR-V to MSL. But D3D9 bytecode isn't SPIR-V -- need a D3D9-to-SPIR-V translator first (e.g., from DXVK's `d3d9` module or a custom one).

Pros: Leverages mature tooling
Cons: Large dependency, two translation steps

**C. Mesa/NIR pipeline**
Use Mesa's NIR intermediate representation. The `nir_d3d9` frontend could parse D3D9 bytecode into NIR, then a NIR-to-MSL backend emits Metal shaders.

Pros: Well-tested D3D9 parsing in Mesa
Cons: Huge dependency, complex integration

Recommendation: Start with **A** (bytecode interpreter) for FNV's small shader set. Profile which instructions FNV actually uses, implement just those. Expand as needed.

## Architecture Evolution: Wine Unix Lib Integration

### The problem

The current IPC approach (shared memory file between PE DLL and native viewer) works but has limitations:
- Copies all geometry data per frame (VB/IB/constants) through file I/O
- Texture streaming through the file will be bandwidth-constrained
- Two separate processes means no zero-copy data sharing
- Present timing is poll-based (120Hz timer), not synchronized

### The solution

Implement Wine's unix library mechanism (`__wine_unix_call`) so the PE DLL calls directly into the native ARM64 dylib. This gives:
- In-process, zero-copy access to the upload arena and frontend state
- Direct Metal API calls from the bridge (no IPC needed)
- Synchronized present (no polling)
- The existing `metal_presenter.m` code works as-is

This is the correct long-term architecture. The `libdx9mt_unixlib.dylib` is already built with the Metal presenter -- it just needs to be loaded by Wine.

### Implementation sketch

1. PE DLL calls `__wine_init_unix_lib` at `DLL_PROCESS_ATTACH`
2. Wine loads `dx9mt_unixlib.dylib` into the process
3. The dylib exports a dispatch function table
4. Bridge calls go through `__wine_unix_call(handle, func_code, params)`
5. Metal rendering happens in-process with direct memory access

## Performance Optimization (RB5)

Not critical yet, but track these for later:

- **Buffer recycling**: replace per-draw `newBufferWithBytes` with a ring allocator
- **PSO cache**: cache by (vertex layout + blend state + depth state) hash, not just stride
- **State dedup**: skip redundant viewport/PSO/buffer binds when state hasn't changed
- **Texture cache**: cache `MTLTexture` objects by (object_id + generation), only re-upload on Lock/Unlock
- **Async shader compile**: compile MSL shaders on background thread, use fallback until ready
- **Triple-buffered command submission**: overlap CPU recording with GPU execution

## Priority Order

1. **Texture data + sampler state** -- most visual impact, FNV menus need textures
2. **Depth/stencil state** -- prevents z-fighting and draw ordering issues
3. **Blend state** -- transparency, UI overlays
4. **D3D9 shader translation** -- correctness for all geometry, not just WVP-at-c0
5. **Wine unix lib integration** -- performance, zero-copy, proper architecture
6. **Render state transmission** -- cull mode, fill mode, fog, etc.
