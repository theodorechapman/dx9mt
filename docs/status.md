# dx9mt Status (2026-02-17)

## User-Observed Behavior (Current Session)

- Startup loading screen appears normal (texture fade behavior looks right).
- Main menu enters after click; some choppiness during transition.
- Once right-side menu options appear, cursor visibility becomes intermittent.
  - Cursor often invisible unless overlapping a selectable menu item.
- Save loading works and enters gameplay.
- In gameplay, HUD updates and input is live, but world viewport is flat light blue.

## What Is Working

- D3D9 device creation, packet sequencing, and present flow are stable.
- IPC path is active and continuous (`present ... (metal-ipc)`).
- Main menu rendering pipeline is functional (textures, blending, shader translation path).
- Save load no longer crashes (adapter/caps path and VEH safety net active).
- In-game draw volume is high and being captured (1000+ draw frames observed).

## Active Issues

### 1) Gameplay world not visible (blue viewport)

- Symptom: world output collapses to light-blue clear color while HUD continues.
- Strong evidence of frame-data integrity pressure in gameplay:
  - upload slot overflow events at frame `3334`.
  - many `draw packet missing constants_vs` and `constants_ps` validation failures.
- Practical impact: backend drops affected draws before IPC serialization, so viewer can present mostly clear color.

### 2) Cursor visibility regression in menu

- Symptom: cursor appears only when overlapping interactive elements.
- Current code still relies on default stubbed cursor-related `IDirect3DDevice9` methods unless explicitly overridden.
- Needs dedicated cursor behavior implementation/bridging.

### 3) Documentation drift was significant

- Old docs claimed several implemented features were missing.
- TODO and next-steps were duplicated and diverged.
- Architecture navigation doc was missing.

## Concrete Runtime Evidence

From `/tmp/dx9mt_runtime.log` in the latest run:

- IPC present is active:
  - `present frame=3360 ... (metal-ipc) ... draws=1476`
  - `present frame=3480 ... (metal-ipc) ... draws=1094`
- Overflow and dropped-required-payload signs:
  - `slot overflow: frame=3334 slot=1 ... capacity=134217728`
  - repeated `draw packet missing constants_vs payload` (and one `constants_ps`).
- Clear color in failing range is blue-ish:
  - `last_clear=0x0096a8be`

From `/tmp/dx9mt_frame_dump_0480.txt` and `/tmp/dx9mt_frame_dump_0481.txt`:

- non-zero draw frames are captured (`draws: 57`) with valid RT metadata.
- intermittent zero-draw frame dumps also occur nearby (`draws: 0`).

## Where To Look Next

- Code navigation: [architecture.md](architecture.md)
- Execution plan: [roadmap.md](roadmap.md)
