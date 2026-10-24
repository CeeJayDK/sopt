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
import statistics
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
    "0x1638": "AMD Radeon Vega (Cezanne)",
    "0x15D8": "AMD Radeon RX Vega 11 (Picasso)",
    "0x164E": "AMD Radeon Graphics (Raphael)",
    "0x13C0": "AMD Radeon Graphics (Granite Ridge)",
    "0x9A78": "Intel UHD Graphics (Tiger Lake)",  # Xe-LP, measured = Iris Xe (2026-10-13)
}
SKIP_DEVICES = {"0x008C"}  # Microsoft Basic Render Driver (software)

# Reports whose clock moved during the run (see CLAUDE.md): left out.
# OpBench tests whose step or base changed: readings from older reports measured something else and are left out
# (owner, 2026-10-15: the integer tests' base was an integer multiply until 0.7.4, cheaper ops hid under it).
REDEFINED = {
    (0, 7, 5): {"iadd", "imul", "iand", "imin", "ishr", "irot", "bitrev", "ishl", "ineg", "iabs", "bfe", "xor3", "or3",
                "andor", "bitsel", "maj3", "iadd3", "lshladd", "addlshl", "imad", "icmpsel", "udiv", "umod", "idiv",
                "imod", "itof", "utof", "unitf", "popc", "fbh", "fbl", "ftou"},
}


def reading_valid(version, test):
    """Whether a report of this OpBench version measured the test as it is defined now."""
    v = tuple(int(x) for x in str(version).split(".") if x.isdigit())
    return all(v >= since or test not in tests for since, tests in REDEFINED.items())


UNRELIABLE = {
    "nvidia-rtx-3050-laptop.csv", "nvidia-rtx-4070.csv", "nvidia-gtx-1660-ti.csv", "nvidia-gtx-1660-ti-2.csv",
    "nvidia-rtx-4090-laptop.csv", "nvidia-rtx-4090-laptop-dgpu.csv", "amd-radeon-680m-rembrandt.csv",
    "nvidia-rtx-4090-b.csv",  # 420% drift, 11 tests without consensus, fp32 43 of 83 TFLOPS: throttled
    "nvidia-rtx-4090-c.csv",  # 158% drift, fp32 40 of 83 TFLOPS: throttled (costs still = the first 4090)
    "nvidia-rtx-3090.csv",  # 178% drift, ~30% low on most tests (clock, not chip)
    "nvidia-rtx-3050-laptop-gpu.csv",  # 157% drift, many tests off the family
    "nvidia-gtx-1660-11.csv",  # 84-115% drift, 60+ tests off the family, fp32 4.1 of 5.0 TFLOPS (owner's second run, 2026-10-17)
}

# Card name -> cost model, first match wins.
MODEL_RULES = [
    (r"RTX 50\d\d", "nvidia-blackwell"),
    (r"RTX 40\d\d", "nvidia-ada"),
    (r"RTX 30\d\d|RTX 2050", "nvidia-ampere"),  # the RTX 2050 is an Ampere chip (GA107)
    (r"GTX 16\d\d", "nvidia-turing"),           # Turing GTX 16 (TU116 / TU117)
    (r"RTX 20\d\d", "nvidia-turing-rtx"),       # Turing RTX 20 (TU10x): fp16 also faster in pixel shaders
    (r"GTX 10\d\d|GT 10\d\d", "nvidia-pascal"),
    # Maxwell 2 (GM20x, SM52) before Maxwell 1 (GM107 / GM108, SM50: no hardware sqrt).
    (r"GTX (9[5-8]0( Ti)?|TITAN X|9(65|70|80)M)\b|Quadro M(2000|3000M|4000|5000M?|5500|6000)\b", "nvidia-maxwell2"),
    (r"GTX (745|750( Ti)?|8\d\dM|9[2-6][05]MX?)\b|MX1[13]0\b|Quadro (K620|K1200|K2200|M(500M|520|600M|620|1000M|1200|2000M))\b",
     "nvidia-maxwell1"),
    (r"RX 9\d\d\d", "amd-rdna4"),
    (r"RX 7\d\d\d", "rdna3"),
    (r"RX 6\d\d\d|Radeon 6[68]0M|Raphael|Granite Ridge", "amd-rdna2"),
    (r"Vega", "amd-gcn5"),
    (r"HD [67]\d\d\dM?\b", "amd-terascale2"),
    (r"Arc\S* B\d\d\d|Arc\S* 1[34]0V", "intel-xe2"),
    (r"Iris\S* Xe|UHD Graphics (7\d\d|\(Tiger Lake\))", "intel-gen12"),
    (r"Iris\S* (Graphics )?5\d\d|U?HD Graphics 6\d\d|HD Graphics 5\d\d", "intel-gen9"),
    (r"HD Graphics 4[2-6]\d\d|Iris\S* (Pro )?(Graphics )?5[12]00", "intel-gen7.5"),
    (r"HD Graphics [23]000\b", "intel-gen6"),  # Sandy Bridge
]


