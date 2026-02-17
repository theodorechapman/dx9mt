# dx9mt Next Steps

## Where We Are

RB5 is in progress. Save game loading now works. The game enters gameplay but the 3D viewport is black (ESC overlay texture is visible). Loading screen hangs until Cmd+Tab forces it forward.

### What's Working
- Main menu renders correctly
- Save loading doesn't crash (adapter identity + caps fix)
- Game enters gameplay state (ESC overlay visible, audio playing)
- BSShader factory NULL returns handled by VEH safety net + proper adapter identity

### What's Broken
- **3D world is black** -- the game is running (we can hear audio, see ESC overlay) but no 3D geometry renders
- **Loading screen hangs** -- progress bar doesn't animate, needs Cmd+Tab to continue
- **Cursor clipping** -- disappears when not on menu buttons
- **Save thumbnails** -- preview image missing in save/load screen

## Immediate Next: Fix Black Viewport

### Hypothesis 1: Draws not reaching the viewer
The gameplay draws may not be making it through the IPC. Check:
- Are gameplay frames being written to IPC? (grep for frame sequence progression in logs)
- Is the draw count changing between menu and gameplay?
- Are we hitting upload arena overflow on gameplay frames (more draws/data than menu)?

### Hypothesis 2: Shader translation failures
Gameplay shaders are likely more complex than menu shaders. Check:
- Are shader compile errors being logged to stderr?
- What percentage of gameplay draws have successful shader translation?
- Is relative addressing (a0) blocking most gameplay VS?

### Hypothesis 3: Render target mismatch
The game may switch to a different render target for gameplay. Check:
- What render_target_id are gameplay draws using?
- Is the final Present pointing at the right RT?
- Are offscreen RTs being composited correctly?

### Steps
1. **Capture a frame dump** during gameplay (press D in viewer) -- compare draw count, RT IDs, shader status between menu and gameplay
2. **Check stderr** for shader compilation failures during gameplay
3. **Check logs** for frame progression and IPC activity after loading completes
4. **Add RT diagnostics** if needed -- log which RTs are created and which one is presented

## Next: Fix Loading Screen Hang

The loading screen likely hangs because:
- Our Present implementation may not actually flip/present during loading
- The IPC writer might be waiting for the viewer to consume the current frame
- A stub method called during loading might block

Check: is Present being called during loading? Are frames progressing?

## Remaining RB5 Items

| Item | Priority | Notes |
|------|----------|-------|
| Black viewport diagnosis | Critical | Must fix to make progress |
| Loading screen hang | Critical | Blocks normal workflow |
| Relative addressing (a0) | High | Blocks skinned character rendering |
| Fog state | Medium | Cosmetic, outdoor scenes |
| Stencil operations | Medium | Shadow volumes, masking |
| VS/PS linkage validation | Medium | Wrong output but won't crash |
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

### Cleanup
- Remove VEH diagnostic dumps (factory, shader table, TLS -- no longer needed)
- Gate or remove VEH BSShader patches if adapter fix is confirmed sufficient
- Restore sampled logging for STUB macro to reduce log volume

## Priority Order

1. **Fix black viewport** -- diagnose and fix, this blocks everything
2. **Fix loading screen hang** -- quality of life for testing
3. **Relative addressing** -- unblock skinned mesh shaders
4. **Render state coverage** -- fog, stencil ops as needed
5. **Cleanup** -- remove debugging scaffolding from crash investigation
6. **Wine unix lib integration** -- performance, zero-copy, proper architecture
7. **Performance optimization** -- buffer recycling, state dedup, async compile
