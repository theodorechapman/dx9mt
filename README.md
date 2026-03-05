# dx9mt

A Direct3D 9 to Metal translation layer for running Fallout: New Vegas on macOS via Wine. A Wine-side `d3d9.dll` captures D3D9 state and bulk uploads, a shared backend bridge serializes the frame into IPC, and a native Metal viewer replays the frame on Apple GPUs.

![Current in-game state on March 5, 2026](assets/images/current_gameplay_2026-03-05.png)

## Current State

As of March 5, 2026:

- Main menu rendering is stable.
- In-game 3D rendering is now working in the Metal viewer.
- Offscreen HDR render targets and `StretchRect` replay are working well enough for the world scene to reach the drawable.
- The frame is still visibly corrupted: large white/light-blue planar bands, exploding billboard or foliage geometry, and incomplete composite or post-process passes remain.
- The translated shader path is now the primary render path, with per-hash shader and PSO failure artifacts written into the session output directory.

## How It Works

```text
FalloutNV.exe (i686 / Wine)
        |
        v
+------------------------------------+
| d3d9.dll (PE32, MinGW)             |
| Implements IDirect3DDevice9        |
| Captures draws, clears, blits,     |
| shaders, textures, and state       |
+----------------+-------------------+
                 | packets + 3 x 256MB upload refs
                 v
+------------------------------------+
| Backend Bridge (compiled twice)    |
| PE32 inside the DLL, ARM64 inside  |
| libdx9mt_unixlib.dylib             |
| Validates packets and assembles    |
| shared-memory IPC on Present()     |
+----------------+-------------------+
                 | 256MB IPC file
                 v
+------------------------------------+
| Metal Viewer (native ARM64 macOS)  |
| Replays draws and StretchRect      |
| Translates D3D9 bytecode to MSL    |
| Renders via Metal into NSWindow    |
+------------------------------------+
```

The frontend DLL replaces the system `d3d9.dll` inside a Wine prefix. Each indexed draw snapshots the device state into a packet and uploads bulk geometry, constants, shader bytecode, declarations, and texture payloads into a rotating arena. The backend serializes the frame into `/tmp/dx9mt_metal_frame.bin`, and the standalone Metal viewer snapshots and replays that frame.

## Current Coverage

- Shader translation covers D3D9 shader model 1.x through 3.0 bytecode and caches compiled Metal functions by bytecode hash.
- Viewer replay covers indexed draws, `Clear`, offscreen render targets, `StretchRect`, depth and stencil state, blending, scissor, cull, fog parameters, and up to 8 texture stages.
- Supported sampled or render-target formats include DXT1, DXT3, DXT5, A8R8G8B8, X8R8G8B8, A8, R32F, and A16B16G16R16F.
- Render-target-to-texture linkage is tracked explicitly, so later passes can sample earlier offscreen output.
- Diagnostics emit `dx9mt_runtime.log`, `dx9mt_viewer.log`, `dx9mt_shader_fail_*.txt`, `dx9mt_pso_fail_*.txt`, and frame dumps in `dx9mt-output/session-*`.

## Current Problems

- Scene composition is not yet correct; the current gameplay image still contains large planar corruption across the screen.
- Some billboard, alpha-tested, or foliage-heavy draws are still exploding into spikes or dense point clouds.
- Not every translated VS/PS pair produces a valid Metal pipeline yet.
- Some draws still depend on textures that arrive with metadata but no upload payload, relying on cache or render-target override state the viewer may not have yet.
- Heavy frames can still hit upload-arena pressure or bulk-range validation failures.

## Building

Requires macOS (ARM64), Xcode command-line tools, and an `i686-w64-mingw32-gcc` cross-compiler.

```sh
# Build the DLL, backend dylib, viewer, and native tests
make -C dx9mt

# Run native contract tests
make test

# Build, install into the Wine prefix, and launch FNV plus the viewer
make run
```

## Build Outputs

| Artifact | Arch | Description |
|----------|------|-------------|
| `build/d3d9.dll` | PE32 (i686) | D3D9 frontend injected into Wine |
| `build/libdx9mt_unixlib.dylib` | ARM64 | Backend bridge library |
| `build/dx9mt_metal_viewer` | ARM64 | Standalone Metal frame replay app |

## Docs

- [`docs/architecture.md`](docs/architecture.md): current packet, IPC, and viewer architecture
- [`docs/rendering_findings.md`](docs/rendering_findings.md): current visual state and active bug buckets
- [`docs/insights.md`](docs/insights.md): implementation gotchas and debugging notes
- [`dx9mt/README.md`](dx9mt/README.md): inner build notes for the source tree

## License

MIT License
