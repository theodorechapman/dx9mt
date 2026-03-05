# Rendering Findings

Current evidence from the in-game blank-world investigation.

## Symptom

- Loading into the game shows a light-blue/clear-colored scene.
- HUD elements render correctly on top.
- The process can remain stable for a long time with no crash or immediate runtime failure.

## Primary Root Cause Identified So Far

The dominant failure is offscreen render-target replay, not `Present()`.

- Viewer cohort logs show most world draws targeting `rt=83886199` with
  `fmt=113 (0x71)`.
- `113` is `D3DFMT_A16B16G16R16F`.
- Before support for this format existed in the viewer, these targets were
  rejected as unsupported, producing hundreds to more than a thousand skipped
  draws per frame.
- This matches the visible symptom:
  - scene clear reaches the drawable
  - HUD reaches the drawable
  - world scene, rendered through HDR offscreen targets, does not
- The viewer now maps `D3DFMT_A16B16G16R16F` to `MTLPixelFormatRGBA16Float`
  and keys translated PSOs by the actual render-target pixel format.
  The next run should show how much blank-world behavior remains once HDR RT
  replay is no longer blocked at format creation time.

Representative viewer diagnostics:

```text
dx9mt_metal_viewer: ERROR: frame 1777 diagnostics: translated=57 skipped=1213 missing_target_texture=1206 ...
dx9mt_metal_viewer: INFO: frame 1777 cohort[0] count=604 rt=83886199 fmt=113(0x00000071) ...
```

Representative format error:

```text
dx9mt_metal_viewer: ERROR: render target format unsupported rt_id=83886199 size=800x450 fmt_dec=113 fmt_hex=0x00000071 fmt_name=?
```

## Secondary Issues Still Present

These are real bugs, but they are not the first-order explanation for the blank
world scene:

- Shader translation failures
  - Example hashes:
    - `VS 0xdf77824a`
    - `PS 0x6ec1c3f7`
    - `PS 0xe881b8fb`
- Translated PSO interface mismatches
  - Example:
    - `Vertex attribute v0(0) is missing from the vertex descriptor`
    - `Fragment input(s) user(attr0) mismatching vertex shader output type(s) or not written by vertex shader`
- Texture cache misses with `upload_size == 0`
  - Some textures arrive only as metadata references, relying on the viewer to
    already have them cached.
  - If the viewer has not seen them before, the draw fails.
- Upload-arena overflow on later heavy frames
  - This causes missing constant uploads and rejected draw packets.
  - It happens later and is not the initial blank-world cause.

## Diagnostics Added

Frontend/runtime:

- `dx9mt/rttrace`
  - `CreateTexture`
  - `CreateRenderTarget`
  - `CreateDepthStencilSurface`
  - `SetRenderTarget`
  - `GetRenderTarget`
  - `SetDepthStencilSurface`
  - sampled `Present()` summaries
- `dx9mt/texdiag`
  - unsupported type
  - missing metadata
  - missing level surface
  - no sysmem copy
  - zero upload size
  - not dirty / not in refresh window
  - upload copy failure

Viewer:

- `dx9mt_viewer.log`
  - unsupported RT formats
  - RT materialization success/failure
  - RT-to-texture override linkage
  - texture resolution source: `rt_override`, `cache`, `upload`, `cache_miss`
  - per-frame draw skip summaries
  - per-frame cohort summaries
- `dx9mt_shader_fail_vs_<hash>.txt`
- `dx9mt_shader_fail_ps_<hash>.txt`
- `dx9mt_pso_fail_<key>.txt`

## Diagnostics Crash

A viewer crash was introduced by the investigation tooling:

- The cohort logger hashed shader bytecode using IPC offsets without validating
  the bulk-data range first.
- That caused `EXC_BAD_ACCESS` in `dx9mt_cohort_add -> dx9mt_sm_bytecode_hash`.
- This was a diagnostics-path bug and has been fixed by validating IPC bulk
  ranges before hashing bytecode.

## Current Priorities

1. Support `D3DFMT_A16B16G16R16F` in the viewer render-target path.
2. Re-run and see how much of the blank-world symptom remains.
3. Then fix the remaining translated shader / PSO interface failures.
4. Then address missing texture cache seeding and later upload-arena overflow.
