#!/usr/bin/env python3
"""Plot framerate over a dx9mt run from its runtime log.

The backend logs a present line every 120 frames with a [HH:MM:SS] timestamp;
fps for each interval = delta_frames / delta_seconds, anchored on the last
entry whose timestamp actually advanced (timestamps have 1 s resolution).

Usage:
    tools/plot_fps.py [runtime_log] [output_png]

Defaults: the latest dx9mt-output/session-*/dx9mt_runtime.log, written to
assets/fps_<session>.png in the repo root.
"""
import glob
import os
import re
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import MultipleLocator

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Chart palette (dataviz reference instance, light mode)
SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK2 = "#52514e"
GRID = "#e7e6e2"
SERIES1 = "#2a78d6"

GAMEPLAY_DRAW_THRESHOLD = 500  # draws/frame above this = dense 3D scene


def default_log():
    candidates = glob.glob(
        os.path.join(REPO, "dx9mt-output", "session-*", "dx9mt_runtime.log"))
    candidates += glob.glob(
        os.path.join(REPO, "dx9mt-output", "session-*", "wined3d_fps.log"))
    if not candidates:
        sys.exit("no session logs found under dx9mt-output/")
    return max(candidates, key=os.path.getmtime)


def parse_wined3d_samples(log_path):
    """Wine's +timestamp,+fps channels: '123.456:...:trace:fps:... 59.94fps'.

    Each sample is already a rate over the preceding ~1.5s window; the
    timestamp prefix is seconds since wine started.
    """
    pat = re.compile(r"^(\d+)\.(\d+):.*:fps:.*?(\d+(?:\.\d+)?)\s*fps")
    times, fps = [], []
    with open(log_path, "r", errors="replace") as f:
        for line in f:
            m = pat.match(line)
            if not m:
                continue
            t = int(m.group(1)) + int(m.group(2)) / 1000.0
            times.append(t)
            fps.append(float(m.group(3)))
    if len(times) < 2:
        sys.exit(f"not enough wined3d fps samples in {log_path}")
    t0 = times[0]
    return [(t - t0) / 60.0 for t in times], fps, [0] * len(fps)


def parse_samples(log_path):
    pat = re.compile(r"^\[(\d+):(\d+):(\d+)\].*present frame=(\d+) .*draws=(\d+)")
    entries = []
    with open(log_path, "r", errors="replace") as f:
        for line in f:
            m = pat.match(line)
            if not m:
                continue
            h, mnt, s, frame, draws = map(int, m.groups())
            t = h * 3600 + mnt * 60 + s
            if entries and t < entries[-1][0]:
                t += 86400  # midnight rollover
            entries.append((t, frame, draws))
    if len(entries) < 2:
        sys.exit(f"not enough present samples in {log_path}")

    t0 = entries[0][0]
    times, fps, draws_at = [], [], []
    at, af = entries[0][0], entries[0][1]
    for t, frame, d in entries[1:]:
        if t <= at or frame <= af:
            continue
        times.append(((t + at) / 2 - t0) / 60.0)  # interval midpoint, minutes
        fps.append((frame - af) / (t - at))
        draws_at.append(d)
        at, af = t, frame
    return times, fps, draws_at


def main():
    log_path = sys.argv[1] if len(sys.argv) > 1 else default_log()
    is_wined3d = os.path.basename(log_path).startswith("wined3d")
    sess = re.search(r"(session-[0-9-]+(?:-wined3d)?)", log_path)
    sess_name = sess.group(1) if sess else "run"
    if is_wined3d and not sess_name.endswith("wined3d"):
        sess_name += "-wined3d"
    out_path = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
        REPO, "assets", f"fps_{sess_name}.png")
    os.makedirs(os.path.dirname(out_path), exist_ok=True)

    if is_wined3d:
        times, fps, draws_at = parse_wined3d_samples(log_path)
    else:
        times, fps, draws_at = parse_samples(log_path)

    fig, ax = plt.subplots(figsize=(10, 5), dpi=200)
    fig.patch.set_facecolor(SURFACE)
    ax.set_facecolor(SURFACE)

    ax.plot(times, fps, color=SERIES1, linewidth=1.8, marker="o",
            markersize=4.5, markerfacecolor=SERIES1, markeredgecolor=SURFACE,
            markeredgewidth=1.0, zorder=3)

    # Selective annotations for the two regimes
    gp = [(x, y, d) for x, y, d in zip(times, fps, draws_at)
          if d > GAMEPLAY_DRAW_THRESHOLD]
    if gp:
        gx = sum(x for x, _, _ in gp) / len(gp)
        gy = sum(y for _, y, _ in gp) / len(gp)
        gd = sum(d for _, _, d in gp) / len(gp)
        ax.annotate(
            f"dense exterior gameplay\n~{gy:.0f} fps @ ~{gd:.0f} draws/frame",
            xy=(gx, gy), xytext=(gx, gy + 32),
            color=INK2, fontsize=9, ha="center",
            arrowprops=dict(arrowstyle="-", color=INK2, linewidth=0.8))
    top = max(fps)
    menu = [(x, y) for x, y, d in zip(times, fps, draws_at) if y >= top * 0.9]
    if menu and top >= 3 * min(fps):
        mx = sum(x for x, _ in menu) / len(menu)
        my = sum(y for _, y in menu) / len(menu)
        ax.annotate(f"menus / light scenes (~{my:.0f} fps)",
                    xy=(mx, my), xytext=(mx, my * 0.85),
                    color=INK2, fontsize=9, ha="center")

    ax.set_ylim(0, max(132.0, top * 1.1))
    ax.set_xlim(left=0)
    ax.yaxis.set_major_locator(MultipleLocator(20))
    ax.grid(axis="y", color=GRID, linewidth=0.8, zorder=0)
    ax.set_axisbelow(True)
    for side in ("top", "right", "left"):
        ax.spines[side].set_visible(False)
    ax.spines["bottom"].set_color(GRID)
    ax.tick_params(colors=INK2, labelsize=9, length=0)

    ax.set_xlabel("minutes into session", color=INK2, fontsize=10)
    ax.set_ylabel("frames per second", color=INK2, fontsize=10)
    backend = "wined3d (builtin d3d9)" if is_wined3d else "dx9mt"
    method = ("WINEDEBUG=+fps swapchain samples (~1.5 s windows)"
              if is_wined3d else
              "game-side present rate, sampled every 120 frames "
              "(1 s timestamp resolution)")
    ax.set_title(f"Fallout: New Vegas under {backend} — framerate over the run",
                 color=INK, fontsize=13, loc="left", pad=18, weight="bold")
    ax.text(0, 1.02, f"{sess_name} · {method}",
            transform=ax.transAxes, color=INK2, fontsize=9)

    fig.tight_layout()
    fig.savefig(out_path, facecolor=SURFACE, bbox_inches="tight")
    msg = f"wrote {out_path}: {len(times)} samples"
    if gp:
        msg += f", gameplay avg {sum(y for _, y, _ in gp) / len(gp):.1f} fps"
    print(msg)


if __name__ == "__main__":
    main()
