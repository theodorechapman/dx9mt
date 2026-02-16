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
- [x] Render-target texture routing (offscreen RT -> shader-readable for later draws)
- [x] Present packet carries render_target_id for primary RT hint
- [x] RT-to-texture override cache persists across frames
- [x] FVF-to-vertex-declaration conversion for legacy draws
- [x] Pre-transformed vertex (XYZRHW/POSITIONT) handling with synthetic screen-to-NDC matrix

## Completed: RB3 Phase 3 -- D3D9 Shader Translation

- [x] Transmit VS/PS bytecode via IPC (upload ref in draw packet + bulk copy)
- [x] Add vertex_shader_id to IPC draw entry (was missing)
- [x] D3D9 SM2.0/SM3.0 bytecode parser (version token, instructions, dcl, def)
- [x] Register token decoding (type, number, swizzle, write mask, source/result modifiers)
- [x] Register mapping: D3D9 registers -> MSL variables (r#, v#, c#, s#, oPos, oD#, oT#, oC#)
- [x] VS instruction emitter: dp3, dp4, mov, add, sub, mul, mad, rcp, rsq, min, max, slt, sge, exp, log, lit, dst, lrp, frc, pow, crs, abs, nrm, sincos, mova, m4x4, m4x3, m3x4, m3x3, m3x2
- [x] PS instruction emitter: texld, texldl, texkill, mov, mul, add, mad, cmp, lrp, dp2add + all VS arithmetic ops
- [x] Swizzle emission, write mask application, source modifiers (negate, abs, complement, x2, bias)
- [x] Result modifier _sat -> saturate()
- [x] MSL source compilation via newLibraryWithSource + function cache by bytecode hash
- [x] Translated PSO creation and caching by (vs_hash, ps_hash, vertex layout, blend state)
- [x] Fallback to hardcoded TSS/c0 path on parse/compile failure (sticky cache)
- [x] POSITIONT draws skip translation (use existing synthetic matrix path)
- [x] DX9MT_SHADER_TRANSLATE=0 env var for A/B comparison

## Completed: RB4 -- Depth/Stencil Support

- [x] Transmit depth render states (ZENABLE, ZWRITEENABLE, ZFUNC) in draw packet
- [x] Transmit stencil render states (STENCILENABLE, STENCILFUNC, STENCILREF, STENCILMASK, STENCILWRITEMASK)
- [x] Mirror depth/stencil fields through backend bridge (draw command struct, hash, IPC copy)
- [x] D3D9 default state initialization for depth (ZFUNC=LESSEQUAL) and stencil
- [x] D3DCMP_* -> MTLCompareFunction conversion
- [x] Per-render-target Depth32Float texture cache
- [x] Drawable depth texture creation/caching
- [x] MTLDepthStencilState cache keyed by (zenable, zwriteenable, zfunc)
- [x] depthAttachmentPixelFormat on all PSO creation paths (geometry, translated, overlay)
- [x] Depth attachment on every render pass (Clear with clear_z on first use, Load on subsequent)
- [x] Per-draw [encoder setDepthStencilState:] binding
- [x] Static no-depth state for overlay draws (always pass, no write)
- [x] Depth/stencil state in frame dump
- [x] Contract test defaults updated with depth/stencil values
- [x] Clean build, backend contract + frontend surface-copy tests passing
- [x] FNV main menu still renders correctly (depth state transparent for 2D menu)

## Completed: Crash Hardening (2026-02-16)

- [x] DXT block-compressed copy guardrails:
  - reject invalid non-block-aligned rects
  - reject scaling for block-compressed copy paths
  - reject `ColorFill` on block-compressed surfaces
- [x] Added frontend Wine regression test coverage for DXT/linear surface copies (`frontend_surface_copy_test`)
- [x] Implemented per-device `D3DCREATE_MULTITHREADED` synchronization:
  - lock-aware wrappers for full `IDirect3DDevice9` vtbl dispatch
  - shared device guard on VB/IB/surface/texture lock/unlock hot paths

## Current Priority: RB5 -- In-Game Rendering

### Shader Translation Hardening
- [x] Hard-fail on malformed streams and unsupported flow-control opcodes
- [x] Hard-fail on unsupported relative addressing (a0 dynamic indexing)
- [x] Hard-fail on unsupported multi-texture declarations/usages beyond s0
- [x] Remove dead flow-control comment emission from MSL emitter
- [x] Graceful fallback to TSS/c0 path when shader translation fails (instead of skipping draw)
- [x] Flow control translation (if/ifc/else/endif, rep/endrep, break/breakc)
- [x] Multi-texture support in translated PS (tex0..tex7 bindings)
- [ ] Relative addressing (a0 register for dynamic constant indexing)
- [ ] VS/PS interface linkage validation (match output semantics to input semantics)

### Stability
- [ ] Re-verify problematic save-load path no longer crashes with dx9mt
- [ ] Expand lock coverage to all child COM vtbl methods if crash signature persists

### Render State Coverage
- [x] Cull mode (D3DRS_CULLMODE -> MTLCullMode)
- [ ] Fog state (D3DRS_FOGENABLE, fog color, fog mode)
- [ ] Fill mode (D3DRS_FILLMODE -> MTLTriangleFillMode)
- [ ] Stencil operations (D3DRS_STENCILPASS, STENCILFAIL, STENCILZFAIL)
- [ ] Blend-op fidelity (D3DRS_BLENDOP: ADD, SUBTRACT, REVSUBTRACT, MIN, MAX)
- [ ] Separate alpha blend support (D3DRS_SEPARATEALPHABLENDENABLE)

### Multi-texture
- [x] Texture stages 0-7 data transmission (array-based pipeline)
- [x] Multiple texture/sampler bindings in translated shaders
- [x] Per-stage texture upload in IPC bulk region
- [x] PS sampler index > 0 rejection removed from parser

## Later

- [ ] Wine unix lib integration (__wine_unix_call, zero-copy)
- [ ] Buffer ring allocator (replace per-draw newBufferWithBytes)
- [ ] PSO cache persistence (serialize to disk)
- [ ] Async shader compilation
- [ ] In-game rendering validation (gameplay, save/load)
- [ ] Skinned mesh support (blend weights/indices)
- [ ] Multiple render target (MRT) support
- [ ] Shadow pass rendering
