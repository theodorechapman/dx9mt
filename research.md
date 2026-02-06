Below is a concrete, “you could actually build this” architectural plan for a **Direct3D 9 → Metal** drop‑in `d3d9.dll` designed specifically for **Apple Silicon macOS** and **32‑bit DX9 games (Fallout: New Vegas first)** running under **Wine WoW64**.

The main performance thesis is:

* The *killer* overhead in your current chain is not “1000 draws is too many” (1000 draws/frame is normal for DX9-era engines), it’s that **each draw (and the many state calls around it) becomes a pile of translated work** through **DXVK → Vulkan → MoltenVK → Metal**, plus **WoW64/emulation boundary costs**, plus extra validation and sync.
* A *fast* solution looks less like “call Metal once per D3D call” and more like what real drivers do: **front-end records a compact command stream**, **back-end replays it efficiently**, **state changes are deduped**, and **resource binding is amortized** using Metal features like **argument buffers** (bind lots of resources with fewer CPU calls) ([Apple Developer][1]) and (optionally) **indirect command buffers** ([Apple Developer][2]).

---

## 0) Ground rules and key constraints

### Target runtime environment

* **32-bit Windows game** (FNV) under **Wine WoW64** on **macOS ARM64**.
* You want a replacement **`d3d9.dll`** that ultimately submits **Metal** work.

Wine has a well-established mechanism for “Windows PE DLL + native Unix sidecar library” (a Wine builtin is “a Win32 dll encapsulated inside a Unix library”) ([Arch Manual Pages][3]), and modern WoW64 support allows 32‑bit PE modules to call 64‑bit Unix libraries via thunks/dispatch mechanisms ([Hacker News][4]).

### Reality check: D3DMetal doesn’t save you for DX9

On mac gaming stacks today, **DX9 commonly falls back to DXVK→MoltenVK→Metal** because Apple’s D3DMetal is aimed at newer DirectX (DX11/12) in practice, so DX9 is exactly the gap you’re filling ([A journey into a wild pointer][5]).

---

## 1) The performance architecture you actually want

If you build a naïve D3D9→Metal wrapper where *every* COM call crosses into native Metal code, you’ll still drown in CPU overhead because D3D9 is extremely chatty (lots of SetRenderState / SetTexture / SetSamplerState between draws).

### The “driver-like” architecture

**Split into two layers inside `d3d9.dll`:**

1. **Front-end (32-bit PE, runs where the game runs)**

* Implements all D3D9 COM interfaces (`IDirect3D9`, `IDirect3DDevice9`, resources, shaders…).
* **Does almost no heavy work.**
* Maintains a **shadow copy of device state** (render states, samplers, textures, shaders, vertex decl, stream sources, constants).
* Writes rendering work into a **shared command stream ring buffer** as compact packets.
* Only forces synchronization on hard boundaries (e.g., `Present`, `GetQueryData`, `Lock` readback cases).

2. **Back-end (64-bit “unixlib” / native side, ARM64)**

* Owns **Metal device**, queues, pipeline caches, heaps, command buffers.
* Reads command stream packets and **replays** them into Metal with aggressive:

  * state dedupe
  * pipeline caching
  * resource binding amortization
  * batched encoders / minimized pass breaks

This is conceptually similar to Wine’s own “command stream” approach used in wined3d to decouple app thread from driver work (Wine has long-running work around wined3d command streams and enabling MT command streams by default) ([WineHQ][6])—but you’re taking it further by also reducing **WoW64 boundary churn**.

### Why this helps your “1000 draw calls = 20 fps” scenario

Even if FNV is “only” ~1000 draws, the real number of D3D9 calls per frame may be 10k–50k once you count state and resource chatter. With the architecture above:

* Many state calls become **front-end only** (update shadow state, no native call).
* The back-end sees a **much smaller stream**: “draw with state snapshot X”, “update buffer region”, “present”.
* You can make per-draw replay very tight on the ARM64 side.

---

## 2) Exact binary layout: how to ship it as `d3d9.dll` under WoW64

### Recommended packaging

* `d3d9.dll` **(PE32)**: implements the full COM surface for 32-bit games; mostly writes packets to ring buffer.
* `d3d9.dll` **(PE64)**: optional (useful for 64-bit D3D9 titles / tools).
* `d3d9.dll.so` / unixlib **(Mach-O ARM64)**: Metal back-end, command processor thread, presentation glue.

