# dx9mt Next Steps

## Where We Are

RB3 Phase 3 complete. The Metal viewer now has a full D3D9 SM2.0/SM3.0 bytecode-to-MSL transpiler. Shader bytecode is transmitted from the frontend through IPC, parsed into an intermediate representation, emitted as MSL source, compiled with Metal, and cached. Draws with translated shaders use the full constant arrays instead of the hardcoded WVP/c0 approximations. The existing TSS combiner and c0 tint fallback paths remain as automatic fallback when translation fails.

## Immediate Next: Shader Translation Hardening

### The situation

The transpiler is structurally complete but untested against real FNV bytecode. The first run will likely surface MSL compilation errors from:
- VS/PS interface mismatches (output struct fields must match input struct fields)
- Missing attribute mappings (vertex elements the shader expects but the PSO doesn't declare)
- Edge cases in register usage or instruction patterns FNV shaders actually use

### Steps

1. **Run FNV** and examine stderr for shader compilation errors
2. **Fix MSL compilation issues** one by one -- the error messages include the full MSL source and parsed IR for diagnosis
3. **A/B comparison** with `DX9MT_SHADER_TRANSLATE=0` to verify translated output matches the existing fallback
4. **Flow control** -- if FNV's gameplay shaders use `if`/`else`/`endif` or `rep`/`endrep`, implement proper MSL emission (currently emitted as comments)
5. **Relative addressing** -- `a0` register for dynamic constant array indexing (`c[a0.x + N]`)

## After Hardening: RB4 -- Depth/Stencil + Pass Structure

### Depth state

Need actual render state values for:
- `D3DRS_ZENABLE`, `D3DRS_ZWRITEENABLE`, `D3DRS_ZFUNC`

Maps to `MTLDepthStencilDescriptor` -> `MTLDepthStencilState`. Also needs a depth texture attachment on the render pass. Without this, z-fighting and draw-order artifacts will appear in gameplay.

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
- Multi-texture stages (stages 1-7) in translated shaders
- Fog state
- Cull mode / fill mode render states
- More vertex declaration formats

## Priority Order

1. **Shader translation hardening** -- fix MSL compilation issues with real FNV bytecode
2. **Depth/stencil state** -- prevents z-fighting and draw ordering issues in gameplay
3. **Wine unix lib integration** -- performance, zero-copy, proper architecture
4. **In-game render state coverage** -- fog, cull, multi-texture, etc.
