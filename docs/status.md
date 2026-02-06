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
- Added/kept analyzer tooling:
  - `tools/analyze_dx9mt_log.py`
  - `uv` project setup (`pyproject.toml`, `uv.lock`).

## Current Verified Runtime State
- Launcher path:
  - `dx9mt` loads into `FalloutNVLauncher.exe`.
  - Device detection now works (no immediate crash from previous `CreateQuery` stub).
- Game path:
  - `dx9mt` loads into `FalloutNV.exe`.
  - `Direct3DCreate9` called.
  - `CreateDevice` succeeds.
  - `GetDeviceCaps` succeeds.
  - Game still exits with user-visible error: `failed to initialize gamebryo renderer`.

## Current Blocker
- Renderer initialization fails after successful `CreateDevice`.
- This is now a post-device initialization compatibility issue (not an early load/export/override failure).
- No obvious remaining "default stub" call appears in the latest captured path around the failure point.

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
1. Retest with the expanded caps patch now in tree and capture fresh logs.
2. Instrument first failure window immediately after `CreateDevice`/`GetDeviceCaps` to identify the next rejected contract.
3. Patch the next concrete incompatibility (caps bit, method behavior, or init-time object contract), then retest.