Wine provides the bridging mechanism for PE→Unix calls (historically `__wine_unix_call` and newer dispatch optimizations) ([blog.hiler.eu][7]). You should use it **only for coarse-grained control** (init, flush, present, rare sync), not per draw.

### Overriding loading for testing

Use Wine DLL override mechanisms (`WINEDLLOVERRIDES`) during development to ensure your `d3d9.dll` is used (Wine man pages document override patterns) ([Ubuntu Manpages][8]).

---

## 3) Command stream design: packets, IDs, and zero-copy goals

### Object identity model (critical for speed)

Every D3D9 object that the game sees is a small front-end wrapper around an **opaque 32-bit ID**:

* `DeviceId`, `SwapchainId`
* `BufferId`, `TextureId`, `SurfaceId`
* `VShaderId`, `PShaderId`
* `StateBlockId`, `QueryId`

The back-end owns the real Metal objects and maps `ID → object*`.

### Packet format

Use a single-producer (game thread) → single-consumer (render thread) ring buffer:

```c
struct PacketHeader {
  uint16_t type;
  uint16_t size;     // bytes including header, 8/16 byte aligned
  uint32_t seq;      // monotonically increasing
};
```

Avoid pointers in packets. Use:

* IDs (32-bit)
* small POD structs
* offsets into shared “upload arenas” (see below)

### Upload arenas (for constants, dynamic VB/IB, small immediate data)

Allocate a few big shared buffers per frame:

* `UploadArena[FrameIndex]` for:

  * shader constants uploads
  * `DrawPrimitiveUP` vertex data
  * `UpdateTexture` staging

Packets reference data by `(arenaIndex, offset, size)`.

### Frame boundaries

Define:

* `BeginFrame` packet (optional)
* `Present` packet ends the frame and includes present params.

On `Present`, front-end signals back-end (one unix call or an event). Back-end drains up to that frame boundary and submits Metal command buffer(s).

---

## 4) The single biggest win: “state snapshot” draws instead of state-chatter replay

**Do not** send `SetRenderState` / `SetTexture` packets unless you absolutely must.

Instead:

1. Front-end keeps a complete **ShadowDeviceState** struct.
2. When the game calls `Draw*`, the front-end:

   * computes a **hash** of the relevant state (shaders, render states, blend/depth, vertex decl, sampler bindings, RT formats, etc.)
   * looks up / creates a **CompiledStateBlock** object keyed by that hash
   * emits a `Draw` packet that references `StateBlockId` + draw params + constant upload offsets

So the back-end mostly sees:

* `CreateBuffer/Texture/Shader`
* `UpdateBuffer/Texture`
* `SetRenderTargets`
* `Draw(stateBlockId, …)`
* `Present`

That reduces per-frame packet count dramatically.

This also lets you do **CPU-side optimization of fixed-function configurations** similarly to how DXVK discusses optimizing fixed-function configs (different project, same idea: pre-analyze and reduce CPU overhead) ([GitHub][9]).

---

## 5) Metal back-end: render loop and pass management

### Metal objects

* `id<MTLDevice> device`
* `id<MTLCommandQueue> queue`
* per-frame:

  * `id<MTLCommandBuffer> cmd`
  * one (or few) `id<MTLRenderCommandEncoder> enc` per render pass
  * `id<MTLBlitCommandEncoder>` when needed

### Render pass strategy (very important for DX9 engines)

D3D9 encourages frequent:

* render target switches
* depth surface switches
* `StretchRect` / resolves / post-processing

Back-end rules:

* Keep an encoder open across many draws **as long as**:

  * render targets unchanged
  * depth-stencil unchanged
  * no operation requires ending the pass (readback, resource hazards that Metal can’t handle in-pass, etc.)

When a break is needed:

* end encoder
* start a new `MTLRenderPassDescriptor`

### Preserve D3D9 semantics

D3D9 has implicit layout transitions; Metal requires correctness. Your back-end tracks per resource:

* last usage (RT, SRV, DSV, copy src/dst)
* whether a hazard requires a blit/resolve or a new pass

Keep it simple initially:

* conservative pass breaks first
* then add hazard analysis to reduce breaks

---

## 6) Mapping D3D9 pipeline state to Metal efficiently

### Key idea: pipeline state cache

Metal PSOs are expensive to create; you must cache aggressively.

Your pipeline key must include:

* vertex shader ID + pixel shader ID
* vertex descriptor layout (from D3D9 vertex decl / FVF)
* color attachment formats + blend state
* depth/stencil format + depth/stencil enable
* sample count (MSAA)
* write masks

