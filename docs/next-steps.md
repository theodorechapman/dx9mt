# dx9mt Next Steps

## Where We Are

RB4 complete. The full data pipeline from frontend render state capture through IPC to Metal depth testing is operational. The Metal viewer creates per-render-target Depth32Float textures, caches MTLDepthStencilState objects, attaches depth to every render pass, and binds depth state per draw. All three PSO paths (fixed-function geometry, translated shader, overlay) include the depth format. Stencil state fields are transmitted but not yet consumed for stencil operations.

The FNV main menu continues to render correctly -- the menu is 2D so depth state is effectively transparent (zenable=1, zwrite=1, zfunc=LESSEQUAL with all geometry at the same depth).

## Immediate Next: RB5 -- In-Game Rendering

### The situation

The main menu is fully rendering. To advance into gameplay, the viewer needs to handle the more diverse rendering that FNV uses in-game: 3D geometry with actual depth variation, potentially skinned meshes, landscape, multiple render targets, shadow passes, and more shader variety.

### Shader Translation Hardening

The transpiler handles FNV's menu shaders but gameplay shaders may use:
1. **Flow control** -- `if`/`else`/`endif`, `rep`/`endrep`, `break`/`breakc` (currently emitted as comments)
2. **Relative addressing** -- `a0` register for dynamic constant array indexing (`c[a0.x + N]`)
3. **Multi-texture** -- gameplay PS may sample from multiple texture stages
4. **More instruction patterns** -- any SM2/SM3 opcodes not yet seen in the menu

### Render State Coverage

Gameplay draws will exercise render states the menu doesn't use:
- **Cull mode** -- back-face culling for 3D geometry
- **Fog** -- distance fog in outdoor scenes
- **Stencil operations** -- shadow volumes, UI masking (state already transmitted, need MTL stencil ops)
- **Blend operations** -- SUBTRACT, REVSUBTRACT, MIN, MAX (currently only ADD)
- **Separate alpha blend** -- independent alpha channel blend factors

### Steps

1. **Enter FNV gameplay** -- start a new game or load a save, observe what breaks
2. **Fix shader compilation errors** -- stderr will show full MSL source for failures
3. **Add missing render states** as needed (cull, fog, stencil ops)
4. **Multi-texture** -- when gameplay PS samples multiple textures
5. **Validate depth correctness** -- 3D scenes should have proper occlusion now

## Medium Term

### Wine unix lib integration
Replace IPC with in-process `__wine_unix_call` for zero-copy data sharing and synchronized present. The `libdx9mt_unixlib.dylib` is already built with Metal support.

### Performance optimization
- Buffer ring allocator to replace per-draw `newBufferWithBytes` allocations
- State deduplication to avoid redundant PSO/DSS lookups
- PSO cache serialization to disk for faster startup
- Async shader compilation to avoid hitching on new shaders

## Priority Order

1. **In-game rendering** -- enter gameplay, fix what breaks
2. **Shader hardening** -- flow control, relative addressing, multi-texture
3. **Render state coverage** -- cull, fog, stencil ops
4. **Wine unix lib integration** -- performance, zero-copy, proper architecture
5. **Performance optimization** -- buffer recycling, state dedup, async compile
