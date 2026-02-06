# Metal Graphics API Programming Guide (dx9mt-Focused)

Last updated: 2026-02-06  
Audience: engine and backend engineers building or porting renderers to Metal

## 1. Scope
This document is a practical guide for implementing a production-grade Metal rendering backend, with a direct focus on `dx9mt` needs (D3D9-style frontend, packetized backend execution, Wine + Apple Silicon target).

It combines:
- Current `dx9mt` implementation realities.
- Official Apple Metal programming model guidance.
- Current Metal 4 direction (command model, resource model, compilation model, ML integration).

## 2. Source Baseline (Primary Sources)
The guidance here is grounded in:
- Metal resources hub and official spec links:
  - https://developer.apple.com/metal/resources/
- Metal feature table PDF (published October 20, 2025):
  - https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf
- Metal “What’s new” page (Metal 4 overview):
  - https://developer.apple.com/metal/whats-new/
- WWDC25 video transcripts:
  - Discover Metal 4: https://developer.apple.com/videos/play/wwdc2025/205/
  - Explore Metal 4 games: https://developer.apple.com/videos/play/wwdc2025/254/
- Apple docs and archive references for core architecture and best practices:
  - Command organization: https://developer.apple.com/library/archive/documentation/Miscellaneous/Conceptual/MetalProgrammingGuide/Cmd-Submiss/Cmd-Submiss.html
  - Resource objects: https://developer.apple.com/library/archive/documentation/Miscellaneous/Conceptual/MetalProgrammingGuide/Mem-Obj/Mem-Obj.html
  - Resource heaps: https://developer.apple.com/library/archive/documentation/Miscellaneous/Conceptual/MetalProgrammingGuide/ResourceHeaps/ResourceHeaps.html
  - Best practices (persistent objects, command buffers, triple buffering, drawables, load/store actions, render encoder merging):
    - https://developer.apple.com/library/archive/documentation/3DDrawing/Conceptual/MTLBestPracticesGuide/PersistentObjects.html
    - https://developer.apple.com/library/archive/documentation/3DDrawing/Conceptual/MTLBestPracticesGuide/CommandBuffers.html
    - https://developer.apple.com/library/archive/documentation/3DDrawing/Conceptual/MTLBestPracticesGuide/TripleBuffering.html
    - https://developer.apple.com/library/archive/documentation/3DDrawing/Conceptual/MTLBestPracticesGuide/Drawables.html
    - https://developer.apple.com/library/archive/documentation/3DDrawing/Conceptual/MTLBestPracticesGuide/LoadandStoreActions.html
    - https://developer.apple.com/library/archive/documentation/3DDrawing/Conceptual/MTLBestPracticesGuide/RenderCommandEncoders.html

Notes:
- Archive docs are older but still useful for core mental models and best practices.
- Metal 4 behavior and APIs should be validated against the latest live API docs when implementing concrete codepaths.

## 3. Core Metal Mental Model
### 3.1 Device and Submission Hierarchy
At baseline (Metal 3-era model):
- `MTLDevice` represents one GPU.
- `MTLCommandQueue` defines ordered execution of submitted command buffers.
- `MTLCommandBuffer` is a single-use unit of GPU work.
- Encoders append commands into a command buffer:
  - render
  - compute
  - blit

Key invariants from Apple docs:
- Command buffers on one queue execute in enqueue/commit order.
- A command buffer is transient and non-reusable after commit.
- Only one encoder is active on a command buffer at a time (except parallel render encoder model).

### 3.2 Render Pass-Centric Design
Metal is attachment/pass explicit:
- You define attachments (color/depth/stencil) per pass.
- Load/store actions are explicit and materially affect bandwidth and performance.
- Draws execute in one encoder/pass context with explicit fixed-function and pipeline state binding.

### 3.3 CPU Overhead Is a First-Class Constraint
Apple best-practice guidance remains stable:
- Persist expensive objects (`MTLDevice`, command queues, pipeline states).
- Minimize command buffer count per frame (often one or two).
- Triple-buffer dynamic data to avoid CPU/GPU hazards.
- Keep drawable ownership window short.