**Everything else should be dynamic state** where possible (viewport, scissor, stencil ref, depth bias, cull mode).

### Use Metal binary archives to reduce stutter

Metal provides **`MTLBinaryArchive`**: a container for pipeline descriptors + compiled shader code ([Apple Developer][10]).

Plan:

* At runtime, whenever you create a new PSO, add it to the archive.
* Serialize archive to disk cache per title + GPU family.
* On next launch, preload the archive and seed pipeline creation.

This matters because FNV may generate many PSO variants (blend modes, RT formats, vertex decls).

### Asynchronous pipeline builds

Apple’s best practices emphasize building pipelines asynchronously and avoiding lazy pipeline creation on the critical path ([Apple Developer][11]).

Practical approach:

* If a draw references a not-yet-built PSO:

  * fall back to a safe “ubershader” PSO (see below), or
  * queue async compile and temporarily skip / stall (stalls are safer but hurt)
* For your first target (FNV), you can get away with “compile on first use but cache forever”, plus binary archives.

---

## 7) Shader translation: DX9 bytecode → MSL

### What you need to support for Fallout New Vegas

FNV is largely Shader Model 3 era (vertex + pixel shaders), and uses lots of DXT textures and typical DX9 render states.

### Recommended translation pipeline (pragmatic + fast to implement)

1. Parse D3D9 shader bytecode (vs_3_0, ps_3_0) into an IR.
2. Emit SPIR-V (reuse proven components if possible).
3. Use SPIRV-Cross (or your own emitter) to generate Metal Shading Language.
4. Compile to `MTLLibrary`, extract `MTLFunction`s.

This keeps correctness risk lower than “write a full DX9 ASM → MSL compiler from scratch.”

### Critical semantic fixes you must implement

* D3D9 constant registers and packing
* sampler/texture binding mapping
* instruction quirks (texkill/discard, partial precision, etc.)
* coordinate conventions (Metal clip space Z is 0..1 like D3D, which helps)

### Fixed-function pipeline support

Even if FNV is mostly programmable, DX9 games often use bits of fixed function. You have two strategies:

**Strategy A (recommended initially): “FFP ubershaders”**

* Generate a small set of ubershaders that emulate texture stages + lighting using branches controlled by uniform state.
* This reduces PSO explosion (fewer variants), increases GPU work slightly.

**Strategy B: “specialized FFP shaders”**

* Generate specialized shaders per texture-stage configuration.
* Faster on GPU, but can explode PSO count (and compile time).

DXVK has a whole DX9 fixed-function shader generation path and even recent discussions about optimizing fixed function configurations on CPU ([GitHub][12])—use that as inspiration even if you don’t copy it verbatim.

---

## 8) Resource model: textures, buffers, and the Lock/Unlock trap

DX9 performance lives or dies on how you implement:

* `Lock/Unlock` on dynamic vertex buffers
* `D3DLOCK_DISCARD/NOOVERWRITE` patterns
* `UpdateTexture`, `StretchRect`

### Buffers

Back-end representation:

* static VB/IB: one `MTLBuffer`
* dynamic VB/IB: a per-frame ring allocator, suballocated from big `MTLBuffer`s

Front-end:

* on `Lock(DISCARD)`: return pointer into the ring (CPU-visible staging)
* on `Unlock`: emit `BindVBRange` or `UploadVBRange` packet that references arena offsets (ideally zero-copy if shared memory maps into the MTLBuffer)

### Textures

Use:

* `MTLTexture` for GPU
* staging buffers for updates

For DX9 compressed formats (DXT1/3/5), prefer native BC formats if supported on macOS GPU family; if not, decompress in a worker thread at load time.

### Pool semantics (D3DPOOL_DEFAULT vs MANAGED)

You must emulate:

* “lost device” behavior: default pool resources invalidated on Reset, managed restored.
* On Metal you won’t “lose device”, but games expect the lifecycle.

Implement:

* a “resource residency layer” that can recreate GPU resources from system-memory copies for managed resources.

---

## 9) Binding and per-draw CPU cost: use Metal argument buffers

Your main per-draw CPU cost drivers in Metal are typically:

* setting pipeline
* binding N buffers/textures/samplers
* updating constants
* calling draw

Metal **argument buffers** are explicitly designed to reduce CPU binding overhead by grouping many resources into one shader argument ([Apple Developer][1]).

### Concrete plan for D3D9 mapping