def read_report(path):
    """GPU name, vendor, device, driver and OpBench version of a report (header lines, or the old per-row format)."""
    info = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        lines = f.read().splitlines()
    for line in lines:
        m = re.match(r"# (OpBench|TexBench|StageBench) (\S+)", line)
        if m:
            info["tool"] = m.group(1)
            info["version"] = m.group(2)
        m = re.match(r"# (gpu|vendor|device|driver|stage|run time): (.*)", line)
        if m:
            info[m.group(1)] = m.group(2).strip()
        m = re.match(r"# reference drift(?: tput)?: (\S+)", line)
        if m and "drift" not in info:
            info["drift"] = m.group(1)
    info["rows"] = max(0, sum(1 for l in lines if l and not l.startswith("#")) - 1)  # tests (one header line)
    if "gpu" not in info:  # version 1: gpu,vendor,device,driver on every row
        rows = [r for r in csv.reader(l for l in lines if l and not l.startswith("#"))]
        if len(rows) > 1 and rows[0][:4] == ["gpu", "vendor", "device", "driver"]:
            info.update(gpu=rows[1][0], vendor=rows[1][1], device=rows[1][2], driver=rows[1][3], version="0.0")
    return info


def drift_of(info):
    try:
        return float(info.get("drift", "").rstrip("%"))
    except ValueError:
        return float("inf")


def pick(reports, key, info):
    """Which of one card's reports to use (owner, 2026-10-24): the newest by key, or rather, among the reports with the
    newest one's tool version (key[0]) and driver, the one whose clock moved least: a noisy rerun must not replace a
    clean run. Other versions and drivers stay out (other tests, other measurements)."""
    top = max(reports, key=key)
    same = [r for r in reports if key(r)[0] == key(top)[0] and info(r).get("driver") == info(top).get("driver")]
    return min(same, key=lambda r: drift_of(info(r)))


def by_cleanest(reports, key, info):
    """One card's reports in the order to read them test by test, later ones winning (owner, 2026-10-24: each test from
    the cleanest run of the same driver; tests change between versions, and reading_valid already leaves out readings
    of a test that changed): reports with another driver than the newest first (by key), then those with its driver
    from the most to the least drift."""
    drv = info(max(reports, key=key)).get("driver")
    return sorted(reports, key=lambda r: (info(r).get("driver") == drv,
                                          -drift_of(info(r)) if info(r).get("driver") == drv else 0.0, key(r)))


def clean_name(name):
    name = re.sub(r"\((R|TM)\)", "", name)
    name = re.sub(r"\s+", " ", name).strip()
    name = re.sub(r" Series$", "", name)  # "Radeon RX 590 Series" -> "AMD Radeon RX 590"
    return "AMD " + name if name.startswith("Radeon") else name


