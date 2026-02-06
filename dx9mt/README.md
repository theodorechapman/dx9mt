# dx9mt

Incremental Direct3D9-to-Metal translation layer for Wine WoW64 (FNV-first bring-up).

## Outputs
- `build/d3d9.dll` (PE32): D3D9 front-end COM implementation + packet bridge calls.
- `build/libdx9mt_unixlib.dylib` (native): backend bridge stub (packet logging/no-op replay for now).

## Build
```sh
make -C dx9mt
```

## Clean
```sh
make -C dx9mt clean
```

## Current Coverage
- Device lifecycle: `Direct3DCreate9`, `CreateDevice`, `Reset`, `BeginScene`, `EndScene`, `Present`.
- Draw path: `DrawIndexedPrimitive` packet emission.
- Core state/storage: render states, sampler/texture stage state, stream/index bindings, shaders/constants.
- Resource objects: surface, swapchain, vertex/index buffer, vertex declaration, vertex/pixel shader.
- Copy/readback path: `UpdateSurface`, `UpdateTexture`, `StretchRect`, `GetRenderTargetData`, `ColorFill`.
- Textures: minimal `IDirect3DTexture9` from `CreateTexture`.

## Known Gaps
- `Direct3DCreate9Ex` / D3D9Ex device paths are not implemented.
- `CreateCubeTexture` and `CreateVolumeTexture` currently return `D3DERR_NOTAVAILABLE`.
- Native backend still does not execute real Metal rendering work.
