# dx9mt

Incremental Direct3D9-to-Metal translation layer for Wine WoW64, currently focused on Fallout: New Vegas.

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

- `build/d3d9.dll` (PE32): D3D9 frontend, packet emitter, upload-arena producer, and IPC writer entrypoint.
- `build/libdx9mt_unixlib.dylib` (ARM64): backend bridge library compiled from the shared bridge sources.
- `build/dx9mt_metal_viewer` (ARM64): standalone Metal replay app that consumes shared-memory IPC.
- `build/backend_bridge_contract_test` (ARM64): native contract test binary for packet and IPC validation logic.

## Current Coverage

- Device lifecycle: `Direct3DCreate9`, `CreateDevice`, `Reset`, `BeginScene`, `EndScene`, `Present`.
- Draw path: `DrawIndexedPrimitive` packet emission with full per-draw state capture.
- Blit path: `StretchRect` packet emission and viewer-side replay.
- Resource objects: textures, cube textures, render targets, depth surfaces, vertex and index buffers, declarations, shaders, and queries.
- Shader translation: D3D9 shader model 1.x through 3.0 parse, MSL emission, Metal compile, and cache keyed by bytecode hash.
- Render-target replay: drawable plus offscreen render targets, including HDR `D3DFMT_A16B16G16R16F`.
- Multi-texture support: stages `0..7` in packet, IPC, and translated fragment binding.
- Render state coverage in the viewer: depth/stencil, cull, scissor, blend op/factors, color write mask, fog parameters, sampler state, and viewport routing.
- Diagnostics: runtime and viewer logs, frame dumps, shader failure artifacts, and PSO failure artifacts under `dx9mt-output/session-*`.

## Current State

- Main menu rendering is stable.
- In-game 3D rendering now reaches the Metal viewer and the world is visible.
- The frame is still corrupted by white or light-blue planar bands, broken billboard or foliage draws, and remaining composite mistakes.
- The viewer now depends on valid translated shader bytecode for most world draws, with a narrow compatibility fallback for a few known cases.

## Known Gaps

- `Direct3DCreate9Ex` and D3D9Ex remain unsupported (`D3DERR_NOTAVAILABLE`).
- `CreateVolumeTexture` remains unsupported (`D3DERR_NOTAVAILABLE`).
- `DrawPrimitive` is still a no-op stub because FNV relies on indexed draws.
- Several low-frequency `IDirect3DDevice9` methods still use placeholder behavior.
- Some translated VS/PS pairs still fail to compile or fail Metal PSO interface validation.
- Some draws still miss texture data when the frontend sends metadata without a fresh upload and the viewer cache is not seeded.
- Heavy frames can still exhaust the upload arena or fail later IPC bulk-range validation.
