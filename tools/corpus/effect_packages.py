#!/usr/bin/env python3
"""Installs every package in ReShade's own effect list (EffectPackages.ini) the way ReShade's setup does, then runs
sopt-fx on each package's effects and writes a summary (crashes, parse failures, variant checks, savings).

  effect_packages.py ids EffectPackages.ini [ID,ID...]   -> the package ids as JSON (for a job matrix)
  effect_packages.py install EffectPackages.ini DEST [ID,ID...]  -> DEST/reshade-shaders/Shaders/..., DEST/packages.json
  effect_packages.py run DEST SOPT_FX OUT [--time S] [--timeout S]  -> OUT/<n>/ reports, OUT/summary.md
  effect_packages.py summary OUT_DIR...                  -> one summary.md from several runs' (stdout); exit 1 on problems

install with ids installs those packages and the required ones (Required=1: the standard effects with ReShade.fxh),
which are then only there for the includes and not run unless asked for.

Setup's rule (setup/MainWindow.xaml.cs): the archive's first folder named Shaders, else the shortest folder holding an
.fx file, is copied to the package's InstallPath; DenyEffectFiles are removed. Textures are not needed by sopt.
"""
import configparser, glob, io, json, os, re, shlex, shutil, subprocess, sys, tempfile, time, urllib.request, zipfile


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


def chosen(ini, ids):
    want = set(ids.split(",")) if ids else None
    return [(p, want is None or p["id"] in want) for p in packages(ini) if want is None or p["id"] in want or p.get("Required") == "1"]


def install(ini, dest, ids=""):
    out = []
    for p, run in chosen(ini, ids):
        rel = p["InstallPath"].replace("\\", "/").lstrip("./")
        target = os.path.join(dest, rel)
        entry = {"id": p["id"], "name": p["PackageName"], "url": p["RepositoryUrl"], "effects": [], "error": "", "run": run}
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
    saved = grab(r"SweetOpt saved:\S* (\d+) of \d+ statements in \d+ files? cheaper: (\d+) -> (\d+)")
    return parsed, checks, saved


# Skip reasons in plain words (owner, 2026-10-26: "be better about saying why"). (prefix, nothing to do?, meaning);
# the first prefix that matches wins. "window: " (a multi-line piece) is stripped first.
EXPLAIN = [
    ("copy or constant", True, "no math: a copy, a constant or a table entry"),
    ("ReShade's copy of a call argument", True, "ReShade copies every argument of a function call; not the effect's own code"),
    ("one instruction (value used several times)", False,
     "one GPU instruction whose value several statements read: only a piece with several results could share a better form"),
    ("one instruction", True, "one GPU instruction: nothing cheaper exists"),
    ("input is a constant", True, "the compiler already works it out"),
    ("compile-time constant expression", True, "the compiler already works it out"),
    ("constant expression", True, "the compiler already works it out"),
    ("boolean value", False, "a true / false result: sopt searches only number results"),
    ("more than maxOps", False, "too much math in one piece for the search"),
    ("vector logic", False, "&& / || / ! on vectors"),
    ("function call: reassigns a variable", False, "a helper that changes a variable after setting it (x = ...; x *= ...;)"),
    ("function call: branches", False, "a helper with an if (or a ?: on whole statements)"),
    ("function call: loop", False, "a helper with a loop"),
    ("function call: early return", False, "a helper that returns before its end"),
    ("function call: texture or sampler argument", False, "a helper that reads a texture passed to it"),
    ("function call: out parameter", False, "a helper that writes to one of its parameters"),
    ("function call: writes part of a variable", False, "a helper that sets only some components (c.rgb = ...)"),
    ("function call: ", False, "a helper function sopt cannot look into yet"),
    ("function call", False, "a function call sopt cannot look into"),
    ("uses a macro", False, "a #define in the math sopt cannot spell out (a setting, # or ##, or several matching definitions)"),
    ("assignment shape", False, "an assignment sopt cannot rewrite in place"),
    ("loop header", True, "a for / while header: its counter is integer math"),
    ("array initializer", True, "a table of values (the table rewrite handles tables)"),
    ("not a simple statement", False, "a statement whose start sopt cannot find on a line it shares with { or }"),
    ("several declarators", False, "several variables declared in one statement (float a = ..., b = ...;)"),
    ("several statements on one line", False, "several statements on one line"),
    ("too many inputs", False, "reads more values than the search handles (--max-inputs / --max-slots)"),
    ("non-float arithmetic (min16float)", False, "half-precision (min16float) math: not searched yet (fp16 plan, step 2)"),
    ("non-float", False, "integer or boolean math; integers read as float are inputs, their math is not searched"),
    ("vector select", False, "a ? b : c with a vector condition"),
    ("access chain", False, "an array element at a computed index, or a struct member sopt cannot follow"),
    ("no assignment", False, "a value set without = in the source (x++, a call's out argument)"),
    ("intrinsic", False, "an intrinsic sopt does not model yet (a texture read inside a helper, isnan, mul, ...)"),
    ("declaration shape", False, "a declaration sopt cannot rewrite in place (an array, a semantic, ...)"),
    ("matrix", False, "a matrix or integer vector is built"),
    ("depends on BUFFER", False, "depends on the screen size in a way sopt does not cover"),
    ("return value not at statement start", False, "a return whose value starts on another line"),
    ("string literal", True, "text, not math"),
]


