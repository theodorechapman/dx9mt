# dx9mt

Incremental Direct3D9-to-Metal translation layer for Wine WoW64 (FNV-first bring-up).

## Build

```sh
make -C dx9mt
```

## Test

```sh
make -C dx9mt test-native
```

## Clean

```sh
make -C dx9mt clean
```

## Outputs

- `build/d3d9.dll` (PE32): D3D9 frontend + packet bridge + IPC writer.
- `build/libdx9mt_unixlib.dylib` (native): backend bridge library (not yet active in Wine runtime path).
- `build/dx9mt_metal_viewer` (native): standalone Metal viewer that consumes shared-memory IPC.

## Current Coverage

- Device lifecycle: `Direct3DCreate9`, `CreateDevice`, `Reset`, `BeginScene`, `EndScene`, `Present`.
- Draw path: `DrawIndexedPrimitive` packet emission with per-draw state capture.
- Resource objects: textures, cube textures, render targets, depth surfaces, VB/IB, declarations, shaders, queries.
- Copy/readback path: `UpdateSurface`, `UpdateTexture`, `StretchRect`, `GetRenderTargetData`, `ColorFill`.
- Texture formats: DXT1/DXT3/DXT5, A8R8G8B8/X8R8G8B8/A8.
- Shader translation: D3D9 SM2/SM3 bytecode parse + MSL emit + Metal compile/cache.
  - includes flow control and relative constant indexing support.
- Multi-texture support: stages `0..7` in packet/IPC path and translated shader binding.
- Render state coverage in viewer: depth/stencil, cull, scissor, blend op/factors, color write mask, fog parameters.
- IPC: 256MB shared memory file with draw entries + bulk VB/IB/texture/constants/shader data.

## Known Gaps

- `Direct3DCreate9Ex` / D3D9Ex path remains `D3DERR_NOTAVAILABLE`.
- `CreateVolumeTexture` remains unsupported (`D3DERR_NOTAVAILABLE`).
- `DrawPrimitive` is still a no-op stub (FNV relies on indexed draws).
- Several low-frequency `IDirect3DDevice9` methods still use default stub behavior.
- Runtime can hit upload-arena overflow on heavy in-game frames, causing dropped draws due missing required upload refs.
