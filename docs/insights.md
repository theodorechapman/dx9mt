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
6. Contract hardening phase (`RB0`):
   - moved packet sequencing to runtime-global monotonic counter.
   - added explicit present-target metadata bridge update (`target/size/format/windowed`).
   - expanded `DRAW_INDEXED` packet contract with draw-critical object/state IDs and hashes.
   - added backend-side draw packet size/state-ID validation and native contract tests.
7. Current state:
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
- Current rendering failure is now mostly backend-output related, not loader/export related:
  - exports/ordinals and COM shape are now good enough for launcher and device creation.
- Kind-tagged object IDs make log interpretation and contract checks clearer:
  - example: `target=33554433` (`0x02000001`) encodes swapchain object kind (`0x02`) plus serial.

## What the Logs Tell Us Now
- Confirmed in latest runs:
  - attach to `FalloutNVLauncher.exe`
  - attach to `FalloutNV.exe`
  - `CreateDevice` success on game process
  - sustained `DRAW_INDEXED` + `PRESENT` activity at runtime
- Backend confirms active frame loop with thousands of `DRAW_INDEXED` packets and repeated `Present`.
- Present is still backend `no-op`, so black-screen + live audio is an expected transitional state.
- Latest manual grep slice confirms present-target metadata path is active:
  - `present target updated: target=33554433 size=1280x720 fmt=22 windowed=1`
  - repeated `present frame=... target=33554433 ... (no-op)` through at least frame `2760`.
- Latest manual grep slice confirms no active contract-parse failures:
  - no `draw packet missing state ids`
  - no `draw packet too small`
  - no `packet parse error`
  - no `packet sequence out of order`
- Bootstrap present options now exist for visual validation while full backend replay is still no-op:
  - backend side: `DX9MT_BACKEND_SOFT_PRESENT=1`
  - frontend side: `DX9MT_FRONTEND_SOFT_PRESENT=1`
- Manual validation observed top-left marker color changes frame-to-frame, consistent with bootstrap present behavior.
- Added sampled instrumentation for unsupported paths (`CreateCubeTexture`, `CreateVolumeTexture`, `DrawPrimitive`) to verify whether remaining API gaps are affecting startup visuals.
- Logs identified `CreateCubeTexture` as an active startup call path; minimal cube texture object support is now implemented.
- Latest verification shows `CreateCubeTexture` now returns `hr=0x00000000` in the game path.
- Latest verification reached at least frame `2760` with stable sampled packet cadence.
- No `CreateVolumeTexture unsupported` and no `DrawPrimitive stub` signal in the newest capture window.
- Probe/perf log noise is now throttled by default; full probe verbosity is opt-in via `DX9MT_TRACE_PROBES=1`.
- Native contract test target currently passes via `make test` (`backend_bridge_contract_test: PASS`).

## Current Hypothesis
- Primary blocker is no-op backend present/render path, not early init rejection.
- `RB0` packet/bridge contract baseline is now in place and validated.
- Secondary risk remains compatibility drift between reported caps and unimplemented resource APIs.

## Immediate Direction
- The next meaningful milestone is not another caps tweak; it is `RB1` first-visible backend present.
- Immediately after first visible output, move into `RB2` deterministic frame-state replay while preserving current packet-contract guardrails.
- Backend implementation order is documented in `docs/rendering-backend-plan.md`.

## Practical Validation Pattern
- Always validate with:
  - `make test`
  - `make run`
  - reproduce (`Detect`, then `Play`)
  - `make show-logs`
- Optional visible-bootstrap run:
  - `DX9MT_BACKEND_SOFT_PRESENT=1 make run`
  - `DX9MT_FRONTEND_SOFT_PRESENT=1 make run`
- Optional deep summary:
  - `uv run tools/analyze_dx9mt_log.py /tmp/dx9mt_runtime.log --top 20`
- Target query for progression:
  - `rg -n "PROCESS_ATTACH|CreateDevice|GetDeviceCaps|default stub:|PROCESS_DETACH" /tmp/dx9mt_runtime.log`
- Target query for backend contract health:
  - `rg -n "present target updated|present frame=.*target=|draw packet missing state ids|draw packet too small|packet parse error|packet sequence out of order" /tmp/dx9mt_runtime.log`
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
- 2026-02-06: Implemented monotonic packet sequencing and explicit present-target metadata publish/update path.
- 2026-02-06: Expanded draw packet contract with replay-critical object/state IDs and hashes.
- 2026-02-06: Added native backend contract tests (`make test`) and validated parser/order/state-ID guardrails.
- 2026-02-06: Manual runtime validation confirmed stable present-target metadata (`target=33554433`) and no active parser/sequence/draw-state contract errors.
