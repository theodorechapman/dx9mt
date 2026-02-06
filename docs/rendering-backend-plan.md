# dx9mt Rendering Backend Plan

Last updated: 2026-02-06  
Owner: backend bring-up

## Goal
Move from backend `Present` no-op to visible, packet-driven rendering while preserving the now-stable FNV startup and game-loop behavior.

## Current Starting Point
- Front-end is stable enough to enter long-running frame loops in `FalloutNV.exe`.
- Runtime packets already include:
  - `CLEAR`
  - `DRAW_INDEXED`
  - `PRESENT`
- Backend currently logs frame summaries only and does not render.
- Latest observed sample frames (`~120` interval logs) are consistent:
  - `packets=21`
  - `draws=19`
  - `clears=1`

## Success Criteria
- `RB1` success:
  - visible window output changes with frame updates (not black no-op).
  - no regression in launcher/game startup.
- `RB2` success:
  - clear color and frame activity map to visible output from packet data.
- `RB3` success:
  - first geometry path rendered from `DRAW_INDEXED` using tracked device state.
- `RB4+` success:
  - progressively higher fidelity with stable frame pacing.

## Implementation Phases

### RB0 - Contract Cleanup And Guardrails
- Purpose:
  - prepare bridge API and runtime metadata for non-no-op present.
- Changes:
  - define explicit present-target metadata path (window target, dimensions, format) from front-end to backend.
  - keep no-op fallback behind a feature flag for safe rollback.
  - ensure per-frame stats still log at sampled intervals.
- Touchpoints:
  - `dx9mt/include/dx9mt/backend_bridge.h`
  - `dx9mt/src/frontend/d3d9_device.c`
  - `dx9mt/src/backend/backend_bridge_stub.c`
- Acceptance:
  - build passes.
  - logs show present-target metadata received and updated on reset.

### RB1 - First Visible Present Path
- Purpose:
  - replace "present no-op" with a minimal visible output path.
- Strategy:
  - render a deterministic frame visualization driven by latest packet state:
    - clear color fill
    - simple draw-count indicator overlay
  - this can be a temporary software/backend debug path, but it must be visible and frame-driven.
- Touchpoints:
  - `dx9mt/src/backend/backend_bridge_stub.c`
  - `dx9mt/src/frontend/d3d9_device.c` (target metadata plumbing)
- Acceptance:
  - user sees non-black output that updates per frame.
  - runtime logs no longer include `present ... (no-op)`.

### RB2 - Packet Replay Frame Model
- Purpose:
  - move from ad-hoc debug visualization to deterministic per-frame packet replay state.
- Changes:
  - build a frame-state object populated by `submit_packets`.
  - store clear state and ordered draw commands for the frame.
  - present consumes frame-state snapshot and resets for next frame.
- Touchpoints:
  - `dx9mt/src/backend/backend_bridge_stub.c`
  - `dx9mt/include/dx9mt/packets.h` (only if extra packet fields are needed)
- Acceptance:
  - packet counts in logs match replayed command counts.
  - clear and draw activity visibly correlate with log summaries.

### RB3 - Minimal Geometry Pipeline
- Purpose:
  - render first real primitives from `DRAW_INDEXED`.
- Prerequisites:
  - state capture for currently bound VB/IB, declaration/FVF, shaders, and key render states.
- Changes:
  - add minimal backend draw path with conservative assumptions.
  - support only the subset currently observed in FNV startup loop first.
  - keep unsupported state combinations explicit in logs.
- Touchpoints:
  - `dx9mt/src/frontend/d3d9_device.c` (packet payload completeness)
  - backend renderer files (new files expected as backend grows)
- Acceptance:
  - visible geometry appears in-game path.
  - no crash regression.

### RB4 - State Fidelity And Pass Structure
- Purpose:
  - increase correctness without exploding complexity.
- Changes:
  - implement strict ordering for:
    - render target/depth target handling
    - viewport/scissor
    - depth/stencil and blend essentials
  - introduce pass breaks only when required.
- Acceptance:
  - scene stability improves (less flicker/wrong ordering).
  - packet-to-frame behavior remains deterministic.

### RB5 - Performance Pass
- Purpose:
  - remove avoidable CPU overhead while preserving RB4 correctness.
- Changes:
  - state dedupe and command batching at replay layer.
  - reduced per-draw binding churn.
  - pipeline/shader cache scaffolding.
- Acceptance:
  - lower CPU time in backend replay.
  - frame pacing improves under same test scene.

### RB6 - Compatibility Hardening
- Purpose:
  - close remaining known API gaps encountered during real gameplay.
- Priority gaps:
  - `CreateVolumeTexture` support if it appears in logs.
  - any new startup/runtime stubs surfaced by sampled instrumentation.
- Acceptance:
  - extended gameplay session without new fatal API rejects.

## Work Breakdown For RB0/RB1 (Immediate)
1. Add present-target metadata bridge contract.
2. Populate metadata on `CreateDevice` and `Reset`.
3. Implement visible present fallback in backend.
4. Keep `DX9MT_BACKEND_TRACE_PACKETS` support unchanged.
5. Retest with:
   - `make run`
   - `make show-logs`
   - `make analyze-logs`
   - `rg -n "present frame|CreateDevice|PROCESS_ATTACH|PROCESS_DETACH" /tmp/dx9mt_runtime.log`

## Definition Of Done For "Begin Rendering Backend"
- Backend is no longer present no-op.
- Output is visibly updated each frame from packet data.
- FNV still launches through Steam and reaches active runtime loop.
- Docs (`status.md`, `insights.md`) updated with results and next milestone.

## Risks And Mitigations
- Risk:
  - breaking currently stable startup path while changing present contract.
- Mitigation:
  - feature-flag new present path and preserve old no-op fallback.
- Risk:
  - backend complexity grows before first visible win.
- Mitigation:
  - force RB1 visible-output milestone before deeper fidelity work.
- Risk:
  - noisy logs hide regressions.
- Mitigation:
  - keep sampled logging defaults; enable deep trace only when targeted.