def section_title(lines):
    """A library comment block as a short heading and the rest as its description: the first line up to a source note
    in parentheses or an explanation after a colon; notes such as "(owner, 2026-10-02; docs/opbench/)" are dropped.
    A formula heading (lerp(a, b, t) = ...) keeps its parentheses."""
    owner = r"\s*\([^()]*\bowner\b[^()]*\)"
    text = re.sub(owner, "", " ".join(lines)).strip()
    title = re.sub(owner, "", lines[0]).strip()
    title = re.sub(r"\s*\([A-Z][^()=*+]*\)\s*$", "", title)  # a trailing source note such as (DXC)
    head = title.split(" (", 1)[0]
    if " = " not in head:
        title = head
    title = re.split(r"(?<! ): |\. ", title, maxsplit=1)[0].rstrip(":.;,")  # not the colon of "? 1 : 0"
    about = text[len(title):].lstrip(" :.").strip() if text.startswith(title) else text
    if re.match(r"[a-z]{2}|a ", title) and " = " not in title:  # a word, not x * 2^n or a formula
        title = title[:1].upper() + title[1:]
    return title, about


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
    heading = []
    block = []
    for i, line in enumerate(lines, 1):
        if line.startswith("#"):
            block.append(line.lstrip("#").strip())
        elif not line.strip():
            if block:
                heading = [b for b in block if b]
            block = []
        else:
            if block:
                heading = [b for b in block if b]
                block = []
            heading_at[i] = heading
    for r in lib["rules"]:
        n = int(r["source"].rsplit(":", 1)[1])
        raw = lines[n - 1]
        r["line"] = n
        r["comment"] = raw.split("#", 1)[1].strip() if "#" in raw else ""
        m = re.match(r"\[([^\]]+)\]\s*", r["comment"])  # [name]: the rule's short name on the page
        r["name"] = m.group(1) if m else ""
        r["comment"] = r["comment"][m.end():] if m else r["comment"]
        r["where"] = r["text"].split(" where ", 1)[1].strip() if " where " in r["text"] else ""
        r["section"], r["about"] = section_title(heading_at[n]) if heading_at.get(n) else ("", "")
        del r["source"]
    return lib


TB_SKIP_CONFIGS = {"dep"}


# Tests renamed in TexBench 0.7.1 (owner, 2026-10-12: 1D is linear, 3D trilinear): old reports join the new rows.
TB_RENAMES = {"tex1D bilinear": "tex1D linear", "tex3D linear": "tex3D trilinear"}


# OpBench tests that measure memory, barriers or branches (not the math unit), or swing on clean runs: not compared.
NOT_MATH = re.compile(r"^(a[A-Z]|atom|gs|group|mem|barrier|array|constarray|branch|selectboth|transpose)")


def driver_label(driver, vendor):
    """NVIDIA's Windows version 32.0.16.1656 as the driver people know (616.56); others as they are."""
    parts = driver.split(".")
    if vendor.upper() == "0X10DE" and len(parts) == 4:
        s = (parts[2] + parts[3].zfill(4))[-5:]
        return s[:3] + "." + s[3:]
    return driver


