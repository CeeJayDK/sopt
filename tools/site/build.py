#!/usr/bin/env python3
"""Builds the SweetOpt / GPU Blueprint web pages (owner, 2026-10-05), published at ceejay.dk/sopt/ with GitHub Pages by
.github/workflows/pages.yml: the SweetOpt start page, the rewrite library, the cost models with every DirectX 11
architecture (measured or not yet) and the TexBench findings with the pixel order pictures.

    build.py <sopt --cost-models-json output> <sopt --library-json output> <output folder>

Only hardware facts are published (card, vendor / device ID, driver version): no names of the people who ran the
reports (GDPR). Reports that cannot be tied to a model (software renderers, cards without a model yet) are listed
as "not modelled yet".
"""
import csv
import glob
import json
import os
import re
import html
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# Reports whose own GPU string is wrong: a spoofed device ID, and a laptop whose switchable graphics made DXGI name
# the Intel GPU while the Radeon did the work (see CLAUDE.md).
NAME_BY_FILE = {
    "nvidia-rtx-5080.csv": "NVIDIA GeForce RTX 5080",
    "amd-radeon-hd-7400m-as-intel-hd-3000.csv": "AMD Radeon HD 7400M",
}
# Generic names that only the device ID tells apart.
NAME_BY_DEVICE = {
    "0x1681": "AMD Radeon 680M",
    "0x1636": "AMD Radeon Vega (Renoir)",
    "0x164E": "AMD Radeon Graphics (Raphael)",
}
SKIP_DEVICES = {"0x008C"}  # Microsoft Basic Render Driver (software)

# Card name -> cost model, first match wins.
MODEL_RULES = [
    (r"RTX 50\d\d", "nvidia-blackwell"),
    (r"RTX (30|40)\d\d", "nvidia-ampere"),
    (r"GTX 16\d\d|RTX 20\d\d", "nvidia-turing"),
    (r"GTX 10\d\d|GT 10\d\d", "nvidia-pascal"),
    (r"GTX (9\d\d|8\d\dM)|Quadro M\d", "nvidia-maxwell"),
    (r"RX 9\d\d\d", "amd-rdna4"),
    (r"RX 7\d\d\d", "rdna3"),
    (r"RX 6\d\d\d|Radeon 6[68]0M|Raphael", "amd-rdna2"),
    (r"Vega", "amd-gcn5"),
    (r"HD [67]\d\d\dM?\b", "amd-terascale2"),
    (r"Iris\S* (Graphics )?5\d\d|U?HD Graphics 6\d\d|HD Graphics 5\d\d", "intel-gen9"),
    (r"HD Graphics 4[2-6]\d\d|Iris\S* (Pro )?(Graphics )?5[12]00", "intel-gen7.5"),
]


def read_report(path):
    """GPU name, vendor, device, driver and OpBench version of a report (header lines, or the old per-row format)."""
    info = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        lines = f.read().splitlines()
    for line in lines:
        m = re.match(r"# (OpBench|TexBench) (\S+)", line)
        if m:
            info["tool"] = m.group(1)
            info["version"] = m.group(2)
        m = re.match(r"# (gpu|vendor|device|driver): (.*)", line)
        if m:
            info[m.group(1)] = m.group(2).strip()
    if "gpu" not in info:  # version 1: gpu,vendor,device,driver on every row
        rows = [r for r in csv.reader(l for l in lines if l and not l.startswith("#"))]
        if len(rows) > 1 and rows[0][:4] == ["gpu", "vendor", "device", "driver"]:
            info.update(gpu=rows[1][0], vendor=rows[1][1], device=rows[1][2], driver=rows[1][3], version="0.0")
    return info


def clean_name(name):
    name = re.sub(r"\((R|TM)\)", "", name)
    return re.sub(r"\s+", " ", name).strip()


def model_of(card):
    for pattern, model in MODEL_RULES:
        if re.search(pattern, card):
            return model
    return None