## 4. Feature and Compatibility Strategy
### 4.1 Never Assume Capabilities by Marketing Name
Use runtime checks (`supportsFamily`, specific support properties) and gate features dynamically.

### 4.2 What Feature Tables Show (2025 snapshot)
The Metal Feature Set Tables PDF confirms broad limits and feature rollout by family, including:
- 8 color render targets per pass descriptor.
- Argument buffers tiering.
- Mesh shading, ray tracing, sparse resources, dynamic libraries, binary archives, and newer Metal 4-era capabilities by family.

Design implication:
- Build renderer paths around capability matrices, not one fixed path.

### 4.3 Metal 4 Direction (Adoption Signal)
WWDC25 and “What’s new” indicate:
- New command model objects (`MTL4*`) for lower-overhead explicit control.
- Command allocators and decoupled queue/buffer model.
- Argument tables + residency sets for large-scale binding.
- Low-overhead barrier model for stage-to-stage sync.
- Dedicated compilation contexts (`MTL4Compiler`) and faster/smarter compilation workflows.
- Placement sparse resources and texture view pools for large content streaming.

For `dx9mt`, this means:
- Build a Metal 3-compatible path first.
- Keep architecture ready for Metal 4 migration where CPU overhead or resource scale dominates.

## 5. Practical Frame Architecture
### 5.1 Baseline Frame Loop
1. Acquire or reuse per-frame CPU-side staging state.
2. Build command buffer.
3. Encode passes (render/compute/blit) in dependency order.
4. Acquire drawable as late as practical.
5. Encode final pass to drawable texture.
6. Call `present` on command buffer before `commit`.
7. Commit.

### 5.2 Pseudocode Skeleton
```cpp
id<MTLCommandBuffer> cb = [queue commandBuffer];

// Offscreen pass
id<MTLRenderCommandEncoder> r0 = [cb renderCommandEncoderWithDescriptor:gBufferPass];
[r0 setRenderPipelineState:gBufferPSO];
// bind resources, draw...
[r0 endEncoding];

// Optional compute post
id<MTLComputeCommandEncoder> c0 = [cb computeCommandEncoder];
[c0 setComputePipelineState:postPSO];
// bind resources, dispatch...
[c0 endEncoding];

// Final present pass
id<CAMetalDrawable> drawable = [layer nextDrawable];
id<MTLRenderCommandEncoder> r1 = [cb renderCommandEncoderWithDescriptor:presentPass(drawable)];
[r1 setRenderPipelineState:presentPSO];
// bind + draw fullscreen
[r1 endEncoding];

[cb presentDrawable:drawable];
[cb commit];
```

### 5.3 Load/Store Action Rules
Use simple, strict defaults:
- `DontCare` load when full overwrite is guaranteed.
- `Clear` when partial writes but previous data irrelevant.
- `Load` only when prior pass data must be preserved.
- `Store` only when data is needed later (another pass, readback, or presentation chain).

Incorrect load/store choices silently burn bandwidth and can dominate frame time.

## 6. Resource Management and Memory
### 6.1 Core Resource Types
- Buffers: typeless data allocations.
- Textures: formatted image allocations.
- Texture views: reinterpret same memory with compatible format/view layout.

### 6.2 Heaps and Aliasing
Use heaps when you need:
- predictable memory budgeting,
- reduced allocation churn,
- transient resource reuse.

Apple guidance worth carrying directly:
- Separate heaps by render target type when required (color/depth/stencil/MSAA considerations).
- Separate aliasable and non-aliasable usage patterns.
- Avoid overly fine-grained fencing.

### 6.3 Sparse and Placement Sparse Resources
Sparse resources are for streaming-scale workloads:
- decouple resource object lifetime from physical backing.
- map/unmap pages/tiles as needed.

Metal 4 extends this with placement sparse buffers/textures and queue-integrated mapping synchronization, which is directly relevant for very large scenes or texture streaming.

### 6.4 Residency Discipline
For large resource sets:
- group resources into residency sets.
- attach mostly-static sets at queue scope.
- update dynamic subsets on producer/streaming threads.

