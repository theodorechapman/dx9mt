# dx9mt Next Steps

## Where We Are

RB5 is still in progress, but the focus shifted from feature bring-up to crash hardening after save-load failures.

- **Block-compressed safety fixes landed** -- DXT copy rect validation, block-size scaling rejection, and `ColorFill` rejection for compressed surfaces.
- **Multithread runtime guard landed** -- per-device critical section with lock-aware `IDirect3DDevice9` wrappers; VB/IB/surface/texture lock/unlock paths now use the same guard.
- **Known crash signature** -- save-load crash was a null dereference in game code (`falloutnv+0x757aa9`, `ESI=0`) while device behavior flags included `D3DCREATE_MULTITHREADED` (`0x00000054`).

## Immediate Next: Re-Validate Save/Load Stability

### The situation

The highest-priority question is now binary: does gameplay save/load remain stable with the new multithread guard enabled.

### Steps

1. **Replay the failing save-load path with dx9mt active** (`make run`), capture whether crash reproduces.
2. **Hold in gameplay for sustained runtime** (streaming + UI transitions) to catch delayed race fallout.
3. **If crash persists**, collect:
   - `backtrace.txt`
   - `/tmp/dx9mt_runtime.log` around crash time
   - whether faulting thread is still a worker thread vs render thread
4. **If crash persists with same signature**, extend locking scope to all child COM vtbls (not only lock/unlock hot paths) and add upload-arena global guarding for multi-device/thread edge cases.

### Remaining RB5 Items

| Item | Priority | Notes |
|------|----------|-------|
| Save/load stability verification | High | Gate before more feature work |
| Full child-object multithread wrapping (if needed) | High | Fallback if crash still reproduces |
| Relative addressing (a0) | High | Blocks skinned character rendering |
| VS/PS linkage validation | Medium | Wrong output but won't crash |
| Fog state | Medium | Cosmetic, outdoor scenes |
| Stencil operations | Medium | Shadow volumes, masking |
| Blend-op fidelity | Low | SUBTRACT/MIN/MAX are rare |
| Fill mode | Low | Almost always solid |
| Separate alpha blend | Low | Uncommon in FNV |

## Medium Term

### Wine unix lib integration
Replace IPC with in-process `__wine_unix_call` for zero-copy data sharing and synchronized present. The `libdx9mt_unixlib.dylib` is already built with Metal support.

### Performance optimization
- Buffer ring allocator to replace per-draw `newBufferWithBytes` allocations
- State deduplication to avoid redundant PSO/DSS lookups
- PSO cache serialization to disk for faster startup
- Async shader compilation to avoid hitching on new shaders

## Priority Order

1. **Save/load stability validation with dx9mt**
2. **Crash hardening follow-up (if still reproducible)**
3. **Relative addressing and shader completeness**
4. **Render state coverage** -- fog, stencil ops as needed
5. **Wine unix lib integration + performance work**