def opbench_extras():
    """Driver splits and fp16 per family (owner, 2026-10-12: newer drivers that made something faster tell people to
    update and show the vendor still works on the card; fp16 tells developers whether to support it).
    Clean reports only (neg ~0, not UNRELIABLE); for fp16 the newest report per card."""
    reps = []
    for path in sorted(glob.glob(os.path.join(ROOT, "docs", "opbench", "*.csv"))):
        base = os.path.basename(path)
        info = read_report(path)
        if base in UNRELIABLE or not info.get("driver") or info.get("device", "").upper().replace("0X", "0x") in SKIP_DEVICES:
            continue
        name = clean_name(NAME_BY_FILE.get(base) or NAME_BY_DEVICE.get(info.get("device", "")) or info.get("gpu", ""))
        model = model_of(name)
        lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
        rows = [r for r in csv.DictReader(l for l in lines if l and not l.startswith("#")) if r.get("config") == "tput"]
        vals, units, steps = {}, {}, {}
        for r in rows:
            if not reading_valid(info.get("version", "0"), r.get("test", "")):
                continue
            try:
                units[r["test"]] = float(r["units"])
                if r.get("consensus") == "yes":
                    vals[r["test"]] = float(r["units_vs_base"])
                    steps[r["test"]] = r.get("step", "")
            except (KeyError, ValueError):
                pass
        if not model or abs(vals.get("neg", 9.0)) > 0.3:
            continue
        half = next((l.split(":", 1)[1].strip() for l in lines if l.startswith("# min16float:")), "")
        m = re.search(r"-v(\d+)$", base[:-4])
        key = (tuple(int(x) for x in info.get("version", "0").split(".") if x.isdigit()), int(m.group(1)) if m else 0)
        reps.append({"name": name, "model": model, "vendor": info.get("vendor", ""), "key": key,
                     "driver": info.get("driver", ""), "drift": info.get("drift", ""),
                     "drv": tuple(int(x) for x in info["driver"].split(".") if x.isdigit()),
                     "label": driver_label(info["driver"], info.get("vendor", "")),
                     "vals": {t: v for t, v in vals.items() if not NOT_MATH.match(t)}, "all": vals, "steps": steps,
                     "units": units, "half": half})

    # Driver splits: in one family, a test whose readings fall into a low and a high group that the driver version
    # separates (all reports of one driver alike; every older driver on one side). Splits with the same drivers on
    # each side are one row.
    splits = {}
    fams = {}
    for r in reps:
        fams.setdefault(r["model"], []).append(r)
    for model, rs in fams.items():
        for t in sorted({t for r in rs for t in r["vals"]}):
            pts = sorted(((r["drv"], r["vals"][t], r) for r in rs if t in r["vals"]), key=lambda p: p[:2])
            if len({p[0] for p in pts}) < 2:
                continue
            v = [p[1] for p in pts]
            lo, hi = min(v), max(v)
            if hi - lo < max(1.5, 0.3 * max(abs(hi), 1.0)):
                continue
            high = [x > (lo + hi) / 2 for x in v]
            byDrv = {}
            for p, h in zip(pts, high):
                byDrv.setdefault(p[0], set()).add(h)
            if any(len(s) > 1 for s in byDrv.values()):
                continue
            cut = next(k for k in range(1, len(high)) if high[k] != high[0])
            if any(high[k] != high[0] for k in range(cut)) or any(high[k] == high[0] for k in range(cut, len(high))):
                continue
            old, new = pts[:cut], pts[cut:]
            mean = lambda ps: sum(p[1] for p in ps) / len(ps)
            by_version = lambda ds: sorted(ds, key=lambda d: [int(x) for x in re.findall(r"\d+", d)])
            side = lambda ps: {"cards": sorted({p[2]["name"] for p in ps}), "drivers": by_version({p[2]["label"] for p in ps})}
            key = (model, tuple(side(old)["drivers"]))  # one row per older driver (newer ones merged)
            row = splits.setdefault(key, {"model": model, "older": side(old), "newer": {"cards": [], "drivers": []}, "tests": []})
            row["newer"]["cards"] = sorted(set(row["newer"]["cards"]) | set(side(new)["cards"]))
            row["newer"]["drivers"] = by_version(set(row["newer"]["drivers"]) | set(side(new)["drivers"]))
            row["tests"].append({"test": t, "older": round(mean(old) / 4, 2), "newer": round(mean(new) / 4, 2)})
    drivers = []
    for row in splits.values():
        faster = sum(1 for t in row["tests"] if t["newer"] < t["older"])
        row["newerFaster"] = faster * 2 >= len(row["tests"])
        drivers.append(row)

    # fp16: speed of min16float against the same work in fp32, family median over its cards (one report each: pick).
    newest = best_reports(reps)
    pairs = {"fma": ("mad", "mad16"), "vec3": ("mad3v", "mad16v3"), "add": ("add", "add16"), "rcp": ("rcp", "rcp16")}
    fp16 = {}
    for r in newest.values():
        f = fp16.setdefault(r["model"], {"model": r["model"], "cards": [], "native": [], "ratios": {}})
        f["cards"].append(r["name"])
        f["native"].append(r["half"].startswith("16"))
        for k, (a, b) in pairs.items():
            if r["units"].get(a, 0) > 0 and r["units"].get(b, 0) > 0:
                f["ratios"].setdefault(k, []).append(r["units"][a] / r["units"][b])
    for f in fp16.values():
        f["native"] = all(f["native"])
        f["speedup"] = {k: round(statistics.median(v), 2) for k, v in f.pop("ratios").items()}
        f["cards"].sort()
    return {"drivers": drivers, "fp16": sorted(fp16.values(), key=lambda f: f["model"]), "alltests": all_tests(reps)}


def display_order():
    """OpBench's summary sections and test order: kDisplayOrder in sopt_opbench.cpp ("#" entries start a section)."""
    with open(os.path.join(ROOT, "tools", "windows", "opbench", "sopt_opbench.cpp"), encoding="utf-8") as f:
        body = f.read().split("kDisplayOrder[] = {", 1)[1].split("};", 1)[0]
    sections = []
    for name in re.findall(r'"([^"]*)"', re.sub(r"//.*", "", body)):
        if name.startswith("#"):
            sections.append({"title": name[1:], "tests": []})
        else:
            sections[-1]["tests"].append(name)
    return sections


def best_reports(reps):
    """Card name -> its report to use (pick) among opbench_extras' reports."""
    by = {}
    for r in reps:
        by.setdefault(r["name"], []).append(r)
    return {name: pick(rs, lambda r: r["key"], lambda r: r) for name, rs in by.items()}