Create a unified “DX9 binding table” argument buffer:

* textures `[0..15]`
* samplers `[0..15]`
* a pointer/offset to a constant data block for VS/PS
* optionally, extra tables for lights / fog / FFP state

Then per draw you do:

* bind 1 argument buffer (VS + PS)
* bind vertex/index buffers
* draw

To avoid rewriting argument buffers every draw:

* build a cache of “DescriptorSet-like” objects keyed by:

  * texture IDs per slot
  * sampler state IDs per slot
* many draws reuse the same material bindings → reuse the argument buffer

This can be a *huge* CPU win for DX9 engines with many repeated materials.

---

## 10) Optional “beyond current solutions” step: Indirect Command Buffers (ICB)

Metal **Indirect Command Buffers** are explicitly intended to reduce CPU overhead and allow reuse / GPU generation of command sequences ([Apple Developer][2]).

### Where ICB helps *in a translation layer*

ICB is most valuable when:

* you can reuse command structure, or
* you can encode many draws with fewer driver costs, or
* you move some command generation to GPU

For a DX9 translation layer, a realistic ICB use is:

* During back-end replay of a frame, instead of calling `drawIndexedPrimitives` directly for every draw:

  * encode draws into an ICB (CPU-encoded ICB)
  * execute the ICB once for the pass

This **can** reduce CPU overhead in some drivers because Metal can validate/optimize the indirect buffer path differently, and it sets you up for future GPU-driven improvements.

Caveats:

* You often need argument buffers to make ICB practical (resource binding model).
* There are limits (e.g., max commands), and you must manage resource residency carefully.

I would treat ICB as **Phase 3** optimization after you already have a fast baseline.

---

## 11) Queries, occlusion, and “things that force sync”

Fallout-era engines often use:

* occlusion queries
* timestamp queries
* `GetRenderTargetData` (screenshots, sometimes post effects)
* `LockRect` on render targets (rare but possible)

Metal has visibility result support that can back occlusion query behavior, but these features can force pipeline breaks and CPU/GPU sync.

Plan:

* Implement query objects with a “deferred result” model:

  * results become available N frames later
  * `GetQueryData` returns `S_FALSE` until ready (D3D9 allows this)
* Only block when the game explicitly blocks.

---

## 12) Presentation on macOS: CAMetalLayer integration

For swapchain presentation:

* ensure the target NSView is backed by `CAMetalLayer` (a common requirement even for Vulkan-on-mac setups) ([Apple Developer][13]).
* back-end obtains a drawable from the layer each Present and renders into it, or blits your backbuffer into it.

If you’re integrating into Wine’s mac driver, you may need a small “surface bridge” similar in spirit to how Vulkan-on-mac uses NSView/CAMetalLayer surfaces ([docs.vulkan.org][14]).

---

## 13) Concrete phased implementation plan (what to build, in order)

### Phase 1: Minimal correctness on FNV main menu

Goal: reach stable rendering with basic performance.

Front-end:

* Implement:

  * `Direct3DCreate9`
  * `IDirect3D9::CreateDevice`
  * `IDirect3DDevice9::{Clear, BeginScene, EndScene, Present}`
  * basic resource create (VB/IB/Texture2D)
  * shader objects (vs/ps)
  * `DrawIndexedPrimitive`

Back-end:

* single render pass to drawable
* simple pipeline cache keyed by shader pair + vertex layout + RT format
* constant upload ring buffer
* basic sampler cache

### Phase 2: D3D9 “real game” features

Add:

* render target switching (`SetRenderTarget`, `SetDepthStencilSurface`)
* `StretchRect` (post effects)
* alpha blending modes, depth/stencil ops, culling, scissor/viewport
* gamma/sRGB toggles
* dynamic VB/IB and Lock flags

At this point you should be able to play, but maybe with stutter.

### Phase 3: Performance architecture (the big win)

* Implement the **state snapshot** model:

  * shadow state in front-end
  * hashed state blocks
  * draw packet references state block ID
* Add argument-buffer based resource binding ([Apple Developer][1])
* Add robust PSO caching + disk cache + `MTLBinaryArchive` seeding ([Apple Developer][10])
* Add multithreaded shader compilation and PSO compilation (async) ([Apple Developer][11])

### Phase 4: “Beyond current stacks” optimizations

* CPU-encoded ICB for draw-heavy passes ([Apple Developer][2])
* More aggressive hazard analysis to reduce render pass breaks
* Optional automatic batching/instancing experiments (risky, likely not needed at ~1000 draws)

