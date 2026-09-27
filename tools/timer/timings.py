#!/usr/bin/env python3
"""Merge sopt-timer.csv files (one per API / GPU) into a Markdown table.

    python3 tools/timer/timings.py dx11-amd.csv vulkan-amd.csv dx11-nv.csv > timings.md

One row per preset (effect and SOPT_ALL step), one column per CSV: orig -> sopt in
microseconds, the median per-frame difference in percent, and a verdict from the
difference's 10th/90th percentiles: "faster" when both are below 0, "slower" when both
are above, "same" otherwise (the difference is inside the frame-to-frame noise).
"""
import csv
import re
import sys


def load(path):
    header, rows = "", {}
    with open(path, newline="", encoding="utf-8", errors="replace") as f:
        lines = f.read().splitlines()
    if lines and lines[0].startswith("#"):
        header = lines[0].lstrip("# ").strip()
        lines = lines[1:]
    for r in csv.DictReader(lines):
        rows[(r["preset"], r["technique"])] = r
    return header, rows


def cell(r):
    if r is None:
        return "-"
    if not r.get("frames") or r["frames"] == "0":
        return "no data"
    try:
        o, s = float(r["orig_us"]), float(r["sopt_us"])
        d, lo, hi = float(r["diff_pct"]), float(r["diff_p10"]), float(r["diff_p90"])
    except (KeyError, ValueError):
        return "?"
    verdict = "faster" if hi < 0 else "slower" if lo > 0 else "same"
    return f"{o:.1f} -> {s:.1f} ({d:+.1f}%, {verdict})"


def main(paths):
    if not paths:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    runs = [load(p) for p in paths]
    keys = []
    for _, rows in runs:
        for k in rows:
            if k not in keys:
                keys.append(k)
    keys.sort()
    print("## Measured (sopt-timer)\n")
    for i, (p, (h, _)) in enumerate(zip(paths, runs), 1):
        print(f"- [{i}] {p}: {h}")
    print("\nmicroseconds per run, orig -> sopt (median per-frame difference, verdict from its "
          "p10-p90)\n")
    print("| effect | step | " + " | ".join(f"[{i}]" for i in range(1, len(runs) + 1)) + " |")
    print("|---|---|" + "---|" * len(runs))
    for preset, tech in keys:
        m = re.match(r"sopt-\d+-(.+)-(\d+of\d+)\.ini$", preset)
        effect, step = (m.group(1), m.group(2)) if m else (preset, "")
        cells = [cell(rows.get((preset, tech))) for _, rows in runs]
        print(f"| {effect} | {step} | " + " | ".join(cells) + " |")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
