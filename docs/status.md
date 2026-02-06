# dx9mt Status

Last updated: 2026-02-06  
Owner: project-wide (update every working session)

## Objective
Ship a high-performance direct `d3d9.dll` replacement for 32-bit DX9 games under Wine WoW64 on Apple Silicon, with Fallout New Vegas (FNV) as first target.

## Scope
- In scope:
  - PE32 D3D9 front-end (`d3d9.dll`) with COM ABI compatibility.
  - ARM64 native backend bridge (`unixlib`) and packet path.
  - FNV-first compatibility and performance bring-up.
- Out of scope for initial milestones:
  - Generic non-Wine runtime support.
  - Broad DX9Ex/64-bit game support.
  - Replacing Wine CPU emulation strategy.

## Environment Baseline
- `Wine Staging.app` + fresh `wineprefix`.
- Steam + FNV installed under `C:\Games\Steam\...`.
- `dxmt/` present as D3D10/11->Metal reference.
- Frame capture inputs:
  - `frame3172-api-calls.txt` (32,647 lines)
  - `frame3172-unique-api-calls.txt` (34 unique calls)

## Milestone Status
- `M0` Docs + baseline data interpretation: completed.
- `M1` Buildable scaffold (`d3d9.dll` + backend bridge + packet/log plumbing): completed.
- `M2` Minimal runtime object correctness (device/swapchain/surface/VB/IB/shaders): completed.
- `M3` FNV startup and launch compatibility: in progress.
  - `M3a` DLL load and launcher compatibility: completed.
  - `M3b` Game post-`CreateDevice` renderer init: in progress.
- `M4` Performance architecture and batching: pending.
- `M5` Hardening and long-session validation: pending.

## What Was Implemented (Current Session Window)
- Wine-compatible `d3d9.dll` exports/ordinals (`d3d9.def`) and missing compatibility exports.
- Runtime attach/detach logging with PID + exe + command line.
- Startup capability-check implementation for:
  - `CheckDeviceType`
  - `CheckDeviceFormat`
  - `CheckDepthStencilMatch`
  - `CheckDeviceMultiSampleType`
  - `GetDeviceCaps`
  - `EnumAdapterModes`
- Per-app override flow updated so both executables use dx9mt:
  - `FalloutNVLauncher.exe`
  - `FalloutNV.exe`
- Make workflow simplified and stabilized:
  - `make run`
  - `make show-logs`
  - `wine-restart` hook before override-sensitive targets.
- `CreateQuery` implemented (minimal `IDirect3DQuery9` COM object) to unblock launcher device detection.
- `GetDeviceCaps` expanded to a more realistic HAL profile for Gamebryo expectations.
- Added `DX9MT_PACKET_CLEAR` emission from `IDirect3DDevice9::Clear`.
- Backend stub logging changed from per-packet spam to per-frame summaries (`packets/draws/clears/last_clear`) with optional packet trace via `DX9MT_BACKEND_TRACE_PACKETS=1`.
- Probe/perf log throttling to keep runtime logs readable by default:
  - set `DX9MT_TRACE_PROBES=1` to restore full probe verbosity.
  - `DebugSetMute` now logs sampled counts instead of every call.
  - capability-probe failure loops now sampled (not logged every time).
- Added targeted runtime instrumentation for unimplemented/high-risk paths:
  - `CreateCubeTexture` call-path log (sampled).
  - `CreateVolumeTexture` unsupported log (sampled).
  - `DrawPrimitive` stub-use log (sampled).
- Implemented minimal `IDirect3DCubeTexture9` object + `CreateCubeTexture` success path for common startup usage.
- Added/kept analyzer tooling:
  - `tools/analyze_dx9mt_log.py`
  - `uv` project setup (`pyproject.toml`, `uv.lock`).
- Backend contract hardening for rendering bring-up (`RB0`) landed:
  - monotonic packet sequencing via `dx9mt_runtime_next_packet_sequence()`.
  - present-target metadata publishing on device create/reset (`target/window/size/format/windowed`).
  - swapchain present delegated to device present to keep one packet sequencing path.
  - `DRAW_INDEXED` packets now include draw-critical IDs/hashes (RT/DS/VB/IB/decl/shaders/FVF/stream0/viewport/scissor).
  - backend parser validates draw packet size and required state IDs.