def all_tests(reps):
    """Every OpBench test per family (owner, 2026-10-16): the family median of each card's newest clean reading, in fma
    units over the test's base, rows in OpBench's own order."""
    latest, steps = {}, {}  # (card, test) -> (model, value)
    cards = {}
    for r in reps:
        cards.setdefault(r["name"], []).append(r)
    for r in [r for rs in cards.values() for r in by_cleanest(rs, lambda r: r["key"], lambda r: r)]:
        for t, v in r["all"].items():
            latest[(r["name"], t)] = (r["model"], v)
            steps[t] = r["steps"][t]
    per = {}
    for (card, t), (model, v) in latest.items():
        per.setdefault(t, {}).setdefault(model, []).append(v)
    sections = []
    for s in display_order():
        rows = [{"test": t, "step": steps[t], "v": {m: round(statistics.median(v) / 4, 2) for m, v in per[t].items()}}
                for t in s["tests"] if t in per]
        if rows:
            sections.append({"title": s["title"], "rows": rows})
    cards = {}
    for (card, _), (model, _) in latest.items():
        cards.setdefault(model, set()).add(card)
    families = [{"model": m, "cards": sorted(cards[m])} for m in dict.fromkeys(m for _, m in MODEL_RULES) if m in cards]
    return {"families": families, "sections": sections}


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
        version = tuple(int(x) for x in info.get("version", "0").split(".") if x.isdigit())
        best.setdefault(name, []).append(((version, rank), path, base, info, rows, lines))
    cards = []
    for name, (rank, path, base, info, rows, lines) in sorted((n, pick(rs, lambda r: r[0], lambda r: r[3])) for n, rs in best.items()):
        hdr = {k: v for k, v in (re.match(r"# ([^:]+): (.*)", l).groups() for l in lines if re.match(r"# [^:]+: ", l))}
        version = tuple(int(x) for x in info.get("version", "0").split(".") if x.isdigit())
        col = {c: i for i, c in enumerate(rows[0])}
        sections = {}
        order = []
        for r in rows[1:]:
            if len(r) < len(rows[0]) - 1:
                continue
            cfg, test, sec = r[0], TB_RENAMES.get(r[1], r[1]), r[2].replace("Do this, not that:", "Recommendation:")  # name until 0.7.14
            if test.endswith(" write smooth") and version < (0, 7, 7):
                continue  # alpha was a gradient until 0.7.6 (now 1, what effects write)
            if cfg in TB_SKIP_CONFIGS or not sec:
                continue
            note = r[col["note"]] if "note" in col else ""
            if cfg == "order":
                order.append({"test": test, "value": r[col["units"]] or r[col["units_vs_base"]], "note": note})
                continue
            s = sections.setdefault(sec, {"title": sec, "kind": cfg if cfg in ("write", "blend", "pass") else "cost", "rows": {}})
            row = s["rows"].setdefault(test, {"test": test, "note": note,
                                              "step": r[col["step"]] if "step" in col and len(r) > col["step"] else ""})
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


def stagebench_data():
    """Per card with StageBench reports: each test's throughput cost (fma units) in compute shaders (OpBench), pixel and
    vertex shaders (StageBench), from the newest report of each. Runs that switched to CPU timing part way are left out."""
    steps = {}  # test -> the HLSL step it times
    def costs(path):
        lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
        if any(l.startswith("# timing:") and "from the timestamp check" not in l for l in lines):
            return None  # mixed GPU / CPU timing
        rows = list(csv.reader(l for l in lines if l and not l.startswith("#")))
        if not rows or rows[0][:2] != ["config", "test"]:
            return None
        col = {c: i for i, c in enumerate(rows[0])}
        out = {}
        for r in rows[1:]:
            if "step" in col and len(r) > col["step"]:
                steps.setdefault(r[1], r[col["step"]])
            if len(r) > col["units_vs_base"] and r[0] == "tput":
                try:
                    out[r[1]] = float(r[col["units_vs_base"]]) / 4.0
                except ValueError:
                    pass
        return out

    def newest(kind, stage=None):
        best = {}
        for path in glob.glob(os.path.join(ROOT, "docs", kind, "*.csv")):
            info = read_report(path)
            if stage and info.get("stage") != stage:
                continue
            m = re.search(r"-(\d+)\.csv$", path)
            key = (tuple(int(x) for x in info.get("version", "0").split(".") if x.isdigit()), int(m.group(1)) if m else 1)
            name = clean_name(NAME_BY_DEVICE.get(info.get("device", "")) or info.get("gpu", ""))
            c = costs(path)
            if name and c:
                best.setdefault(name, []).append((key, c, info))
        return {name: pick(rs, lambda r: r[0], lambda r: r[2]) for name, rs in best.items()}

    ps, vs, cs = newest("stagebench", "ps"), newest("stagebench", "vs"), newest("opbench")
    cards = []
    for name in sorted(set(ps) & set(vs)):
        tests = []
        for test, p in ps[name][1].items():
            tests.append({"test": test, "step": steps.get(test, ""), "cs": cs.get(name, (0, {}))[1].get(test), "ps": p,
                          "vs": vs[name][1].get(test)})
        cards.append({"name": name, "driver": ps[name][2].get("driver", ""), "version": ps[name][2].get("version", ""),
                      "drift": {k: v[name][2].get("drift", "") for k, v in (("cs", cs), ("ps", ps), ("vs", vs)) if name in v},
                      "tests": tests})
    return {"cards": cards}