def library_data(library_path):
    """The rules from `sopt --library-json`, with their comments and section headings from library/rewrites.txt."""
    with open(library_path) as f:
        lib = json.load(f)
    lines = open(os.path.join(ROOT, "library", "rewrites.txt"), encoding="utf-8").read().splitlines()
    heading_at = {}  # line number -> the comment block above it (after a blank line)
    heading = ""
    block = []
    for i, line in enumerate(lines, 1):
        if line.startswith("#"):
            block.append(line.lstrip("#").strip())
        elif not line.strip():
            if block:
                heading = " ".join(b for b in block if b)
            block = []
        else:
            if block:
                heading = " ".join(b for b in block if b)
                block = []
            heading_at[i] = heading
    for r in lib["rules"]:
        n = int(r["source"].rsplit(":", 1)[1])
        raw = lines[n - 1]
        r["line"] = n
        r["comment"] = raw.split("#", 1)[1].strip() if "#" in raw else ""
        r["where"] = r["text"].split(" where ", 1)[1].strip() if " where " in r["text"] else ""
        r["section"] = heading_at.get(n, "")
        del r["source"]
    return lib


TB_SKIP_CONFIGS = {"dep"}


def texbench_data(out):
    """The latest TexBench report per card (0.5.0 with the fixed pixel order test), grouped by section."""
    best = {}
    for path in sorted(glob.glob(os.path.join(ROOT, "docs", "texbench", "*.csv"))):
        info = read_report(path)
        lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
        rows = list(csv.reader(l for l in lines if l and not l.startswith("#")))
        if not rows or rows[0][:3] != ["config", "test", "section"]:
            continue
        if sum(1 for r in rows if r and r[0] == "order") < 20:
            continue  # before the pixel order fix
        name = clean_name(NAME_BY_DEVICE.get(info.get("device", "")) or info.get("gpu", ""))
        base = os.path.basename(path)[:-4]
        m = re.search(r"-v(\d+)$", base)
        rank = int(m.group(1)) if m else 0
        if name not in best or rank > best[name][0]:
            best[name] = (rank, path, base, info, rows, lines)
    cards = []
    for name, (rank, path, base, info, rows, lines) in sorted(best.items()):
        hdr = {k: v for k, v in (re.match(r"# ([^:]+): (.*)", l).groups() for l in lines if re.match(r"# [^:]+: ", l))}
        col = {c: i for i, c in enumerate(rows[0])}
        sections = {}
        order = []
        for r in rows[1:]:
            if len(r) < len(rows[0]) - 1:
                continue
            cfg, test, sec = r[0], r[1], r[2]
            if cfg in TB_SKIP_CONFIGS or not sec:
                continue
            note = r[col["note"]] if "note" in col else ""
            if cfg == "order":
                order.append({"test": test, "value": r[col["units"]] or r[col["units_vs_base"]], "note": note})
                continue
            s = sections.setdefault(sec, {"title": sec, "kind": cfg if cfg in ("write", "blend", "pass") else "cost", "rows": {}})
            row = s["rows"].setdefault(test, {"test": test, "note": note})
            try:
                if cfg in ("write", "blend", "pass"):
                    row["value"] = float(r[col["units"]])
                else:
                    row[cfg] = float(r[col["units_vs_base"]]) / 4.0  # fma units
            except ValueError:
                pass
        card = {
            "name": name,
            "driver": info.get("driver", ""),
            "version": info.get("version", ""),
            "runTime": hdr.get("run time", ""),
            "scores": {k: hdr[k] for k in ("texture rate", "pixel fill rate", "memory bandwidth (writes)") if k in hdr},
            "sections": [dict(s, rows=list(s["rows"].values())) for s in sections.values()],
            "order": order,
        }
        img = os.path.join(ROOT, "docs", "texbench", base + "-order.png")
        if os.path.exists(img):
            shutil.copy(img, os.path.join(out, "img", base + "-order.png"))
            card["orderImage"] = "img/" + base + "-order.png"
            zoom = os.path.join(ROOT, "docs", "texbench", base + "-order-zoom.png")
            if os.path.exists(zoom):
                shutil.copy(zoom, os.path.join(out, "img", base + "-order-zoom.png"))
                card["orderZoom"] = "img/" + base + "-order-zoom.png"
        cards.append(card)
    return {"cards": cards}