- Automated contract tests added:
  - `dx9mt/tests/backend_bridge_contract_test.c`
  - top-level `make test` / `make -C dx9mt test-native` pass.
- RB1 bootstrap paths added (optional):
  - `DX9MT_BACKEND_SOFT_PRESENT=1` enables backend-side window clear/marker present using present-target window metadata.
  - `DX9MT_FRONTEND_SOFT_PRESENT=1` enables a software present path that blits backbuffer sysmem to the Wine window via GDI.
  - `Clear(D3DCLEAR_TARGET)` now updates RT0 sysmem, and present overlays a deterministic frame marker for visual confirmation.
  - these are temporary validation paths; real Metal/backend replay remains the long-term target.

## Current Verified Runtime State
- Launcher path:
  - `dx9mt` loads into `FalloutNVLauncher.exe`.
  - Device detection now works (no immediate crash from previous `CreateQuery` stub).
- Game path:
  - `dx9mt` loads into `FalloutNV.exe`.
  - `Direct3DCreate9` called.
  - `CreateDevice` succeeds.
  - `GetDeviceCaps` succeeds.
  - `CreateCubeTexture` now succeeds in the game startup path (`hr=0x00000000`).
  - Game progresses past previous `failed to initialize gamebryo renderer` error.
  - Runtime reaches active frame loop with large `DRAW_INDEXED` + `PRESENT` traffic.
  - Latest manual validation sustained through at least frame `2760` with stable present target metadata:
    - `present target updated: target=33554433 size=1280x720 fmt=22 windowed=1`
    - repeated `present frame=... target=33554433 size=1280x720 fmt=22 (no-op)`
  - Latest manual validation had no contract errors:
    - no `packet parse error`
    - no `packet sequence out of order`
    - no `draw packet missing state ids`
    - no `draw packet too small`
  - No `CreateVolumeTexture unsupported` or `DrawPrimitive stub` signal in latest runtime slices.
  - Automated contract validation currently passes (`backend_bridge_contract_test: PASS` via `make test`).
  - User-observed behavior in bootstrap mode: top-left marker/texture changes color frame-to-frame (expected).
  - Without bootstrap mode: black screen with active audio/input cues.

## Current Blocker
- Backend present/render path is still intentionally `no-op`; black screen is currently expected.
- Current issue is no longer early launcher/device init failure; it is missing real backend rendering.
- `RB0` bridge/packet guardrails are in place and validated.
- Next blocker to clear is `RB1`: first visible backend present path (real backend path, not frontend bootstrap).
- Remaining compatibility gaps still exist (`CreateVolumeTexture` and other unimplemented methods), with instrumentation to confirm call impact.

## Backend Plan
- Detailed backend bring-up plan is tracked in:
  - `docs/rendering-backend-plan.md`

## Operational Workflow
- `make run`
  - Restarts wineserver first.
  - Reapplies per-app overrides.
  - Installs `dx9mt/build/d3d9.dll` into FNV directory.
  - Starts Steam, then launcher.
  - Writes logs to:
    - `/tmp/dx9mt_runtime.log`
    - `/tmp/fnv_dx9mt_probe.log`
    - `/tmp/steam_probe.log`
- `make show-logs`
  - Dumps runtime log and filtered launcher/steam signals.
- `uv run tools/analyze_dx9mt_log.py /tmp/dx9mt_runtime.log --top 20`
  - Summarizes call frequencies and probe sweep duplication.

## Step 1 Definition (Updated)
Step 1 is complete only when FNV progresses past Gamebryo renderer initialization into stable in-game rendering path under `dx9mt`.

## Next 1-3 Actions
1. Implement `RB1` first-visible present path (replace backend present no-op with a deterministic clear/output path).
2. Start `RB2` frame-state replay scaffolding using the new draw packet identity fields and monotonic sequence contract.
3. Keep the validation loop strict on every change:
   - `make test`
   - `make run`
   - `rg -n "present target updated|present frame=.*target=|draw packet missing state ids|draw packet too small|packet parse error|packet sequence out of order" /tmp/dx9mt_runtime.log`
   - optional visible bootstrap checks:
     - `DX9MT_BACKEND_SOFT_PRESENT=1 make run`
     - `DX9MT_FRONTEND_SOFT_PRESENT=1 make run`
