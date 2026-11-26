#!/usr/bin/env python3
"""How much does fxc's optimization level change ReShade effects? (owner, 2026-11-24)

ReShade compiles Direct3D shaders with fxc /O1 in its normal mode (what most people run) and /O3 in performance
mode (which also turns uniforms into constants). sopt and fxstat have judged code at /O3 only. This compiles every
effect of the installed packages with fxstat (Windows, Microsoft's d3dcompiler_47) in three ways per shader model and
compares the DXBC of each entry point:
  n1 = /O1, uniforms kept      (normal mode)
  n3 = /O3, uniforms kept      (only the optimization level differs from n1)
  p3 = /O3, uniforms constant  (performance mode)

    fxc_levels.py run <installed corpus> <fxstat.exe> <out.jsonl>
    fxc_levels.py summary <out.jsonl>...

Only counts, entry point names and hashes of the code are kept (the shaders stay their authors').
"""
import hashlib, json, os, subprocess, sys, tempfile

CONFIGS = {"n1": ["-O1", "--no-performance-mode"], "n3": ["-O3", "--no-performance-mode"],
           "p3": ["-O3", "--performance-mode"]}
MODELS = ["50", "30"]  # Direct3D 10-12 (sm50 like ReShade on D3D11) and Direct3D 9


def include_dirs(root):
    # ReShade's search paths end in \**: every folder with effect files is an include folder.
    return [d for d, _, files in os.walk(root) if any(f.lower().endswith((".fx", ".fxh")) for f in files)]


def compile_one(fxstat, fx, sm, cfg, incs):
    with tempfile.TemporaryDirectory() as tmp:
        cmd = [fxstat, "--dxbc", "--dxbc-compiler", "d3dcompiler", "--shader-model", sm] + CONFIGS[cfg] + \
              ["--json", "--dump", tmp]
        for d in incs:
            cmd += ["-I", d]
        try:
            r = subprocess.run(cmd + [fx], capture_output=True, text=True, errors="replace", timeout=600)
        except subprocess.TimeoutExpired:
            return {"error": "timeout"}
        try:
            rep = json.loads(r.stdout)
        except ValueError:
            return {"error": (r.stderr.strip().splitlines() or ["no output"])[-1][:200]}
        out = {}
        for e in rep.get("entry_points", []):
            asm = os.path.join(tmp, e["name"] + ".asm")
            code = ""
            if os.path.exists(asm):  # comment lines hold the compiler's header, not code
                code = "\n".join(l for l in open(asm, errors="replace").read().splitlines()
                                 if l.strip() and not l.lstrip().startswith("//"))
            out[e["name"]] = [e.get("total", 0), e.get("alu", 0), e.get("temp_registers", 0),
                              hashlib.sha1(code.encode()).hexdigest()[:12]]
        return out


def run(dest, fxstat, out):
    pk = json.load(open(os.path.join(dest, "packages.json")))
    incs = include_dirs(dest)
    fxstat = os.path.abspath(fxstat)  # Windows' CreateProcess does not find a relative path with / in it
    with open(out, "w") as f:
        for p in pk:
            if not p.get("run", True):
                continue
            for e in p["effects"]:
                fx = os.path.join(dest, e)
                row = {"package": p["id"], "effect": e.replace("\\", "/"), "models": {}}
                for sm in MODELS:
                    row["models"][sm] = {c: compile_one(fxstat, fx, sm, c, incs) for c in CONFIGS}
                f.write(json.dumps(row) + "\n")
                f.flush()
                print(p["id"], e, flush=True)


def summary(files):
    rows = [json.loads(l) for fn in files for l in open(fn) if l.strip()]
    print(f"# fxc /O1 vs /O3 on {len(rows)} effects\n")
    print("Normal mode = /O1, uniforms kept; performance mode = /O3, uniforms constant. DXBC instruction counts of each "
          "entry point (drivers compile DXBC again, so this is fxc's part only).\n")
    for sm in MODELS:
        for a, b, what in [("n1", "n3", "/O1 vs /O3 (uniforms kept)"), ("n1", "p3", "normal vs performance mode")]:
            same = samecount = worse = better = 0
            ta = tb = 0
            diffs = []
            for r in rows:
                m = r["models"].get(sm, {})
                x, y = m.get(a, {}), m.get(b, {})
                if "error" in x or "error" in y:
                    continue
                for name in x:
                    if name not in y:
                        continue
                    ca, cb = x[name][0], y[name][0]
                    ta += ca
                    tb += cb
                    if x[name][3] == y[name][3]:
                        same += 1
                    elif ca == cb:
                        samecount += 1
                    elif ca > cb:
                        worse += 1
                        diffs.append((ca - cb, r["effect"], name, ca, cb))
                    else:
                        better += 1
                        diffs.append((ca - cb, r["effect"], name, ca, cb))
            n = same + samecount + worse + better
            print(f"## sm{sm}: {what}\n")
            print(f"- entry points: {n}; identical code {same}, same count but other code {samecount}, "
                  f"more instructions at {a} {worse}, fewer at {a} {better}")
            print(f"- instructions in all: {a} {ta}, {b} {tb}" + (f" ({(ta - tb) * 100.0 / tb:+.1f}%)" if tb else ""))
            if diffs:
                print(f"\n| effect | entry point | {a} | {b} |\n|---|---|---|---|")
                for d in sorted(diffs, key=lambda d: -abs(d[0]))[:15]:
                    print(f"| {d[1]} | {d[2]} | {d[3]} | {d[4]} |")
            print()
    errs = {}
    for r in rows:
        for sm, m in r["models"].items():
            for c, v in m.items():
                if "error" in v:
                    errs[(sm, c)] = errs.get((sm, c), 0) + 1
    if errs:
        print("Not compiled (effect x model x mode): " + ", ".join(f"sm{s} {c} {n}" for (s, c), n in sorted(errs.items())))


if __name__ == "__main__":
    a = sys.argv[1:]
    if a[:1] == ["run"] and len(a) == 4:
        run(a[1], a[2], a[3])
    elif a[:1] == ["summary"]:
        summary(a[1:])
    else:
        sys.exit(__doc__)
