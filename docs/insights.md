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
   - Game still fails with `failed to initialize gamebryo renderer` after `CreateDevice` and `GetDeviceCaps`.

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
  - process still detaches shortly after initialization
- No obvious `default stub:` log line in the immediate failing window of the newest run.
- This shifts focus to:
  - capability fidelity (`D3DCAPS9` fields/flags)
  - behavior of methods used right after device creation (even if implemented).

## Current Hypothesis
- Gamebryo renderer init is rejecting the device contract after creation.
- Most likely causes:
  - one or more critical `D3DCAPS9` bits/limits are missing or unrealistic
  - post-create method behavior mismatch during renderer bootstrap

## Practical Validation Pattern
- Always validate with:
  - `make run`
  - reproduce (`Detect`, then `Play`)
  - `make show-logs`
- Optional deep summary:
  - `uv run tools/analyze_dx9mt_log.py /tmp/dx9mt_runtime.log --top 20`
- Target query for progression:
  - `rg -n "PROCESS_ATTACH|CreateDevice|GetDeviceCaps|default stub:|PROCESS_DETACH" /tmp/dx9mt_runtime.log`

## Risk Register
- `Direct3DCreate9Ex` path still unimplemented (`D3DERR_NOTAVAILABLE`).
- Texture family still incomplete (`CreateCubeTexture`, `CreateVolumeTexture` currently return not available).
- Backend remains stub/no-op for real rendering work.
- Startup probe logs are still large; precision logging is required to keep debugging efficient.

## Decision Log
- 2026-02-06: Adopted PE32 front-end + ARM64 backend bridge architecture.
- 2026-02-06: Matched Wine ordinal/export compatibility for `d3d9.dll`.
- 2026-02-06: Standardized operator workflow around `make run` + `make show-logs`.
- 2026-02-06: Added wineserver restart guard before override-sensitive operations.
- 2026-02-06: Implemented launcher-critical `CreateQuery` path.
- 2026-02-06: Reached first successful `CreateDevice` in `FalloutNV.exe` path.
- 2026-02-06: Shifted immediate target to post-`CreateDevice` Gamebryo renderer initialization.
