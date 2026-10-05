#!/usr/bin/env python3
"""Builds the GPU Blueprint web page (owner, 2026-10-05): the cost models SweetOpt uses, the cards behind them and
the pixel order pictures, published with GitHub Pages by .github/workflows/pages.yml.

    build.py <models.json from `sopt --cost-models-json`> <output folder>

Only hardware facts are published (card, vendor / device ID, driver version): no names of the people who ran the
reports (GDPR). Reports that cannot be tied to a model (software renderers, cards without a model yet) are listed
as "not modelled yet".
"""
import csv
import glob
import json
import os
import re
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
    (r"RX 6\d\d\d|Radeon 6[68]0M", "amd-rdna2"),
    (r"Vega", "amd-gcn5"),
    (r"HD [67]\d\d\dM?\b", "amd-terascale2"),
    (r"Iris\S* (Graphics )?5\d\d|U?HD Graphics 6\d\d|HD Graphics 5\d\d", "intel-gen9"),
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


def main():
    models_path, out = sys.argv[1], sys.argv[2]
    with open(models_path) as f:
        data = json.load(f)
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
    images = []
    for path in sorted(glob.glob(os.path.join(ROOT, "docs", "texbench", "*-order.png"))):
        base = os.path.basename(path)[: -len("-order.png")]
        csv_path = os.path.join(ROOT, "docs", "texbench", base + ".csv")
        info = read_report(csv_path) if os.path.exists(csv_path) else {}
        name = clean_name(NAME_BY_DEVICE.get(info.get("device", "")) or info.get("gpu") or base)
        entry = {"card": name, "order": "img/" + base + "-order.png"}
        shutil.copy(path, os.path.join(out, "img", base + "-order.png"))
        zoom = os.path.join(ROOT, "docs", "texbench", base + "-order-zoom.png")
        if os.path.exists(zoom):
            shutil.copy(zoom, os.path.join(out, "img", base + "-order-zoom.png"))
            entry["zoom"] = "img/" + base + "-order-zoom.png"
        images.append(entry)
    data["orderImages"] = images

    for f in ("index.html", "app.js", "style.css"):
        shutil.copy(os.path.join(ROOT, "site", f), os.path.join(out, f))
    with open(os.path.join(out, "data.json"), "w") as f:
        json.dump(data, f, indent=1)
    print(f"{len(data['models'])} models, {len(data['cards'])} cards, {len(images)} pixel order pictures -> {out}")


if __name__ == "__main__":
    main()