def run_times():
    """Median run time in seconds per card and tool, over the reports with the newest set of tests: a report counts when
    its number of tests is within 10% of the newest version's (0.8.0 and 0.9.0 alike, 0.7.0 had about half), so the
    times of different cards compare. StageBench: the pixel and the vertex shader run together."""
    out = {}
    for tool, kind in (("OpBench", "opbench"), ("StageBench", "stagebench"), ("TexBench", "texbench")):
        per = {}
        for path in glob.glob(os.path.join(ROOT, "docs", kind, "*.csv")):
            info = read_report(path)
            secs = re.match(r"(\d+)", info.get("run time", ""))
            if not secs:
                continue
            name = clean_name(NAME_BY_FILE.get(os.path.basename(path)) or NAME_BY_DEVICE.get(info.get("device", "")) or info.get("gpu", ""))
            version = tuple(int(x) for x in info.get("version", "0").split(".") if x.isdigit())
            per.setdefault((name, info.get("stage", "")), []).append((version, info["rows"], int(secs.group(1))))
        latest = max((v for reps in per.values() for v, _, _ in reps), default=())
        tests = statistics.median([n for reps in per.values() for v, n, _ in reps if v == latest] or [0])
        parts = {}  # card -> one median per stage
        for (name, _), reps in per.items():
            same = [s for _, n, s in reps if abs(n - tests) <= 0.1 * tests]
            if same:
                parts.setdefault(name, []).append(statistics.median(same))
        for name, p in parts.items():
            if len(p) == (2 if tool == "StageBench" else 1):  # StageBench: both runs, or it would look twice as fast
                out.setdefault(name, {})[tool] = sum(p)
    return out


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
    times = run_times()
    for c in cards.values():
        c["times"] = {k: round(v) for k, v in times.get(c["name"], {}).items()}
    data["cards"] = sorted(cards.values(), key=lambda c: c["name"])
    data.update(opbench_extras())

    os.makedirs(os.path.join(out, "img"), exist_ok=True)
    os.makedirs(os.path.join(out, "data"), exist_ok=True)
    tb = texbench_data(out)
    sb = stagebench_data()
    lib = library_data(library_path)

    # The page files (site/ as it is), then the data.
    for dirpath, _, files in os.walk(os.path.join(ROOT, "site")):
        rel = os.path.relpath(dirpath, os.path.join(ROOT, "site"))
        os.makedirs(os.path.join(out, rel), exist_ok=True)
        for f in files:
            shutil.copy(os.path.join(dirpath, f), os.path.join(out, rel, f))
    for name, obj in (("models", data), ("library", lib), ("texbench", tb), ("stagebench", sb)):
        with open(os.path.join(out, "data", name + ".json"), "w") as f:
            json.dump(obj, f, indent=1)
    findings = os.path.join(ROOT, "docs", "texbench", "FINDINGS.md")
    if os.path.exists(findings):
        with open(findings, encoding="utf-8") as f, open(os.path.join(out, "data", "texbench-findings.html"), "w", encoding="utf-8") as o:
            o.write(markdown_html(f.read()))
    print(f"{len(data['models'])} models, {len(data['cards'])} cards, {len(lib['rules'])} library rules, "
          f"{len(tb['cards'])} TexBench cards, {len(sb['cards'])} StageBench cards -> {out}")


if __name__ == "__main__":
    main()
