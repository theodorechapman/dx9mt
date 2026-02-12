# dx9mt TODO

## Completed: RB3 Phase 2 -- Textured UI Rendering

- [x] Texture data transmission via upload arena + IPC bulk region
- [x] Texture caching by (object_id, generation) in Metal viewer
- [x] DXT1/DXT3/DXT5 compressed format support (BC1/BC2/BC3)
- [x] A8R8G8B8, X8R8G8B8, A8 format support
- [x] Texture generation tracking (dirty on Lock/Unlock, AddDirtyRect, surface copy)
- [x] Periodic texture refresh (every 60 frames) for cache recovery
- [x] Sampler state: min/mag/mip filter, address U/V/W
- [x] D3D9 TSS fixed-function combiner in Metal fragment shader
- [x] PS constant c0 tint fallback for pixel shader draws
- [x] Alpha blend state (enable, src blend, dst blend)
- [x] Alpha test state (enable, ref, func)
- [x] D3D9 default state initialization (TSS, sampler, render states)
- [x] Render-target metadata per draw (RT id, texture id, size, format)
- [x] Render-target texture routing (offscreen RT → shader-readable for later draws)
- [x] Present packet carries render_target_id for primary RT hint
- [x] RT-to-texture override cache persists across frames
- [x] FVF-to-vertex-declaration conversion for legacy draws
- [x] Pre-transformed vertex (XYZRHW/POSITIONT) handling with synthetic screen-to-NDC matrix

## Current Priority: RB3 Phase 3 -- D3D9 Shader Translation

- [ ] Transmit VS/PS bytecode via IPC (upload ref or shader registry packet)
- [ ] D3D9 SM2.0/SM3.0 bytecode parser (version token, instructions, dcl, def)
- [ ] Register mapping: D3D9 registers → MSL variables
- [ ] VS instruction emitter: dp4, mov, add, mul, mad (enough for WVP + basic transforms)
- [ ] PS instruction emitter: texld, mov, mul, add, mad, cmp, lrp
- [ ] MSL source compilation + PSO caching by shader bytecode hash
- [ ] Replace hardcoded "WVP from c0-c3" vertex shader with translated VS
- [ ] Replace "texture * c0" pixel shader fallback with translated PS

## Next After Shaders: RB4 -- Depth/Stencil + Pass Structure

- [ ] Transmit depth/stencil render state values in draw packet
- [ ] Create MTLDepthStencilState from D3D9 state
- [ ] Depth texture attachment per render pass
- [ ] Stencil state (enable, func, pass/fail ops)
- [ ] Explicit clear behavior per non-primary render target
- [ ] Blend-op fidelity (D3DRS_BLENDOP: ADD, SUBTRACT, etc.)
- [ ] Separate alpha blend support

## Later

- [ ] Wine unix lib integration (__wine_unix_call, zero-copy)
- [ ] Multi-texture stages (stages 1-7)
- [ ] Fog state
- [ ] Cull mode / fill mode render states
- [ ] Buffer ring allocator (replace per-draw newBufferWithBytes)
- [ ] PSO cache by full (vertex layout + blend + depth) hash
- [ ] Async shader compilation
- [ ] In-game rendering validation (gameplay, save/load)
