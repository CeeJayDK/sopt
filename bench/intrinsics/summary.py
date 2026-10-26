"""Summary of the deep intrinsic search (.github/workflows/intrinsics.yml): one sopt log per intrinsic and cost
model (<name>.<model>.txt). Each intrinsic alone: can sopt beat the compilers' own lowering? (owner, 2026-10-26)

usage: python3 summary.py <folder with the logs>
"""
import glob
import os
import re
import sys


def parse(text):
    m = re.search(r"\(cost (\d+),", text)
    target = m[1] if m else "?"
    # The first row under "cost  ver  class ..." (the cheapest): columns apart by two or more spaces.
    alt = None
    lines = text.splitlines()
    for i, l in enumerate(lines):
        if l.startswith("cost  ver  class") and i + 1 < len(lines):
            cols = re.split(r"\s{2,}", lines[i + 1].strip())
            if len(cols) >= 7 and cols[0].isdigit():
                alt = (cols[0], cols[2], cols[-1])
            break
    depth = re.search(r"(?:levels complete|searched) up to cost (\d+) of (\d+)", text)
    reach = f"{depth[1]} of {depth[2]}" if depth else ("all" if alt or "no cheaper" in text else "?")
    return target, alt, reach


def main(folder):
    rows, found = [], []
    for f in sorted(glob.glob(os.path.join(folder, "**", "*.txt"), recursive=True)):
        name, model = os.path.basename(f)[:-4].split(".", 1)
        target, alt, reach = parse(open(f, encoding="utf-8", errors="replace").read())
        rows.append(f"| {name} | {model} | {target} | {alt[0] if alt else '-'} | {reach} |")
        if alt:
            found.append(f"| {name} | {model} | {target} -> {alt[0]} | {alt[1]} | `{alt[2]}` |")
    print("# Each intrinsic alone, deep search\n\nCost: the model's units (one mad = 4). Searched: every candidate up to"
          " this cost was tried (\"x of y\": y is the original's cost; \"all\": the search finished).\n\n"
          "| intrinsic | model | cost | best found | searched |\n|---|---|---|---|---|\n" + "\n".join(rows))
    print("\n## Cheaper forms found\n\n| intrinsic | model | cost | class | expression |\n|---|---|---|---|---|\n" +
          ("\n".join(found) if found else "| none | | | | |"))


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
