# dx9mt Technical Insights

## 1) The active architecture is IPC-first

- The PE32 `d3d9.dll` does not execute Metal directly in Wine.
- Runtime rendering path is shared-memory IPC (`/tmp/dx9mt_metal_frame.bin`) to the native viewer.
- `libdx9mt_unixlib.dylib` is built but not yet the active path.

## 2) Frame boundaries and packet order are strict

- `BEGIN_FRAME`, `DRAW_INDEXED`, `CLEAR`, and `PRESENT` are all packetized.
- Backend rejects non-monotonic packet sequence IDs.
- Repeated `BeginScene/EndScene` within the same `frame_id` are expected and accumulated.

## 3) Upload overflow behavior is fail-closed

- Upload arena: `3 x 128 MB` slots.
- Overflow returns a zero upload ref (intentional), backend rejects required missing payloads.
- This avoids silent memory corruption, but can manifest as missing world draws under heavy load.

## 4) Shader translation coverage is materially broader than old docs claimed

Current parser/emitter includes:

- Flow control (`if/ifc/else/endif`, `rep/endrep`, `break/breakc`).
- Relative constant addressing via `a0` indexing.
- `def`, `defi`, `defb` immediate declarations.
- Multi-sampler bindings (`s0..s7`) in translated path.
- Core arithmetic + texture ops (`texld`, `texldl`, `texkill`, etc.).

## 5) State coverage in viewer is broader than old docs claimed

Viewer currently consumes:

- Depth test/write and compare func.
- Stencil compare/op/read-mask/write-mask.
- Cull mode.
- Scissor test.
- Blend factors + blend operation + color write mask.
- Fog params in fallback shader path.

## 6) Some important D3D9 methods are still stub-driven

Default device methods log `dx9mt/STUB` and return generic fallback values unless overridden.
This is still relevant for cursor and less-used API surfaces.

## 7) Log analyzer script currently lags log format

- `tools/analyze_dx9mt_log.py` expects older log lines without `[tid=....]`.
- Current `dx9mt_logf` includes thread IDs, so parser updates are required before relying on that script.