def markdown_html(text):
    """The Markdown subset docs/texbench/FINDINGS.md uses: headings, paragraphs, bullet lists, tables, **bold**,
    *italic*, `code`. The first heading is left out (the page has its own title)."""
    def inline(t):
        t = html.escape(t, quote=False)
        t = re.sub(r"`([^`]+)`", r"<code>\1</code>", t)
        t = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", t)
        t = re.sub(r"\*([^*]+)\*", r"<em>\1</em>", t)
        return t
    out, para, items, rows = [], [], [], []
    def flush():
        if para:
            out.append("<p>" + inline(" ".join(para)) + "</p>")
            para.clear()
        if items:
            out.append("<ul>" + "".join("<li>" + inline(i) + "</li>" for i in items) + "</ul>")
            items.clear()
        if rows:
            cells = [[c.strip() for c in r.strip("|").split("|")] for r in rows if not re.match(r"^\|[-| ]+\|$", r)]
            head, body = cells[0], cells[1:]
            out.append('<div class="tablewrap"><table><thead><tr>' + "".join("<th>" + inline(c) + "</th>" for c in head) +
                       "</tr></thead><tbody>" + "".join("<tr>" + "".join("<td>" + inline(c) + "</td>" for c in r) + "</tr>" for r in body) +
                       "</tbody></table></div>")
            rows.clear()
    first = True
    for line in text.splitlines():
        m = re.match(r"^(#{1,4}) (.*)", line)
        if m:
            flush()
            if not first:
                n = len(m.group(1)) + 1
                out.append(f"<h{n}>{inline(m.group(2))}</h{n}>")
            first = False
        elif line.startswith("|"):
            if para or items:
                flush()
            rows.append(line)
        elif line.startswith("- "):
            if para or rows:
                flush()
            items.append(line[2:])
        elif line.startswith("  ") and items:
            items[-1] += " " + line.strip()
        elif not line.strip():
            flush()
        else:
            if items or rows:
                flush()
            para.append(line.strip())
    flush()
    return "\n".join(out)


def main():
    models_path, library_path, out = sys.argv[1], sys.argv[2], sys.argv[3]
    with open(models_path) as f:
        data = json.load(f)
    with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "architectures.json")) as f:
        data["coverage"] = json.load(f)["vendors"]
    cards = {}  # card name -> {model, reports, drivers}
    for path in sorted(glob.glob(os.path.join(ROOT, "docs", "opbench", "*.csv"))):
        info = read_report(path)
        if not info.get("gpu") or info.get("device", "").upper().replace("0X", "0x") in SKIP_DEVICES:
            continue
        base = os.path.basename(path)
        name = NAME_BY_FILE.get(base) or NAME_BY_DEVICE.get(info.get("device", "")) or info["gpu"]
        name = clean_name(name)
        c = cards.setdefault(name, {"name": name, "model": model_of(name), "reports": 0, "drivers": []})
        c["reports"] += 1
        drv = info.get("driver")
        if drv and drv not in c["drivers"]:
            c["drivers"].append(drv)
    data["cards"] = sorted(cards.values(), key=lambda c: c["name"])

    os.makedirs(os.path.join(out, "img"), exist_ok=True)
    os.makedirs(os.path.join(out, "data"), exist_ok=True)
    tb = texbench_data(out)
    lib = library_data(library_path)

    # The page files (site/ as it is), then the data.
    for dirpath, _, files in os.walk(os.path.join(ROOT, "site")):
        rel = os.path.relpath(dirpath, os.path.join(ROOT, "site"))
        os.makedirs(os.path.join(out, rel), exist_ok=True)
        for f in files:
            shutil.copy(os.path.join(dirpath, f), os.path.join(out, rel, f))
    for name, obj in (("models", data), ("library", lib), ("texbench", tb)):
        with open(os.path.join(out, "data", name + ".json"), "w") as f:
            json.dump(obj, f, indent=1)
    findings = os.path.join(ROOT, "docs", "texbench", "FINDINGS.md")
    if os.path.exists(findings):
        with open(findings, encoding="utf-8") as f, open(os.path.join(out, "data", "texbench-findings.html"), "w", encoding="utf-8") as o:
            o.write(markdown_html(f.read()))
    print(f"{len(data['models'])} models, {len(data['cards'])} cards, {len(lib['rules'])} library rules, "
          f"{len(tb['cards'])} TexBench cards -> {out}")


if __name__ == "__main__":
    main()