def explain(reason):
    r = reason[len("window: "):] if reason.startswith("window: ") else reason
    for prefix, nothing, text in EXPLAIN:
        if r.startswith(prefix):
            return nothing, text
    return False, ""


def skip_reasons(log):
    # "skip <file>:<line>: [window: ]<reason>": why sopt left code out (fewer than minOps is not giving up)
    counts = {}
    for m in re.finditer(r"^skip .*?:\d+: (.*)$", log, re.M):
        r = re.sub(r"'[^']*'|\"[^\"]*\"", "...", m.group(1).strip())  # names out, so reasons group
        counts[r] = counts.get(r, 0) + 1
    return counts


def macro_names(log):
    # "uses a macro (kind) 'NAME'": which macros still stop sopt, by name (the reasons above leave names out)
    counts = {}
    for m in re.finditer(r"^skip .*?:\d+: (?:window: )?uses a macro \(([^)]*)\) '([^']*)'$", log, re.M):
        k = f"{m.group(2)} ({m.group(1)})"
        counts[k] = counts.get(k, 0) + 1
    return counts


def run(dest, sopt, out, secs, timeout, args=""):
    pk = json.load(open(os.path.join(dest, "packages.json")))
    os.makedirs(out, exist_ok=True)
    rows, bad = [], 0
    for p in pk:
        if not p.get("run", True):
            continue
        if not p["effects"]:
            rows.append(f"| {p['id']} | [{p['name']}]({p['url']}) | 0 | | | | | {p['error'] or 'no effects'} |")
            continue
        o = os.path.join(out, p["id"])
        cmd = ([sopt, "-o", os.path.join(o, "sopt-out"), "--time", str(secs)] + shlex.split(args) +
               [os.path.join(dest, e) for e in p["effects"]])

        def one(extra):
            try:
                r = subprocess.run(cmd + extra, capture_output=True, text=True, errors="replace", timeout=timeout)
                return r.stdout + r.stderr, r.returncode
            except subprocess.TimeoutExpired as e:
                part = e.stdout or b""  # bytes even with text=True
                return part.decode(errors="replace") if isinstance(part, bytes) else part, "timeout"

        t0 = time.time()
        log, code = one([])
        # Easy mode too, what the menu does by default (2026-10-25: an easy-only bug broke SuperDepth3D): the same
        # folder, so its cache skips the search and only the files are written and checked again.
        easy_log, easy_code = one(["--easy"])
        secs_used = time.time() - t0
        skips_log, _ = one(["--list", "--skips"])
        os.makedirs(o, exist_ok=True)
        json.dump(skip_reasons(skips_log), open(os.path.join(o, "skips.json"), "w"), indent=1)
        json.dump(macro_names(skips_log), open(os.path.join(o, "macros.json"), "w"), indent=1)
        open(os.path.join(o, "log.txt"), "w", encoding="utf-8").write(log)
        open(os.path.join(o, "log-easy.txt"), "w", encoding="utf-8").write(easy_log)
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
        _, easy_checks, _ = summary_of(easy_log)
        if easy_code != 0:
            problem.append(f"**easy: exit {easy_code}**")
        if easy_checks and easy_checks[0] != "0":
            problem.append(f"**easy: {easy_checks[0]} of {easy_checks[1]} checks failed**")
        bad += bool(problem)
        if problem:  # the reasons into the job's log, so they are there without the reports
            print(f"--- {p['id']} {p['name']}: {', '.join(problem)}")
            for lg in (log, easy_log):
                print("\n".join([l for l in lg.splitlines()
                             if "does not parse" in l or "error" in l.lower() or l.startswith("dropped ")][:90]), flush=True)
        save = f"{saved[0]} regions, {saved[1]} -> {saved[2]}" if saved else ""
        rows.append(f"| {p['id']} | [{p['name']}]({p['url']}) | {len(p['effects'])} | {parsed[0] + ' / ' + parsed[1] if parsed else '?'}"
                    f" | {parsed[2] if parsed else '?'} | {save} | {secs_used:.0f} s | {', '.join(problem)} |")
        print(rows[-1], flush=True)
    open(os.path.join(out, "rows.md"), "w", encoding="utf-8").write("\n".join(rows) + "\n")
    md = ("# sopt-fx on ReShade's effect packages\n\nParsed / failed: effects; savings: regions with written variants,"
          " cost on the default model. Problems: sopt-fx exited with an error (crash, timeout) or a written variant"
          " does not parse, in expert mode or (\"easy:\") in easy mode, the menu's default.\n\n| # | package | effects | parsed / failed | regions | saved | time | problems |\n"
          "|---|---|---|---|---|---|---|---|\n" + "\n".join(rows) + "\n")
    open(os.path.join(out, "summary.md"), "w", encoding="utf-8").write(md)
    return bad


