# dx9mt Status

Last updated: 2026-02-06
Owner: project-wide (update every working session)

## Objective
Ship a high-performance direct `d3d9.dll` replacement for 32-bit DX9 games under Wine WoW64 on Apple Silicon, with Fallout New Vegas as the first target.

## Scope
- In scope:
  - PE32 D3D9 front-end (`d3d9.dll`) with COM ABI compatibility for FNV-first API paths.
  - ARM64 native backend bridge (`unixlib`) that owns Metal work submission.
  - Command-stream architecture that minimizes WoW64/native crossing overhead.
- Out of scope for initial milestones:
  - Replacing Wine CPU emulation strategy.
  - Generic non-Wine runtime support.
  - Broad 64-bit DX9 support (optional later).

## Baseline
- Environment:
  - `Wine Staging.app`, fresh `wineprefix`, FNV installed and currently running via WineD3D.
  - `dxmt/` available as D3D10/11->Metal reference.
- Captured frame data:
  - `frame3172-api-calls.txt`: 32,647 lines.
  - `frame3172-unique-api-calls.txt`: 34 unique calls.
  - Hot calls: `DrawIndexedPrimitive` 1411, `SetVertexShaderConstantF` 3947, `SetPixelShaderConstantF` 2574, `SetSamplerState` 2184, `SetTexture` 1869, `SetIndices` 1406, `SetStreamSource` 1406, `SetRenderState` 1092.

## Milestones
- `M0` Docs + baseline capture interpretation: completed.
- `M1` Buildable skeleton: completed.
  - PE32 `d3d9.dll` + native backend bridge.
  - Shared packet scaffolding and logging.
  - Wine override launch path.
- `M2` Minimal correctness (main menu class APIs): completed.
  - `Direct3DCreate9`, `CreateDevice`, `Clear`, `BeginScene`, `EndScene`, `Present`, basic `DrawIndexedPrimitive`.
  - Functional COM objects for swapchain/surface/VB/IB/declaration/shaders.
- `M3` Real game startup coverage: in progress.
  - Implemented this session:
    - Wine-compatible `d3d9.dll` export name/ordinal map via `dx9mt/src/frontend/d3d9.def`.
    - Missing compatibility exports (`DebugSetLevel`, `Direct3DCreate9On12`, `PSGPError`, `PSGPSampleTexture`).
    - Process-tagged runtime logging (`PROCESS_ATTACH/DETACH` include PID + exe path).
    - Conservative capability responses for startup probing (`CheckDeviceType`, `CheckDeviceFormat`, `CheckDepthStencilMatch`, `CheckDeviceMultiSampleType`, `GetDeviceCaps`, `EnumAdapterModes`).
    - `D3DDEVTYPE_REF` fallback support (treated as supported startup path, pending validation in retest).
    - Log analysis tool: `tools/analyze_dx9mt_log.py` (run via `uv`).
  - Pending:
    - First successful `CreateDevice` in FNV runtime path.
    - Full texture family coverage (`CreateCubeTexture`, `CreateVolumeTexture`).
    - D3D9Ex path (`Direct3DCreate9Ex`/`CreateDeviceEx`).
- `M4` Performance architecture: pending.
  - State snapshot draws, argument-buffer cache, async pipeline compilation, binary archive cache.
- `M5` Advanced optimization and hardening: pending.
  - Optional ICB path, stronger hazard analysis, long-session stability/regression coverage.

## Current Work Package (Step 1)
1. Stabilize startup capability negotiation so FNV reaches `CreateDevice`.
2. Keep 64-bit DX stack at Wine defaults while overriding only 32-bit FNV `d3d9`.
3. Use concise runtime logs and analyzer output to drive compatibility patches.

## Operational Workflow
- `make run`
  - Builds and installs `dx9mt/build/d3d9.dll` into the FNV directory.
  - Enforces override policy: only `FalloutNV.exe` gets `d3d9=native,builtin`.
  - Starts wineserver, starts Steam, launches `FalloutNVLauncher.exe`.
  - Logs:
    - `/tmp/dx9mt_runtime.log`
    - `/tmp/fnv_dx9mt_probe.log`
    - `/tmp/steam_probe.log`
- `make show-logs`
  - Prints runtime log and filtered launcher/steam signals.
- `uv run tools/analyze_dx9mt_log.py /tmp/dx9mt_runtime.log --top 20`
  - Summarizes duplicate calls and startup sweep patterns.

## Latest Signals
- Override/load path confirmed:
  - `dx9mt` attaches inside `FalloutNV.exe` and `Direct3DCreate9` is called.
- Runtime log volume improved:
  - Before capability tightening: ~24,570 parsed `dx9mt` lines.
  - After tightening: ~1,246 parsed lines.
- Current startup blocker:
  - No observed `CreateDevice` call yet.
  - Last call before detach in latest analyzed run: `GetDeviceCaps adapter=0 type=2 -> NOTAVAILABLE` (captured before REF fallback patch was added).

## Blockers
- FNV process still exits before `CreateDevice` in observed startup runs.
- Backend is still a no-op packet consumer; no real Metal rendering yet.
- D3D9Ex interfaces are still unavailable (`Direct3DCreate9Ex` returns `D3DERR_NOTAVAILABLE`).

## Next 1-3 Actions
1. Retest after `D3DDEVTYPE_REF` fallback patch and capture fresh `make show-logs` + analyzer output.
2. If still no `CreateDevice`, relax/adjust startup format acceptance based on last failing check sequence.
3. Once `CreateDevice` appears, gate next work on first failing device/resource API.
