# dx9mt Next Steps

## Where We Are

RB5 is in progress. The major infrastructure for in-game rendering is now in place:

- **Cull mode** -- D3DRS_CULLMODE transmitted through full pipeline and applied per-draw via `[encoder setCullMode:]`. D3D9 left-handed winding convention mapped correctly (D3DCULL_CW -> MTLCullModeFront, D3DCULL_CCW -> MTLCullModeBack). Default is D3DCULL_CCW.

- **Shader flow control** -- The transpiler now handles if/ifc/else/endif, rep/endrep, break/breakc. Comparison operators (GT/EQ/GE/LT/NE/LE) extracted from instruction token bits 18-20. Integer constant registers from `defi` emitted as `float4 i#` for loop bounds. Boolean constant registers from `defb` emitted as `float4 b#` for predicates. Shaders using these opcodes now translate to MSL instead of falling back to the TSS/c0 path.

- **Multi-texture** -- The entire pipeline was refactored from individual `texture0_*`/`sampler0_*` fields to `[DX9MT_MAX_PS_SAMPLERS]` arrays (8 stages). Frontend captures texture metadata + sampler state for all bound stages. Backend bridge copies all stages through hash/record/IPC. Metal viewer binds all active stages at matching `[[texture(N)]]`/`[[sampler(N)]]` indices. The shader emitter already generated correct multi-texture MSL -- only the data pipeline was blocking. PS sampler index > 0 rejection removed from parser.

The FNV main menu continues to render correctly. The system is ready for in-game testing.

## Immediate Next: Test In-Game Rendering

### The situation

The infrastructure for 3D rendering is now in place: depth testing, cull mode, flow control, and multi-texture. The next step is to enter FNV gameplay and see what happens.

### Steps

1. **Enter FNV gameplay** -- start a new game or load a save, observe what renders and what breaks
2. **Fix shader compilation errors** -- stderr will show full MSL source for failures. Most likely causes:
   - Relative addressing (`c[a0.x + N]`) -- still hard-fails, needed for skinned meshes
   - Unhandled opcodes -- any SM2/SM3 ops not yet in the emitter
3. **Add missing render states** as needed:
   - **Fog** -- outdoor scenes will look wrong without distance fog
   - **Stencil operations** -- shadow volumes, UI masking (state transmitted, need MTL stencil ops)
   - **Blend operations** -- SUBTRACT, REVSUBTRACT, MIN, MAX (currently only ADD)
4. **Validate depth correctness** -- 3D scenes should have proper occlusion
5. **Check multi-texture binding** -- normal maps, specular maps should now be sampled correctly

### Remaining RB5 Items

| Item | Priority | Notes |
|------|----------|-------|
| Relative addressing (a0) | High | Blocks skinned character rendering |
| VS/PS linkage validation | Medium | Wrong output but won't crash |
| Fog state | Medium | Cosmetic, outdoor scenes |
| Stencil operations | Medium | Shadow volumes, masking |
| Blend-op fidelity | Low | SUBTRACT/MIN/MAX are rare |
| Fill mode | Low | Almost always solid |
| Separate alpha blend | Low | Uncommon in FNV |

## Medium Term

### Wine unix lib integration
Replace IPC with in-process `__wine_unix_call` for zero-copy data sharing and synchronized present. The `libdx9mt_unixlib.dylib` is already built with Metal support.

### Performance optimization
- Buffer ring allocator to replace per-draw `newBufferWithBytes` allocations
- State deduplication to avoid redundant PSO/DSS lookups
- PSO cache serialization to disk for faster startup
- Async shader compilation to avoid hitching on new shaders

## Priority Order

1. **Test in-game rendering** -- enter gameplay, observe, iterate
2. **Relative addressing** -- unblock skinned mesh shaders
3. **Render state coverage** -- fog, stencil ops as needed
4. **Wine unix lib integration** -- performance, zero-copy, proper architecture
5. **Performance optimization** -- buffer recycling, state dedup, async compile