def summary(dirs):
    rows = []
    for d in dirs:
        f = os.path.join(d, "rows.md")
        if os.path.exists(f):
            rows += [l for l in open(f, encoding="utf-8").read().splitlines() if l.startswith("|")]
    rows.sort(key=lambda r: r.split("|")[1].strip())
    print("# sopt-fx on ReShade's effect packages\n\n| # | package | effects | parsed / failed | regions | saved | time | problems |\n"
          "|---|---|---|---|---|---|---|---|\n" + "\n".join(rows))
    skips = {}
    for d in dirs:
        for f in glob.glob(os.path.join(d, "**", "skips.json"), recursive=True):
            for r, n in json.load(open(f)).items():
                skips[r] = skips.get(r, 0) + n
    if skips:
        ranked = sorted(skips.items(), key=lambda x: -x[1])
        for title, want in (("Where sopt gave up (all packages)", False), ("Nothing to do (all packages)", True)):
            lines = [f"| {n} | {r} | {explain(r)[1]} |" for r, n in ranked if explain(r)[0] == want]
            print(f"\n## {title}\n\n\"window\": a piece of several lines.\n\n| count | reason | what it means |\n"
                  "|---|---|---|\n" + "\n".join(lines))
    macros = {}
    for d in dirs:
        for f in glob.glob(os.path.join(d, "**", "macros.json"), recursive=True):
            for k, n in json.load(open(f)).items():
                macros[k] = macros.get(k, 0) + n
    if macros:
        top = sorted(macros.items(), key=lambda x: -x[1])[:25]
        print("\n## Macros that still stop sopt (top 25)\n\n| count | macro (kind) |\n|---|---|\n" +
              "\n".join(f"| {n} | {k} |" for k, n in top))
    return any("**" in r for r in rows)


if __name__ == "__main__":
    a = sys.argv[1:]
    if a[:1] == ["ids"]:
        print(json.dumps([p["id"] for p, run in chosen(a[1], a[2] if len(a) > 2 else "") if run]))
    elif a[:1] == ["summary"]:
        sys.exit(1 if summary(a[1:]) else 0)
    elif a[:1] == ["install"]:
        install(a[1], a[2], a[3] if len(a) > 3 else "")
    elif a[:1] == ["run"]:
        opt = dict(zip(a[4::2], a[5::2]))
        sys.exit(1 if run(a[1], a[2], a[3], float(opt.get("--time", 1)), float(opt.get("--timeout", 1800)),
                          opt.get("--args", "")) else 0)
    else:
        sys.exit(__doc__)
