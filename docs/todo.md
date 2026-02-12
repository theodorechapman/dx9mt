# dx9mt TODO

## Current Priority: Render-Target Texture Fidelity (Step 1)

- [x] Carry render-target metadata per draw across frontend/backend/IPC.
  - `render_target_id`
  - `render_target_texture_id` (when RT is backed by a texture object)
  - `render_target_width/height/format`
- [x] Route viewer replay by render-target ID instead of forcing all draws to the swapchain target.
- [x] Expose offscreen RT outputs as shader-readable textures for later draws in-frame.
- [x] Prefer present-time primary RT hint over pure “last draw RT” inference.
- [x] Keep RT-to-texture override cache alive across frames to avoid intermittent white UI quads.
- [ ] Handle explicit clear behavior per non-primary render target (currently first-use clear heuristic).
- [ ] Add stable fallback policy for missing RT content (currently skip draw to avoid white fallback quads).
- [ ] Validate pass transitions with live FNV menu and load/save UI captures.
- [ ] Add focused logging for RT misses and texture override hits in viewer.

## Next After Step 1

- [ ] Depth/stencil state replay.
- [ ] Blend-op fidelity (`D3DRS_BLENDOP` and separate alpha blend).
- [ ] Crash triage on save-load transition after RT path stabilizes.
