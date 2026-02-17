# dx9mt

Direct3D9-to-Metal translation layer for Wine WoW64 on Apple Silicon, currently focused on Fallout: New Vegas.

## Quick Start

```bash
make -C dx9mt
make -C dx9mt test-native
make run
```

## Current State (2026-02-17)

The project reaches FNV gameplay and renders UI/HUD, but the in-game world viewport can still collapse to a flat light-blue output in some sessions.

Use these docs for the live picture:

- [docs/status.md](docs/status.md): verified behavior, regressions, and runtime evidence.
- [docs/roadmap.md](docs/roadmap.md): merged TODO + next-steps plan.
- [docs/architecture.md](docs/architecture.md): code navigation map for agents.
- [docs/insights.md](docs/insights.md): implementation constraints and gotchas.

## Repo Layout

- `dx9mt/`: core translation layer (`d3d9.dll`, backend bridge, Metal viewer, shader translator).
- `docs/`: project status, architecture, roadmap, and notes.
- `tools/`: helper scripts (`analyze_dx9mt_log.py`).
- `assets/`: reference screenshots.
- `wineprefix/`: local Wine environment used for testing.
