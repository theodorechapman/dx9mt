# dx9mt

Incremental Direct3D9-to-Metal translation layer for Wine WoW64 (FNV-first bring-up).

## Outputs
- `build/d3d9.dll` (PE32): D3D9 front-end COM implementation + packet bridge + IPC writer.
- `build/libdx9mt_unixlib.dylib` (native): backend bridge stub (for future Wine unix lib integration).
- `build/dx9mt_metal_viewer` (native): standalone Metal renderer reading IPC shared memory.

## Build
```sh
make -C dx9mt
```

## Test
```sh
make -C dx9mt test-native    # 10 contract tests
```

## Clean
```sh
make -C dx9mt clean
```

## Current Coverage
- Device lifecycle: `Direct3DCreate9`, `CreateDevice`, `Reset`, `BeginScene`, `EndScene`, `Present`.
- Draw path: `DrawIndexedPrimitive` packet emission with full state capture.
- Core state/storage: render states, sampler/texture stage state, stream/index bindings, shaders/constants.
- Resource objects: surface, swapchain, vertex/index buffer, vertex declaration, vertex/pixel shader.
- Copy/readback path: `UpdateSurface`, `UpdateTexture`, `StretchRect`, `GetRenderTargetData`, `ColorFill`.
- Textures: `IDirect3DTexture9` with Lock/Unlock, generation tracking, DXT/ARGB format support.
- IPC: 16MB shared memory file with per-draw entries, bulk VB/IB/texture/constant/shader data.
- Metal rendering: vertex declarations, indexed draws, WVP transform, texture sampling, alpha blending.
- Shader translation: D3D9 SM2.0/SM3.0 bytecode -> MSL source transpiler with caching.
- Depth testing: per-RT Depth32Float textures, MTLDepthStencilState cache, per-draw binding.

## Known Gaps
- `Direct3DCreate9Ex` / D3D9Ex device paths are not implemented.
- `CreateCubeTexture` and `CreateVolumeTexture` currently return `D3DERR_NOTAVAILABLE`.
- Stencil operations not yet consumed (state transmitted but not applied).
- Flow control in shaders (if/else, loops) emitted as comments, not yet functional.
- Multi-texture stages 1-7 not yet transmitted.
- Native backend dylib not yet loaded by Wine (IPC approach used instead).
