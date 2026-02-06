# dx9mt Insights

Last updated: 2026-02-06
Source inputs: `research.md`, `frame3172-api-calls.txt`, `frame3172-unique-api-calls.txt`, live `/tmp/dx9mt_runtime.log`, `dxmt/`

## Bottleneck Thesis (Confirmed)
- The key issue is not raw draw count alone.
- The dominant cost comes from CPU overhead:
  - state-chatter translation around each draw
  - repeated WoW64-to-native crossings
  - redundant binding/pipeline work

## FNV Trace Facts Driving Implementation Order
- 34 unique API calls, 32,647 total records in sampled frame.
- Highest-frequency calls are state/constant churn, not just draws.
- `StretchRect` and `GetRenderTargetData` appear in-frame and are correctness-critical barriers.

## Architecture Principles (Still Valid)
- Front-end:
  - PE32 `d3d9.dll` COM implementation with shadow state.
  - Emits compact command packets keyed by IDs/snapshots.
- Backend:
  - ARM64 `unixlib` consumes packets and owns Metal objects/caches.
- Design rule:
  - avoid 1:1 forwarding of every `Set*` call across boundaries.

## Startup Probe Insights (Current Priority)
- FNV currently reaches `Direct3DCreate9`, then runs large capability probes before detaching.
- The dominant startup chatter is still probe-heavy:
  - `CheckDeviceMultiSampleType` dominates logs.
  - `CheckDeviceFormat` and `CheckDepthStencilMatch` are the next largest sets.
- Tightening capability semantics reduced log volume from ~24,570 to ~1,246 lines in a single run, which made failure pivots visible.
- Latest pre-patch pivot found in logs:
  - Last call before detach was `GetDeviceCaps adapter=0 type=2 -> NOTAVAILABLE`.
  - This led to adding a `D3DDEVTYPE_REF` fallback compatibility path.

## Key Practical Insight From This Iteration
- Silent-success defaults on D3D9 capability checks are dangerous.
- Over-advertising capabilities produces very large probe trees and can push apps into unsupported settings.
- Under-advertising certain legacy paths (for example `D3DDEVTYPE_REF` checks) can terminate startup before `CreateDevice`.
- Startup bring-up requires a narrow compatibility band:
  - honest enough to avoid bad mode selection
  - permissive enough to pass old launcher/game negotiation code

## Current Risk Register
- No observed `CreateDevice` call yet in FNV runtime path.
- D3D9Ex path is still missing (`Direct3DCreate9Ex`, `PresentEx`-related behavior).
- Texture family is incomplete (`CreateCubeTexture`, `CreateVolumeTexture` still return not available).
- Backend is still a stub, so current progress is startup negotiation + front-end correctness only.

## Validation Signals To Keep Watching
- Under per-app override (`FalloutNV.exe: d3d9=native,builtin`):
  - first appearance of `CreateDevice` in `/tmp/dx9mt_runtime.log`
  - last call before first `PROCESS_DETACH`
  - whether capability checks settle on a stable mode tuple instead of exiting

## Tooling Notes
- Analyzer script: `tools/analyze_dx9mt_log.py`
- Typical usage:
  - `uv run tools/analyze_dx9mt_log.py /tmp/dx9mt_runtime.log --top 20`
- The script now handles PID-tagged detach lines (`PROCESS_DETACH pid=...`) correctly.

## Decision Log
- 2026-02-06: Adopt PE32 front-end + ARM64 backend command-stream architecture.
- 2026-02-06: Completed M1 scaffold and M2 minimal correctness baseline.
- 2026-02-06: Prioritized frame-visible correctness gates (`StretchRect`, `GetRenderTargetData`) before performance phases.
- 2026-02-06: Added explicit unsupported returns for unimplemented texture families to avoid false-positive success behavior.
- 2026-02-06: Matched Wine `d3d9.dll` export ordinals via `dx9mt/src/frontend/d3d9.def`.
- 2026-02-06: Simplified operator workflow to `make run` + `make show-logs`.
- 2026-02-06: Added runtime log analyzer and moved tooling to `uv` project workflow.
- 2026-02-06: Shifted immediate focus to startup capability negotiation before device creation.