This is especially relevant if `dx9mt` evolves from D3D9-era asset pressure into modern upscaling/RT-assisted paths.

## 7. Binding Model Strategy
### 7.1 Classic Slot Binding (Good for Bring-Up)
Initial backend can stay on familiar per-draw slot binding:
- set buffers/textures/samplers directly each draw.
- simpler to validate correctness quickly.

### 7.2 Argument Buffers / Bindless Evolution
As draw count and resource count grow:
- move to argument buffers (tier-gated).
- use indexed descriptors for scene-scale resource access.

### 7.3 Metal 4 Argument Tables
Metal 4 introduces argument tables as explicit bind-point containers that can be prepared earlier and shared across encoder stages. This can reduce hot-path CPU work for highly dynamic scenes.

## 8. Shader Compilation and Pipeline Lifecycle
### 8.1 Compilation Stages
Canonical pipeline path:
- MSL -> Metal IR -> GPU binaries.

### 8.2 Best Practice Defaults
- Compile libraries at build time whenever possible.
- Minimize runtime source compilation.
- Centralize and cache pipeline state creation.

### 8.3 Variant Control
Use function constants / specialization to avoid exploding unique pipeline bodies where possible.

### 8.4 Metal 4 Compilation Flow
WWDC25 indicates:
- dedicated compiler object (`MTL4Compiler`),
- explicit compilation scheduling/QoS behavior,
- flexible render pipeline states:
  - create unspecialized pipeline,
  - specialize cheaply for attachment/color state variants,
  - reduce redundant compile cost.

### 8.5 Binary and Offline Workflows
For porting pipelines at scale:
- use binary archive / offline compile workflows where available.
- consider Metal shader converter workflows for HLSL/DXIL-heavy migrations.

## 9. Synchronization Model
### 9.1 Order Guarantees
- Queue order gives coarse sequencing.
- Pass/encoder boundaries define execution structure.

### 9.2 Hazard Control
Legacy/standard approaches:
- fences/events for explicit cross-encoder or cross-queue dependencies.
- careful command ordering.

Metal 4 direction:
- barrier APIs for stage-to-stage synchronization at lower overhead.
- queue-level barriers for scalable dependency control.

### 9.3 Principle for Backend Design
Start conservative:
- over-synchronize for correctness in RB1/RB2.
Then remove sync gradually with profiling, not assumptions.

## 10. Performance Patterns That Matter
### 10.1 High-Impact CPU Optimizations
- persistent objects.
- reduced encoder/pass count.
- minimized draw-time rebinding.
- command allocator reuse (Metal 4 path).

### 10.2 High-Impact GPU/Bandwidth Optimizations
- strict load/store tuning.
- fewer render-target transitions.
- attachment merge opportunities.
- efficient transient resource lifetimes.

### 10.3 Submission Pacing
- one to two command buffers per frame is a common stable target.
- triple-buffer dynamic data for parallel CPU/GPU progress.

### 10.4 Indirect Work Encoding
Indirect command buffers can amortize repeated command setup and reduce CPU cost on repeated structures (HUD, repeated mesh groups, predictable pass chunks).

## 11. Debugging and Tooling Workflow
Use a fixed tool stack from day one:
- API Validation + Shader Validation (early correctness catches).
- Xcode GPU Frame Capture/Debugger (state and attachment verification).
- Metal System Trace (CPU/GPU overlap, queue starvation, sync stalls).
- Metal performance HUD (runtime sanity checks).

For each regression:
1. Reproduce with a minimal frame capture.
2. Confirm pass outputs and attachment lifetime.
3. Verify pipeline/shader variant selected matches expectation.
4. Verify synchronization edge.
5. Re-profile after fix.

## 12. D3D9 -> Metal Mapping for dx9mt
### 12.1 Concept Mapping
- D3D9 device state machine -> backend-managed Metal state cache.
- `DrawIndexedPrimitive` -> indexed draw call in current render encoder.
- `Clear` -> pass clear load actions when possible, fallback explicit clear draw for complex partial cases.
- `Present` -> drawable presentation and frame submission boundary.

