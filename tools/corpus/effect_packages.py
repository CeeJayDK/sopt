#!/usr/bin/env python3
"""Installs every package in ReShade's own effect list (EffectPackages.ini) the way ReShade's setup does, then runs
sopt-fx on each package's effects and writes a summary (crashes, parse failures, variant checks, savings).

  effect_packages.py install EffectPackages.ini DEST      -> DEST/reshade-shaders/Shaders/..., DEST/packages.json
  effect_packages.py run DEST SOPT_FX OUT [--time S] [--timeout S]  -> OUT/<n>/ reports, OUT/summary.md

Setup's rule (setup/MainWindow.xaml.cs): the archive's first folder named Shaders, else the shortest folder holding an
.fx file, is copied to the package's InstallPath; DenyEffectFiles are removed. Textures are not needed by sopt.
"""
import configparser, io, json, os, re, shutil, subprocess, sys, tempfile, time, urllib.request, zipfile


def packages(ini):
    cp = configparser.ConfigParser(interpolation=None, strict=False)
    cp.optionxform = str
    cp.read(ini, encoding="utf-8-sig")
    return [dict(cp[s], id=s) for s in cp.sections() if "DownloadUrl" in cp[s]]


def shaders_dir(root):
    found = []
    for d, dirs, files in os.walk(root):
        depth = d[len(root):].count(os.sep)
        if os.path.basename(d).lower() == "shaders":
            found.append((0, depth, len(d), d))
        elif any(f.lower().endswith(".fx") for f in files):
            found.append((1, len(d), 0, d))
    return min(found)[3] if found else None


def install(ini, dest):
    out = []
    for p in packages(ini):
        rel = p["InstallPath"].replace("\\", "/").lstrip("./")
        target = os.path.join(dest, rel)
        entry = {"id": p["id"], "name": p["PackageName"], "url": p["RepositoryUrl"], "effects": [], "error": ""}
        try:
            data = urllib.request.urlopen(p["DownloadUrl"], timeout=120).read()
            with tempfile.TemporaryDirectory() as tmp:
                zipfile.ZipFile(io.BytesIO(data)).extractall(tmp)  # zipfile strips absolute and .. paths
                src = shaders_dir(tmp)
                if not src:
                    raise RuntimeError("no Shaders folder and no .fx file")
                deny = {f.strip().lower() for f in p.get("DenyEffectFiles", "").split(",") if f.strip()}
                for d, _, files in os.walk(src):
                    for f in files:
                        if f.lower() in deny:
                            continue
                        s = os.path.join(d, f)
                        t = os.path.join(target, os.path.relpath(s, src))
                        os.makedirs(os.path.dirname(t), exist_ok=True)
                        shutil.copyfile(s, t)
                        if f.lower().endswith(".fx"):
                            entry["effects"].append(os.path.relpath(t, dest))
        except Exception as e:  # one broken package must not stop the others
            entry["error"] = str(e)
        print(f"[{p['id']}] {p['PackageName']}: {len(entry['effects'])} effects {entry['error']}", flush=True)
        out.append(entry)
    json.dump(out, open(os.path.join(dest, "packages.json"), "w"), indent=1)


def summary_of(log):
    def grab(pattern):
        m = re.search(pattern, log)
        return m.groups() if m else None
    parsed = grab(r"(\d+) effects parsed, (\d+) failed, (\d+) regions")
    checks = grab(r"variant check: (\d+) of (\d+) parses failed")
    saved = grab(r"SweetOpt saved: (\d+) regions? in \d+ files?, cost (\d+) -> (\d+)")
    return parsed, checks, saved


def run(dest, sopt, out, secs, timeout):
    pk = json.load(open(os.path.join(dest, "packages.json")))
    os.makedirs(out, exist_ok=True)
    rows, bad = [], 0
    for p in pk:
        if not p["effects"]:
            rows.append(f"| {p['id']} | [{p['name']}]({p['url']}) | 0 | | | | | {p['error'] or 'no effects'} |")
            continue
        o = os.path.join(out, p["id"])
        cmd = [sopt, "-o", os.path.join(o, "sopt-out"), "--time", str(secs)] + [os.path.join(dest, e) for e in p["effects"]]
        t0 = time.time()
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, errors="replace", timeout=timeout)
            log, code = r.stdout + r.stderr, r.returncode
        except subprocess.TimeoutExpired as e:
            part = e.stdout or b""  # bytes even with text=True
            log, code = part.decode(errors="replace") if isinstance(part, bytes) else part, "timeout"
        secs_used = time.time() - t0
        os.makedirs(o, exist_ok=True)
        open(os.path.join(o, "log.txt"), "w", encoding="utf-8").write(log)
        for f in ("sopt-report.md", "sopt-found.txt", "sopt-facts.txt"):  # reports only: the shaders stay their authors'
            if os.path.exists(os.path.join(o, "sopt-out", f)):
                shutil.copyfile(os.path.join(o, "sopt-out", f), os.path.join(o, f))
        shutil.rmtree(os.path.join(o, "sopt-out"), ignore_errors=True)
        parsed, checks, saved = summary_of(log)
        problem = []
        if code != 0:
            problem.append(f"**exit {code}**")
        if checks and checks[0] != "0":
            problem.append(f"**{checks[0]} of {checks[1]} variant checks failed**")
        bad += bool(problem)
        save = f"{saved[0]} regions, {saved[1]} -> {saved[2]}" if saved else ""
        rows.append(f"| {p['id']} | [{p['name']}]({p['url']}) | {len(p['effects'])} | {parsed[0] + ' / ' + parsed[1] if parsed else '?'}"
                    f" | {parsed[2] if parsed else '?'} | {save} | {secs_used:.0f} s | {', '.join(problem)} |")
        print(rows[-1], flush=True)
    md = ("# sopt-fx on ReShade's effect packages\n\nParsed / failed: effects; savings: regions with written variants,"
          " cost on the default model. Problems: sopt-fx exited with an error (crash, timeout) or a written variant"
          " does not parse.\n\n| # | package | effects | parsed / failed | regions | saved | time | problems |\n"
          "|---|---|---|---|---|---|---|---|\n" + "\n".join(rows) + "\n")
    open(os.path.join(out, "summary.md"), "w", encoding="utf-8").write(md)
    return bad


if __name__ == "__main__":
    a = sys.argv[1:]
    if a[:1] == ["install"]:
        install(a[1], a[2])
    elif a[:1] == ["run"]:
        opt = dict(zip(a[4::2], a[5::2]))
        sys.exit(1 if run(a[1], a[2], a[3], float(opt.get("--time", 1)), float(opt.get("--timeout", 1800))) else 0)
    else:
        sys.exit(__doc__)
