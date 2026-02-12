# dx9mt Next Steps

## Where We Are

RB3 Phase 2 complete. The Metal viewer renders FNV's main menu with correct colors: textured backgrounds (DXT1/DXT5), yellow menu text (PS c0 tint), alpha-blended UI elements, and render-target routing. The remaining gap for visual correctness is proper D3D9 shader translation.

## Immediate Next: RB3 Phase 3 -- D3D9 Shader Translation

### The problem

The viewer currently uses two approximations:
1. **Vertex shader**: Hardcoded "multiply position by WVP from c0-c3". Works for FNV's menu but breaks for shaders with different constant layouts, skinning, lighting, or texcoord generation.
2. **Pixel shader**: Falls back to `texture * c0` when a pixel shader is active. Works for FNV's menu tint but can't handle multi-texture blending, per-pixel lighting, or complex combiners.

### Approach: D3D9 SM2.0/SM3.0 bytecode → MSL transpiler

Parse the D3D9 shader bytecode (DXBC format) and emit equivalent Metal Shading Language source. Key observations:

- FNV uses a small set of shaders (~20-50 unique VS/PS combinations)
- SM3.0 has ~70 instruction types but FNV likely uses <20
- Shader constants are already transmitted (VS/PS float constants in IPC bulk data)
- The transpiler runs once per unique shader, then the compiled MSL is cached

### Implementation sketch

1. **Bytecode parser**: Read version token, instruction stream, dcl registers, def constants
2. **Register allocator**: Map D3D9 registers (r0-r31, v0-v15, t0-t7, c0-c255, s0-s15) to MSL variables
3. **Instruction emitter**: Translate each D3D9 opcode to MSL:
   - Arithmetic: `add`, `mul`, `mad`, `dp3`, `dp4`, `rsq`, `rcp`, `min`, `max`, `mov`
   - Texture: `texld` (tex2D), `texldl`, `texldd`
   - Flow: `if_*`, `else`, `endif`, `rep`, `endrep`
   - Comparison: `slt`, `sge`, `cmp`
4. **VS output**: Emit MSL vertex function reading from vertex attributes and constant buffer
5. **PS output**: Emit MSL fragment function reading from interpolants, textures, and constant buffer
6. **PSO integration**: Compile MSL source with `newLibraryWithSource:`, create PSO, cache by shader bytecode hash

### What to transmit

The frontend already has the shader bytecode (stored at `CreateVertexShader`/`CreatePixelShader` time). Need to:
- Add shader bytecode upload ref to the draw packet (or a separate shader registry packet)
- Include bytecode size so the viewer knows the full program
- Hash the bytecode for PSO cache lookup

### Priority instructions for FNV menu + early gameplay

VS: `dcl_position`, `dcl_texcoord`, `dcl_color`, `dp4` (WVP transform), `mov`, `add`, `mul`, `mad`
PS: `dcl_2d`, `texld`, `mov`, `mul`, `add`, `mad`, `cmp`, `lrp`

## After Shader Translation: RB4 -- Depth/Stencil

### Depth state

Need actual render state values for:
- `D3DRS_ZENABLE`, `D3DRS_ZWRITEENABLE`, `D3DRS_ZFUNC`

Maps to `MTLDepthStencilDescriptor` → `MTLDepthStencilState`. Also needs a depth texture attachment on the render pass. Without this, z-fighting and draw-order artifacts will appear in gameplay.

### Stencil state

- `D3DRS_STENCILENABLE`, `D3DRS_STENCILFUNC`, `D3DRS_STENCILPASS`, etc.

FNV uses stencil for shadow volumes and some UI masking.

## Medium Term

### Blend-op fidelity
Currently only SRCALPHA/INVSRCALPHA is tested. Need full `D3DRS_BLENDOP` (ADD, SUBTRACT, REVSUBTRACT, MIN, MAX) and separate alpha blend support.

### Wine unix lib integration
Replace IPC with in-process `__wine_unix_call` for zero-copy data sharing and synchronized present. The `libdx9mt_unixlib.dylib` is already built with Metal support.

### In-game rendering
FNV gameplay uses much more diverse rendering: skinned meshes, landscape, multiple render targets, shadow passes. Requires all of the above plus:
- Multi-texture stages (stages 1-7)
- Fog state
- Cull mode / fill mode render states
- More vertex declaration formats

## Priority Order

1. **D3D9 shader translation** -- correctness for all geometry, eliminates c0 tint hack
2. **Depth/stencil state** -- prevents z-fighting and draw ordering issues in gameplay
3. **Wine unix lib integration** -- performance, zero-copy, proper architecture
4. **In-game render state coverage** -- fog, cull, multi-texture, etc.