### 12.2 Immediate Semantic Gaps to Plan For
- D3D9 fixed-function remnants and state mutation frequency.
- FVF/declaration to Metal vertex descriptor translation.
- D3D9 blend/depth/stencil/cull semantics mapping.
- sampler and texture stage behavior not directly equivalent to modern shader-centric pipelines.
- coordinate/system conventions and precision edge cases.

### 12.3 Packet Contract Requirements (Current dx9mt Status)
`dx9mt` now has a stronger replay contract baseline for `DRAW_INDEXED`:
- draw packets include primitive/index range plus draw-critical replay identity:
  - `render_target_id`, `depth_stencil_id`
  - `vertex_buffer_id`, `index_buffer_id`
  - `vertex_decl_id`, `vertex_shader_id`, `pixel_shader_id`
  - `fvf`, stream0 offset/stride
  - viewport/scissor hashes and state-block hash
- packet sequence values are monotonic per packet (runtime-global counter), not frame-derived.
- backend parser enforces packet ordering and draw packet shape/required IDs.

Remaining payload work for full-fidelity replay:
- texture/sampler stage identity and state deltas.
- shader constant payload reference population (ABI fields exist, but data plumbing is still pending).
- additional stream bindings beyond stream0 where required by scene content.

## 13. Backend Bring-Up Blueprint (Actionable)
### Phase A: First Visible Correctness
- Keep one queue, one command buffer, one render pass to drawable.
- Implement clear color presentation path from packet `CLEAR`.
- Add frame debug overlay (draw count / state hash) to prove packet consumption.

### Phase B: Deterministic Replay
- Introduce backend frame-state object built from packet stream.
- Encode strict pass order and state application order.
- Add deterministic state hash checks to catch divergence.

### Phase C: Real Geometry
- Translate minimal D3D9 vertex/index/shader path into Metal pipeline state + draw encoding.
- Add robust fallback/logging for unsupported combinations.

### Phase D: Throughput
- Add state dedupe, pipeline cache, descriptor/argument-table batching.
- Move repeated draws toward indirect encoding when profitable.

### Phase E: Scale Features
- Heaps/sparse/placement sparse where memory pressure demands.
- Optional Metal 4 adoption for command allocators, barriers, and high-scale resource binding.

## 14. dx9mt-Specific Implementation Notes (From Current Source Audit)
Observed in current source tree:
- Frontend emits packets for `CLEAR`, `DRAW_INDEXED`, and `PRESENT`.
- `DRAW_INDEXED` packet payload now includes replay-critical object/state IDs and hashes.
- present-target metadata is explicitly published to backend (`target_id`, dimensions, format, windowed).
- packet sequencing is monotonic per packet via runtime counter.
- swapchain present delegates to device present, keeping one present packet path.
- Backend bridge parses packets and reports sampled per-frame stats, but does not render.
- backend parser enforces contract checks (packet parse/sequence + draw packet size/state IDs).
- `BeginScene` triggers backend `begin_frame`; `Present` submits present packet and increments frame id.
- `DrawPrimitive` remains a stub (returns `D3D_OK` but no backend geometry packet).
- `CreateVolumeTexture` returns `D3DERR_NOTAVAILABLE`; `CreateCubeTexture` has minimal object support.
- Significant D3D9 state is still tracked in frontend object state but not fully replayable by backend yet (notably texture-stage/sampler state and full shader-constant payload plumbing).

Consequence:
- The contract baseline is now largely in place; next backend milestone is implementing visible present/replay execution on top of that contract.

## 15. Cross-Project Lessons from dxmt (DX10/11 -> Metal)
### 15.1 Wine Split Architecture Pattern
`dxmt` uses a clear split:
- Windows DLL side initializes unix calls in `DllMain` and stays thin.
- Thunk layer marshals calls through `WINE_UNIX_CALL`.
- Native unixlib side performs Objective-C/Metal work.

For `dx9mt`, this pattern is directly applicable: keep `d3d9.dll` focused on D3D9 semantics and push Metal execution into a backend-facing service boundary.

