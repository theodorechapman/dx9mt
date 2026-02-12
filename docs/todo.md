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

## Completed: RB3 Phase 3 -- D3D9 Shader Translation

- [x] Transmit VS/PS bytecode via IPC (upload ref in draw packet + bulk copy)
- [x] Add vertex_shader_id to IPC draw entry (was missing)
- [x] D3D9 SM2.0/SM3.0 bytecode parser (version token, instructions, dcl, def)
- [x] Register token decoding (type, number, swizzle, write mask, source/result modifiers)
- [x] Register mapping: D3D9 registers → MSL variables (r#, v#, c#, s#, oPos, oD#, oT#, oC#)
- [x] VS instruction emitter: dp3, dp4, mov, add, sub, mul, mad, rcp, rsq, min, max, slt, sge, exp, log, lit, dst, lrp, frc, pow, crs, abs, nrm, sincos, mova, m4x4, m4x3, m3x4, m3x3, m3x2
- [x] PS instruction emitter: texld, texldl, texkill, mov, mul, add, mad, cmp, lrp, dp2add + all VS arithmetic ops
- [x] Swizzle emission, write mask application, source modifiers (negate, abs, complement, x2, bias)
- [x] Result modifier _sat → saturate()
- [x] MSL source compilation via newLibraryWithSource + function cache by bytecode hash
- [x] Translated PSO creation and caching by (vs_hash, ps_hash, vertex layout, blend state)
- [x] Fallback to hardcoded TSS/c0 path on parse/compile failure (sticky cache)
- [x] POSITIONT draws skip translation (use existing synthetic matrix path)
- [x] DX9MT_SHADER_TRANSLATE=0 env var for A/B comparison

## Current Priority: RB4 -- Depth/Stencil + Pass Structure

- [ ] Transmit depth/stencil render state values in draw packet
- [ ] Create MTLDepthStencilState from D3D9 state
- [ ] Depth texture attachment per render pass
- [ ] Stencil state (enable, func, pass/fail ops)
- [ ] Explicit clear behavior per non-primary render target
- [ ] Blend-op fidelity (D3DRS_BLENDOP: ADD, SUBTRACT, etc.)
- [ ] Separate alpha blend support

## Next: Shader Translation Hardening

- [ ] Test with FNV, fix MSL compilation issues from real game bytecode
- [ ] Flow control translation (if/else/endif, rep/endrep, break/breakc)
- [ ] Relative addressing (a0 register for dynamic constant indexing)
- [ ] Multi-texture support in translated PS (tex1..tex7 bindings)
- [ ] VS/PS interface linkage validation (match output semantics to input semantics)

## Later

- [ ] Wine unix lib integration (__wine_unix_call, zero-copy)
- [ ] Multi-texture stages (stages 1-7)
- [ ] Fog state
- [ ] Cull mode / fill mode render states
- [ ] Buffer ring allocator (replace per-draw newBufferWithBytes)
- [ ] PSO cache by full (vertex layout + blend + depth) hash
- [ ] Async shader compilation
- [ ] In-game rendering validation (gameplay, save/load)