---

## 14) How you validate you’re actually fixing *your* bottleneck

Before and after each phase, measure:

1. **D3D9 call count per frame**

* draw calls
* state changes
* resource updates
* lock/unlock count and sizes

2. **Front-end cost**

* time spent in `d3d9.dll` on the emulated thread

3. **Back-end cost**

* time to replay command stream and encode Metal commands

4. **Metal command encoding overhead**

* use Instruments / Metal System Trace

If your current 20 fps is truly “translation CPU overhead”, you should see:

* back-end encode time drop sharply once you implement snapshot states + argument buffers
* fewer redundant encoder calls
* stable frame pacing once PSOs are cached/binary-archived

---

## 15) One important “gotcha” that can masquerade as draw-call overhead

On macOS Wine/CrossOver setups, rendering at “retina” / very high resolution can obliterate performance and look like CPU overhead. There are real-world reports where disabling “High Resolution Mode” and forcing 1080p fixes “terrible performance” dramatically ([A journey into a wild pointer][5]).

Even if you’re convinced it’s CPU, do that sanity check while you build the new layer—otherwise you can misattribute gains/losses.

---

## Summary of why this plan can make FNV “fly”

A direct D3D9→Metal layer only wins big if it also:

* **prevents D3D9 chatty calls from becoming native transitions**
* reduces state churn via **state snapshot hashing**
* amortizes binding via **argument buffers** ([Apple Developer][1])
* eliminates pipeline stutter via **pipeline caching + Metal binary archives** ([Apple Developer][10])
* optionally leverages **ICB** for further CPU reduction ([Apple Developer][2])

That combination is what can turn “1000 DX9 draws = 20 fps” into “1000 draws is nothing.”

If you want, I can follow up by sketching:

* the exact `ShadowDeviceState` struct layout (what to include in the hash vs what to keep dynamic),
* the precise D3D9→Metal enum mapping tables (blend factors/ops, compare funcs, stencil ops),
* and a minimal packet list for FNV-first support (so you don’t overbuild D3D9 you don’t need).

[1]: https://developer.apple.com/documentation/metal/improving-cpu-performance-by-using-argument-buffers?changes=_2&utm_source=chatgpt.com "Improving CPU performance by using argument buffers"
[2]: https://developer.apple.com/documentation/metal/indirect-command-encoding?utm_source=chatgpt.com "Indirect command encoding"
[3]: https://man.archlinux.org/man/winebuild.1.en?utm_source=chatgpt.com "winebuild(1) - Arch manual pages"
[4]: https://news.ycombinator.com/item?id=39020041&utm_source=chatgpt.com "Wine 9.0"
[5]: https://carette.xyz/posts/deep_dive_into_crossover/ "
    
    Deep dive into crossover | A journey into a wild pointer
    
"
[6]: https://www.winehq.org/pipermail/wine-devel/2018-January/121819.html?utm_source=chatgpt.com "Enable the multi-threaded command stream by default."
[7]: https://blog.hiler.eu/wine-pe-to-unix/?utm_source=chatgpt.com "Wine's PE -> UNIX Interface - Ivyl's Blog"
[8]: https://manpages.ubuntu.com/manpages/jammy/man1/wine-development.1.html?utm_source=chatgpt.com "wine - run Windows programs on Unix"
[9]: https://github.com/doitsujin/dxvk/issues/5320?utm_source=chatgpt.com "D3D9 Fixed Function: To-Do · Issue #5320 · doitsujin/dxvk"
[10]: https://developer.apple.com/documentation/metal/mtlbinaryarchive?utm_source=chatgpt.com "MTLBinaryArchive | Apple Developer Documentation"
[11]: https://developer.apple.com/library/archive/documentation/3DDrawing/Conceptual/MTLBestPracticesGuide/Pipelines.html?utm_source=chatgpt.com "Metal Best Practices Guide: Pipelines"
[12]: https://github.com/doitsujin/dxvk/blob/master/README.md?utm_source=chatgpt.com "dxvk/README.md at master · doitsujin/dxvk"
[13]: https://developer.apple.com/documentation/quartzcore/cametallayer?utm_source=chatgpt.com "CAMetalLayer | Apple Developer Documentation"
[14]: https://docs.vulkan.org/refpages/latest/refpages/source/vkCreateMacOSSurfaceMVK.html?utm_source=chatgpt.com "vkCreateMacOSSurfaceMVK(3) - Vulkan Documentation"
