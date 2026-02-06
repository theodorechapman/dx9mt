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
- Packet contract baseline from `RB0` is now active:
  - monotonic per-packet sequence values.
  - explicit present-target metadata (`target/size/format/windowed`).
  - draw-critical object/state IDs in `DRAW_INDEXED`.
- Backend currently logs frame summaries only and does not render.
- Optional bootstrap path exists for visual validation while backend remains no-op:
  - `DX9MT_BACKEND_SOFT_PRESENT=1` enables backend-side window clear/marker present using target window metadata.
  - `DX9MT_FRONTEND_SOFT_PRESENT=1` enables frontend GDI blit + deterministic debug marker overlay.
  - useful for confirming frame progress in-window under Wine while RB1 real replay path is still incomplete.
- Latest observed sample frames (`~120` interval logs) are consistent:
  - `packets=21`
  - `draws=19`
  - `clears=1`

## Implementation Findings From Source Audit (2026-02-06)
- `BeginScene` is the current frame-open hook (`dx9mt_backend_bridge_begin_frame(self->frame_id)`).
- `Present` emits `DX9MT_PACKET_PRESENT`, calls backend present, then increments `frame_id`.
- `SwapChain::Present` delegates to `Device::Present`, so present sequencing is unified in one code path.
- `Clear` and `DrawIndexedPrimitive` emit packets with monotonic sequence via `dx9mt_runtime_next_packet_sequence()`.
- `DRAW_INDEXED` now includes replay-critical IDs/hashes:
  - RT/DS/VB/IB/vertex-decl/shader IDs.
  - FVF, stream0 offset/stride, viewport/scissor hashes, state-block hash.
- Backend stub only parses/counts packets and logs sampled frame summaries; it does not execute rendering.
- Backend parser now rejects malformed/incomplete packet streams:
  - parse-size/tail mismatch.
  - non-monotonic packet sequence.
  - draw packets missing required state IDs or too small payloads.
- Front-end still tracks additional device state (render/sampler/texture-stage state, stream bindings beyond stream0, constant payload refs) that is not fully replayed yet.
- Object IDs are kind-tagged (`kind << 24 | serial`), improving replay identity and log diagnostics.
- `DrawPrimitive` is still a stub (`D3D_OK`, sampled log only) and not represented in backend work.
- `CreateCubeTexture` now has minimal object support; `CreateVolumeTexture` remains `D3DERR_NOTAVAILABLE`.

## Metal-Oriented Execution Guidance (Research Sync)
- Treat RB1 as a real render-pass milestone, not only a log/visualization milestone:
  - one queue + one command buffer + one final pass to present target.
  - clear and present behavior must map directly to packet values.
- Keep feature gating explicit from day one (`supportsFamily`/capability checks); avoid backend assumptions tied to one GPU family.
- Make load/store actions explicit and conservative first (`clear`/`store` as needed), then optimize.
- Establish pipeline/state object caching early to avoid recreating expensive objects on draw hot paths.
- Keep no-op fallback behind a feature flag for rollback.

## dxmt-Derived Architecture Guidance (2026-02-06)
- Keep a strict Wine split:
  - thin Windows DLL/thunk side for API entry and marshaling.
  - backend Metal execution behind unix-side calls.
- Treat 32-bit compatibility as a hard contract requirement:
  - avoid raw pointer transport in backend packet/state contracts.
  - use fixed-width handle/offset/size fields (`u32`/`u64`) and validate accessibility assumptions explicitly.
- Inference from `dxmt` runtime/build layout:
  - wow64-style interop means 32-bit app-facing code can rely on a 64-bit unix execution side, so ABI stability matters more than native pointer convenience.
- Separate command lifecycle stages:
  - record packet/frame model,
  - encode GPU work,
  - process completion/fences and reclaim transient allocations.
- Plan for pass optimization once correctness is stable:
  - clear-pass fusion,
  - render-pass merge when signatures match and no resource dependency violation exists.
- Add explicit resource-usage/residency tracking in replay layer to reduce redundant resource usage calls.
- Isolate present path concerns (layer state, color space/HDR metadata, scaling) behind one presenter boundary so replay logic stays stable.

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

### RB0 - Contract Cleanup And Guardrails (Completed 2026-02-06)
- Purpose:
  - prepare bridge API and runtime metadata for non-no-op present.
