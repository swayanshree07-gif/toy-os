#!/usr/bin/env python3
"""Plot per-scenario wall time and assertion counts from real `make test` output.
Usage: make test > build/test-output.txt; .venv/bin/python tools/plot_tests.py build/test-output.txt docs/media/test-times.png"""
import re, sys
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

src, dst = sys.argv[1], sys.argv[2]
rows, cur = [], None
for line in open(src, errors="replace"):
    m = re.match(r"\s*time\s+(\S+): ([\d.]+) s", line)
    if m:
        cur = [m.group(1), float(m.group(2)), 0]; rows.append(cur); continue
    if cur and line.strip().startswith("PASS"):
        cur[2] += 1
total = re.search(r"TOTAL TIME ([\d.]+) s \(budget (\d+) s\)", open(src).read())
rows.sort(key=lambda r: r[1])
names = [r[0] for r in rows]
fig, ax = plt.subplots(figsize=(8, 0.26 * len(rows) + 1.2), dpi=100)
ax.barh(names, [r[1] for r in rows], color="#3b6ea8", height=0.7)
for i, r in enumerate(rows):
    ax.text(r[1] + 0.1, i, f"{r[2]} checks", va="center", fontsize=7, color="#444")
ax.set_xlabel("wall time per QEMU boot (s)")
title = f"make test: {len(rows)} headless QEMU scenarios, {sum(r[2] for r in rows)} passing checks"
if total:
    title += f"\ntotal {total.group(1)} s (budget {total.group(2)} s)"
ax.set_title(title, fontsize=10, loc="left")
ax.tick_params(axis="y", labelsize=7)
for s in ("top", "right"):
    ax.spines[s].set_visible(False)
ax.margins(y=0.01)
fig.tight_layout()
fig.savefig(dst)
