# dx9mt Insights

Last updated: 2026-02-06  
Source inputs: `research.md`, `frame3172-api-calls.txt`, `frame3172-unique-api-calls.txt`, live `/tmp/dx9mt_runtime.log`, `dxmt/`

## Core Performance Thesis (Unchanged)
- The dominant cost target is CPU-side translation overhead:
  - high-frequency state churn
  - too many boundary crossings
  - redundant bind/pipeline work
- Not just raw draw count.

## Bring-Up Timeline (Most Important)
1. Early state:
   - `dx9mt` loaded but FNV exited before `CreateDevice`.
2. Capability bring-up phase:
   - Implemented non-trivial responses for `CheckDevice*` and `GetDeviceCaps`.
   - Call volume reduced enough to isolate pivots.
3. Launcher-path breakthrough:
   - Overrode launcher to also use `dx9mt`.
   - Exposed launcher-side failure at `CreateQuery` default stub.
4. `CreateQuery` fix:
   - Added minimal `IDirect3DQuery9`, launcher detect path stabilized.
5. Game-path breakthrough:
   - `FalloutNV.exe` now reaches `CreateDevice` and succeeds.
6. Current state:
   - Game now progresses past previous renderer-init failure into active frame loop.
   - Visible output is still black because backend present/render is currently no-op.

## High-Value Technical Insights
- Per-app override granularity matters:
  - overriding only `FalloutNV.exe` was insufficient for reliable launch behavior.
  - launcher and game both needed `d3d9=native,builtin` for consistent detection and handoff.
- Hard Wine restart before override application is required for reliable state:
  - stale wineserver state can mask or delay override changes.
- Missing-method failures are now moving later and are easier to isolate:
  - once `CreateQuery` was implemented, failure advanced to post-device renderer init.
- Current failure is likely contract/fidelity related, not loader/export related:
  - exports/ordinals and COM shape are now good enough for launcher and device creation.

## What the Logs Tell Us Now
- Confirmed in latest runs:
  - attach to `FalloutNVLauncher.exe`
  - attach to `FalloutNV.exe`
  - `CreateDevice` success on game process
  - sustained `DRAW_INDEXED` + `PRESENT` activity at runtime
- Backend confirms active frame loop with thousands of `DRAW_INDEXED` packets and repeated `Present`.
- Present is still backend `no-op`, so black-screen + live audio is an expected transitional state.
- Added sampled instrumentation for unsupported paths (`CreateCubeTexture`, `CreateVolumeTexture`, `DrawPrimitive`) to verify whether remaining API gaps are affecting startup visuals.
- Logs identified `CreateCubeTexture` as an active startup call path; minimal cube texture object support is now implemented.
- Latest verification shows `CreateCubeTexture` now returns `hr=0x00000000` in the game path.
- Latest verification reached at least frame `4080` with stable packet cadence (`~21` packets, `~19` draws, `1` clear per logged sample frame).
- No `CreateVolumeTexture unsupported` and no `DrawPrimitive stub` signal in the newest capture window.
- Probe/perf log noise is now throttled by default; full probe verbosity is opt-in via `DX9MT_TRACE_PROBES=1`.

## Current Hypothesis
- Primary blocker is no-op backend present/render path, not early init rejection.
- Secondary risk remains compatibility drift between reported caps and unimplemented resource APIs.

## Immediate Direction
- The next meaningful milestone is not another caps tweak; it is replacing backend `Present` no-op with a real visible output path.
- Backend implementation order is documented in `docs/rendering-backend-plan.md`.

## Practical Validation Pattern
- Always validate with:
  - `make run`
  - reproduce (`Detect`, then `Play`)
  - `make show-logs`
- Optional deep summary:
  - `uv run tools/analyze_dx9mt_log.py /tmp/dx9mt_runtime.log --top 20`
- Target query for progression:
  - `rg -n "PROCESS_ATTACH|CreateDevice|GetDeviceCaps|default stub:|PROCESS_DETACH" /tmp/dx9mt_runtime.log`
- For deep backend packet tracing only when needed:
  - `DX9MT_BACKEND_TRACE_PACKETS=1 make run`
- For full probe-call verbosity only when needed:
  - `DX9MT_TRACE_PROBES=1 make run`

## Risk Register
- `Direct3DCreate9Ex` path still unimplemented (`D3DERR_NOTAVAILABLE`).
- Texture family still incomplete (`CreateVolumeTexture` remains unsupported).
- Backend remains stub/no-op for real rendering work.
- Startup probe logs are now sampled by default; deep probes should be enabled only for targeted runs.

## Decision Log
- 2026-02-06: Adopted PE32 front-end + ARM64 backend bridge architecture.
- 2026-02-06: Matched Wine ordinal/export compatibility for `d3d9.dll`.
- 2026-02-06: Standardized operator workflow around `make run` + `make show-logs`.
- 2026-02-06: Added wineserver restart guard before override-sensitive operations.
- 2026-02-06: Implemented launcher-critical `CreateQuery` path.
- 2026-02-06: Reached first successful `CreateDevice` in `FalloutNV.exe` path.
- 2026-02-06: Reached sustained in-game draw/present loop with black-screen output (expected while backend is no-op).
- 2026-02-06: Implemented minimal `CreateCubeTexture` support after logs showed startup dependence.