### 15.2 32-bit Safety Pattern (Critical for dx9mt + Wine)
`dxmt` explicitly models pointer transport for `__i386__`:
- pointer wrappers carry split fields and keep fixed size.
- inaccessible pointer scenarios are handled explicitly (`get_accessible_or_null` behavior).
- multiple `__i386__` paths use CPU-placed staging allocations for safe access.

Inference from `dxmt` build/runtime layout:
- 32-bit application-facing code can still depend on 64-bit unix execution paths, so bridge ABI design must be pointer-width agnostic.

Direct `dx9mt` implication:
- packet/bridge ABI should use fixed-width scalar fields (`u32/u64` handles, offsets, sizes).
- never serialize raw host pointers as replay contract state.

### 15.3 Command Chunk and Async Submission Pattern
`dxmt` command flow is chunk-based:
- command chunks carry IDs/event IDs/frame IDs.
- dedicated encode + finish threads decouple submission from completion handling.
- per-chunk allocators and fences coordinate CPU/GPU lifetime.

`dx9mt` takeaway:
- separate packet recording, command encoding, and completion processing even if initial bring-up keeps one-thread execution.

### 15.4 Pass Fusion and Dependency-Guided Reordering Pattern
`dxmt`’s encoder flush path performs dependency-aware optimization:
- clear-pass fusion into compatible render passes by rewriting load actions.
- render-pass merge when attachment signatures and dependency checks allow it.
- explicit data-dependency checks across buffer/texture read/write sets.

`dx9mt` takeaway:
- once replay correctness is stable, add a conservative pass-signature + dependency pass before command emission.

### 15.5 Residency Tracking Pattern
`dxmt` tracks per-resource residency masks by encoder ID and only escalates resource usage when required.

`dx9mt` takeaway:
- even without mesh/tessellation complexity, read/write stage masks for VB/IB/RT/texture usage can reduce redundant resource usage calls and encoder overhead.

### 15.6 Presenter Isolation Pattern
`dxmt` isolates present logic behind a presenter abstraction handling:
- layer property sync,
- color space/HDR metadata decisions,
- present pipeline variant rebuilds.

`dx9mt` takeaway:
- keep present pipeline logic separate from draw replay logic so swapchain/presentation growth does not destabilize packet replay.

### 15.7 Runtime Controls and Caching Pattern
`dxmt` exposes practical toggles (capture, validation, frame pacing, shader cache path/enable) and uses persistent cache storage for shader artifacts.

`dx9mt` takeaway:
- define equivalent runtime controls early so backend validation and performance triage can be done without code edits.

## 16. Implementation Checklists
### 16.1 Correctness Checklist
- Render target dimensions/format are explicit at backend present time.
- Clear, viewport, scissor, depth/stencil state flow into pass descriptors or encoder state.
- All draw-critical bindings are available by packet/state snapshot.
- Frame ordering is deterministic and testable.

### 16.2 Performance Checklist
- Pipeline states cached by stable key.
- Per-draw rebinding minimized.
- Command buffer count/frame justified by profiling.
- Load/store actions audited for every attachment.

### 16.3 Feature Gating Checklist
- Runtime `supportsFamily` checks centralized.
- Optional paths gated cleanly (mesh, RT, sparse, advanced binding).
- Fallbacks preserve correctness first, then quality.

## 17. Suggested Next Reading Order
1. Command model and pass model:
   - Command organization guide.
2. Resource model and heaps:
   - Resource objects + resource heaps docs.
3. Performance discipline:
   - Best-practices sections (persistent objects, load/store actions, command buffers, triple buffering).
4. Capability planning:
   - Metal Feature Set Tables PDF.
5. Metal 4 migration:
   - WWDC25 “Discover Metal 4” and “Explore Metal 4 games”.
6. Shader porting at scale:
   - Metal shader converter docs and toolchain references.

## 18. Bottom Line for dx9mt
- Metal API fundamentals are not the blocker anymore.
- The blocker is backend-visible state completeness and deterministic replay contract.
- Solve packet/state contract first, then leverage Metal pass/pipeline architecture cleanly.
- Adopt Metal 4 incrementally only where it clearly reduces measured overhead or unlocks a needed capability.
