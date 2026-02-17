# dx9mt Roadmap

Merged plan replacing `docs/todo.md` and `docs/next-steps.md`.

## Baseline

- Menu pipeline works.
- Save loading works.
- Gameplay entry works, but world rendering is not stable (flat blue viewport cases).
- Current blocker is frame-data integrity under high in-game draw/upload pressure.

## Priority 0: Stabilize Gameplay Frame Integrity

### Goal

Prevent dropped draw packets and missing constant uploads during heavy frames.

### Tasks

1. Add per-frame upload usage telemetry.
   - Log `slot_index`, bytes used, and largest contributors (VB/IB/constants/texture uploads).
2. Separate guaranteed payloads from optional payloads.
   - Constants and geometry refs must never fail silently.
   - Non-critical texture refresh uploads can be deprioritized first.
3. Introduce hard back-pressure or frame-local policy when near slot cap.
   - Avoid continuing draw emission after required refs are already failing.
4. Capture before/after metrics on the same save/load scene.
   - `draws`, overflow count, missing-constants count, visible output quality.

## Priority 1: Cursor And Menu UX

### Goal

Make cursor behavior deterministic in menu screens.

### Tasks

1. Implement explicit behavior for:
   - `SetCursorProperties`
   - `SetCursorPosition`
   - `ShowCursor`
2. Audit whether cursor draw path is hardware cursor, software cursor, or both in FNV traces.
3. Add sampled logs around cursor state transitions.

## Priority 2: In-Game Rendering Correctness

### Goal

Increase confidence that world-space draws are faithfully reproduced once frame integrity is stable.

### Tasks

1. Validate render-target routing for gameplay multipass sequences.
2. Compare translated-shader usage ratio against fallback path in gameplay.
3. Track skipped draws in viewer (texture unavailable, invalid bytecode, etc.).
4. Verify depth/stencil state usage on gameplay passes with targeted frame dumps.

## Priority 3: Performance Cleanup

### Goal

Reduce hitching and improve iteration speed.

### Tasks

1. Replace per-draw `newBufferWithBytes` churn with reusable buffer strategy.
2. Expand PSO reuse and consider on-disk cache persistence.
3. Keep shader compile failures sticky (already done) and reduce noisy retries.

## Backlog

- D3D9Ex (`Direct3DCreate9Ex`) support.
- Volume textures.
- Non-indexed draw path (`DrawPrimitive` + UP variants) for non-FNV workloads.
- Wine unixlib integration path (`libdx9mt_unixlib.dylib`) to reduce IPC overhead.

## Session Checklist (For Every Gameplay Regression Test)

1. `make clear && make run`
2. Reproduce menu -> load save -> in-game transition.
3. Collect:
   - `dx9mt-output/dx9mt_runtime.log`
   - `dx9mt-output/dx9mt_frame_dump.txt` (or numbered dumps)
4. Record:
   - first frame where viewport turns blue
   - overflow/missing-constants counts
   - draw count and clear color for that window