- Changes:
  - fixed-width bridge metadata contract in use for present target updates.
  - explicit present-target metadata path wired through device create/reset.
  - strict packet-contract baseline implemented:
    - monotonic per-packet sequence counter.
    - explicit draw-critical object/state identity payloads.
    - stable object handle namespace for VB/IB/texture/surface/shader identity.
  - backend-side contract validation logs implemented (missing target metadata, out-of-order sequence, malformed/incomplete draw packets).
  - per-frame sampled stats remain active.
- Touchpoints:
  - `dx9mt/include/dx9mt/backend_bridge.h`
  - `dx9mt/include/dx9mt/packets.h`
  - `dx9mt/src/frontend/d3d9_device.c`
  - `dx9mt/src/frontend/runtime.c`
  - `dx9mt/src/backend/backend_bridge_stub.c`
  - `dx9mt/tests/backend_bridge_contract_test.c`
- Acceptance:
  - `make test` passes (`backend_bridge_contract_test: PASS`).
  - runtime logs show present-target metadata received (`present target updated: target=33554433 ...`).
  - latest manual validation shows no active:
    - `draw packet missing state ids`
    - `draw packet too small`
    - `packet parse error`
    - `packet sequence out of order`

### RB1 - First Visible Present Path
- Purpose:
  - replace "present no-op" with a minimal visible output path.
- Strategy:
  - introduce a dedicated presenter layer in backend code to own present-target metadata, drawable acquisition timing, and final-pass encoding.
  - render a deterministic first-pass output driven by latest packet state:
    - clear color fill
    - simple draw-count indicator overlay
  - prefer real backend pass wiring (not purely software emulation) so later RB2/RB3 work reuses the same submission/present scaffolding.
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
  - store clear state and ordered draw commands for the frame with explicit state snapshots.
  - split backend replay flow into explicit stages:
    - record packet state for frame N,
    - encode frame N command work,
    - process completion/fence + transient allocation reclamation.
  - support synchronous execution first, but keep data model compatible with future chunked/async encoding.
  - enforce canonical packet ordering checks (`begin_frame -> ... -> present`) and report violations.
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
  - state capture for currently bound VB/IB, declaration/FVF, shaders/constants, render target/depth target, viewport/scissor, and key render states.
- Changes:
  - add minimal backend draw path with conservative assumptions.
  - support only the subset currently observed in FNV startup loop first.
  - define and enforce primitive-type and index-format mapping rules in one place.
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
  - add conservative pass optimizer rules:
    - fuse compatible clear operations into pass load actions.
    - merge compatible adjacent render passes only when attachment signature and dependency checks are satisfied.
- Acceptance:
  - scene stability improves (less flicker/wrong ordering).
  - packet-to-frame behavior remains deterministic.

### RB5 - Performance Pass
- Purpose:
  - remove avoidable CPU overhead while preserving RB4 correctness.
- Changes:
  - state dedupe and command batching at replay layer.
  - add resource-usage residency tracking to suppress redundant usage/binding emission.
  - adopt ring-style transient allocator strategy for staging and per-frame command data.
  - reduced per-draw binding churn.
  - pipeline/shader cache scaffolding and asynchronous compile warmup.
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

## Work Breakdown For RB1/RB2 (Immediate)
1. Implement RB1 presenter boundary in backend and replace `present ... (no-op)` with first visible deterministic output path.
2. Keep rollback/fallback control explicit so startup stability can be compared quickly across runs.
3. Build RB2 frame-state recording in backend (`submit_packets` -> frame snapshot -> present consume/reset).
4. Extend replay payload only where needed for first geometry pass:
   - texture/sampler stage identity.
   - shader constant payload references (currently ABI fields exist but are not populated).
   - additional stream bindings beyond stream0 where required by observed scenes.
5. Expand native tests alongside RB1/RB2:
   - keep contract test coverage in `make test`.
   - add focused frame-order/replay-state tests as backend replay logic grows.
6. Retest loop:
   - `make test`
   - `make run`
   - `make show-logs`
   - `rg -n "present target updated|present frame=.*target=|draw packet missing state ids|draw packet too small|packet parse error|packet sequence out of order|no-op" /tmp/dx9mt_runtime.log`

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
- Risk:
  - capability report/implementation drift (caps indicate support that packet/backend path cannot execute correctly).
- Mitigation:
  - tie capability claims to backend readiness gates and keep unsupported paths explicit in sampled logs.
