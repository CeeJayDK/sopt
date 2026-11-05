#!/usr/bin/env python3
"""GPU Blueprint report intake from Dropbox (owner, 2026-10-09: run it as a GitHub Action, it costs no tokens).

Reads the uploads in the Dropbox folder (DROPBOX_FOLDER), keeps the valid reports and
saves them into the repository, then (only after the caller has pushed) deletes the uploads.

  report_intake.py fetch <work dir>    download + validate + save into docs/; writes <work dir>/uploads.json (paths to
                                       delete) and appends to docs/notes/intake-log.md; exit 0 with nothing saved when
                                       the folder is empty
  report_intake.py delete <work dir>   delete every upload listed in uploads.json

Valid: OpBench / TexBench CSVs ("# OpBench <v>" / "# TexBench <v>" header, numeric rows), ShaderInfo reports (start with
"GPU: "), TexBench pixel order PNGs that come with a valid TexBench CSV. Everything else is junk and is deleted too;
an upload whose download failed stays for the next run.
Upload names start with the uploader's name: they never go into the repository or the log (GDPR; the Actions log of a
public repository is public), only hardware and driver do.

Environment: DROPBOX_APP_KEY, DROPBOX_APP_SECRET, DROPBOX_REFRESH_TOKEN (repository secrets), DROPBOX_FOLDER (repository
variable; the app sees its own folder as "/").
"""
import csv
import datetime
import http.client
import io
import json
import math
import os
import re
import sys
import urllib.error
import urllib.parse
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FOLDER = os.environ.get("DROPBOX_FOLDER", "")
VENDORS = {0x10DE: "nvidia", 0x1002: "amd", 0x8086: "intel"}
MAX_BYTES = 50_000_000  # bigger uploads / zip members are junk (zip bombs, wrong files)
MAX_TOTAL = 200_000_000  # a zip that unpacks to more is junk: it would stop the job, and so every later report
TIMEOUT = 120  # seconds; a stuck connection counts as a failed request
HARDWARE_NAME = re.compile(r"[A-Za-z0-9?][A-Za-z0-9 ()+/._-]{0,99}")  # "AMD proprietary driver 2.0 (LLPC) (2.0.3)"
CONTACT = re.compile(r"https?:|www\.|\S@\S")


def token():
    data = urllib.parse.urlencode({
        "grant_type": "refresh_token",
        "refresh_token": os.environ["DROPBOX_REFRESH_TOKEN"],
        "client_id": os.environ["DROPBOX_APP_KEY"],
        "client_secret": os.environ["DROPBOX_APP_SECRET"],
    }).encode()
    try:
        with urllib.request.urlopen("https://api.dropboxapi.com/oauth2/token", data, timeout=TIMEOUT) as r:
            return json.load(r)["access_token"]
    except urllib.error.HTTPError as e:
        # Dropbox says why (invalid_grant: refresh token wrong or revoked; invalid_client: app key / secret)
        sys.exit(f"Dropbox token: HTTP {e.code} {e.read().decode(errors='replace')[:300]}")


def api(tok, endpoint, args):
    req = urllib.request.Request("https://api.dropboxapi.com/2/" + endpoint, json.dumps(args).encode(),
                                 {"Authorization": "Bearer " + tok, "Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=TIMEOUT) as r:
        return json.load(r)


def download(tok, path):
    req = urllib.request.Request("https://content.dropboxapi.com/2/files/download", b"",
                                 {"Authorization": "Bearer " + tok, "Dropbox-API-Arg": json.dumps({"path": path})})
    with urllib.request.urlopen(req, timeout=TIMEOUT) as r:
        return r.read()


def list_files(tok):
    if not FOLDER:
        raise SystemExit("DROPBOX_FOLDER is not set (repository variable)")
    try:
        res = api(tok, "files/list_folder", {"path": FOLDER, "recursive": True})
    except urllib.error.HTTPError as e:
        if e.code == 409:  # wrong folder (app-folder apps see their folder as "/"): fail, do not pass silently
            raise SystemExit("Dropbox folder not found: %s (repository variable DROPBOX_FOLDER)" % FOLDER)
        raise
    files = [(e["path_display"], e["size"]) for e in res["entries"] if e[".tag"] == "file"]
    while res.get("has_more"):
        res = api(tok, "files/list_folder/continue", {"cursor": res["cursor"]})
        files += [(e["path_display"], e["size"]) for e in res["entries"] if e[".tag"] == "file"]
    return files


def header(text):
    """The '# key: value' lines at the top of a CSV."""
    h = {}
    for line in text.splitlines():
        if not line.startswith("#"):
            break
        m = re.match(r"#\s*([^:]+):\s*(.*)", line)
        if m:
            h[m.group(1).strip().lower()] = m.group(2).strip()
    return h


def slug(gpu, vendor_id, device):
    """'NVIDIA GeForce RTX 4090' -> 'nvidia-rtx-4090', like the existing report names."""
    vendor = VENDORS.get(vendor_id, "gpu")
    s = re.sub(r"\((r|tm)\)", " ", gpu.lower())
    s = re.sub(r"\b(nvidia|amd|intel|ati|geforce|corporation)\b", " ", s)
    if vendor == "intel" and not re.search(r"\bhd graphics\b", s):
        s = re.sub(r"\bgraphics\b", " ", s)
    s = re.sub(r"[^a-z0-9]+", "-", s).strip("-")
    if vendor == "amd" and s in ("radeon-graphics", "radeon", ""):  # the integrated GPUs all say this
        s = "radeon-0x%04x" % device
    return vendor + "-" + (s or "0x%04x" % device)


def free_name(folder, base, ext):
    n, name = 1, base
    while os.path.exists(os.path.join(ROOT, folder, name + ext)):
        n += 1
        name = "%s-%d" % (base, n)
    return name


def numeric_rows(text, minimum):
    rows = [l for l in text.splitlines() if l and not l.startswith("#")]
    num = sum(1 for l in rows[1:] if re.search(r",-?\d+(\.\d+)?(,|$)", l))
    return num >= minimum


def is_number(value, empty_ok):
    if not value:
        return empty_ok
    try:
        return math.isfinite(float(value))
    except ValueError:
        return False


def rows_ok(text, empty_ok=False):
    """Every row usable: gen_expected.py and build.py read them all, and one row they choke on would stop the intake and
    the site for everybody. empty_ok: TexBench leaves numbers empty where a test has none."""
    rows = list(csv.reader(l for l in text.splitlines() if l and not l.startswith("#")))
    head = rows[0]
    if not {"config", "test", "units", "units_vs_base"} <= set(head):
        return False
    config, test, units, vs_base = (head.index(c) for c in ("config", "test", "units", "units_vs_base"))
    return all(len(r) >= len(head) and r[config] and r[test] and is_number(r[units], empty_ok) and
               is_number(r[vs_base], empty_ok) for r in rows[1:])


def hardware_only(text):
    """Reports go public by themselves, so the published fields hold hardware names only (GDPR).
    shortcut: stops addresses, links and long notes, not a short invented name or a note in a row; check by hand if
    one ever gets through."""
    if CONTACT.search(text):
        return False
    values = re.findall(r"^#?[ \t]*(?:gpu|driver)[ \t]*:[ \t]*(.*)$", text, re.I | re.M)
    return all(HARDWARE_NAME.fullmatch(v.strip()) for v in values)


def classify(name, data):
    """('opbench' | 'texbench' | 'stagebench' | 'stopped' | 'shaderinfo' | 'png' | None, text or None)."""
    low = name.lower()
    if low.endswith(".png"):
        return ("png", None) if data[:8] == b"\x89PNG\r\n\x1a\n" else (None, None)
    if not (low.endswith(".csv") or low.endswith(".txt")):
        return None, None
    try:
        text = data.decode("utf-8-sig")
    except UnicodeDecodeError:
        return None, None
    if not hardware_only(text):
        return None, None
    if re.match(r"# OpBench \d", text) and "# gpu:" in text and numeric_rows(text, 20) and rows_ok(text):
        return "opbench", text
    if re.match(r"# TexBench \d", text) and "# gpu:" in text and numeric_rows(text, 20) and rows_ok(text, True):
        return "texbench", text
    if re.match(r"# StageBench \d", text) and re.search(r"^# stage: (ps|vs)\r?$", text, re.M) and "# gpu:" in text and numeric_rows(text, 20) and rows_ok(text):
        return "stagebench", text
    if text.startswith("GPU: ") and low.endswith(".txt"):
        return "shaderinfo", text
    # A run that stopped before 20 results (header + "# stopped: why"): kept apart from the reports, to see
    # which cards and drivers fail and where (owner, 2026-10-17). Longer stopped runs are reports as before.
    if re.match(r"# (OpBench|TexBench|StageBench) \d", text) and "# gpu:" in text and re.search(r"^# stopped: ", text, re.M):
        return "stopped", text
    return None, None


def members(name, data):
    """Files of one upload: a zip's members (any folder depth) or the file itself."""
    if name.lower().endswith(".zip"):
        try:
            with zipfile.ZipFile(io.BytesIO(data)) as z:
                infos = [i for i in z.infolist() if not i.is_dir() and i.file_size <= MAX_BYTES]
                if sum(i.file_size for i in infos) > MAX_TOTAL:
                    return []
                return [(os.path.basename(i.filename), z.read(i)) for i in infos]
        except zipfile.BadZipFile:
            return []
    return [(os.path.basename(name), data)]


def known(folder, text):
    """Whether this exact report is already saved: the zip holds every report in the tester's Reports folder, so a
    second run resends the earlier ones unchanged (owner, 2026-10-16)."""
    d = os.path.join(ROOT, folder)
    lines = text.splitlines()  # line ends differ between the upload and a Windows checkout
    return any(f.endswith((".csv", ".txt")) and
               open(os.path.join(d, f), encoding="utf-8", errors="replace").read().splitlines() == lines
               for f in os.listdir(d))


def save_report(k, name, kind, text, pngs):
    """Saves one report with its pictures: ([saved paths], log line), or None when it is already saved."""
    h = {}
    if kind == "shaderinfo":
        gpu = text.splitlines()[0][5:].strip()
        m = re.search(r"vendor (\d+), device (\d+)", text)
        vid, dev = (int(m.group(1)), int(m.group(2))) if m else (0, 0)
        drv = re.search(r"^driver: (.*)$", text, re.M)
        driver, version = drv.group(1) if drv else "?", ""
    else:
        h = header(text)
        gpu, driver = h.get("gpu", "?"), h.get("driver", "?")
        vid, dev = int(h.get("vendor", "0"), 16), int(h.get("device", "0"), 16)
        version = text.split("\n", 1)[0][2:]
    folder = "docs/" + kind
    os.makedirs(os.path.join(ROOT, folder), exist_ok=True)
    if known(folder, text):
        stem = os.path.splitext(name)[0]
        for suffix in ("-order.png", "-order-zoom.png"):
            pngs.pop(stem + suffix, None)
        return None
    stem = slug(gpu, vid, dev) + ("-" + h["stage"] if kind == "stagebench" or "stage" in h else "")
    if kind == "stopped":  # docs/stopped/<tool>-<gpu>[-stage], the reason in the log
        stem = text[2:].split()[0].lower() + "-" + stem
        version += ", " + re.search(r"^# (stopped: .*)$", text, re.M).group(1)
    ext = ".txt" if kind == "shaderinfo" else ".csv"
    base = free_name(folder, stem, ext)
    out = [folder + "/" + base + ext]  # forward slashes on Windows too (log, saved.txt)
    with open(os.path.join(ROOT, out[0]), "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    if kind == "texbench":  # its pixel order pictures: <csv stem>-order.png / -order-zoom.png
        stem = os.path.splitext(name)[0]
        for suffix in ("-order.png", "-order-zoom.png"):
            if stem + suffix in pngs:
                p = folder + "/" + base + suffix
                with open(os.path.join(ROOT, p), "wb") as f:
                    f.write(pngs.pop(stem + suffix))
                out.append(p)
    return out, "- upload %d: %s, %s (driver %s): %s" % (k, kind, gpu, driver,
                                                         ", ".join("`%s`" % p for p in out) +
                                                         (" (%s)" % version if version else ""))


def fetch(work):
    tok = token()
    listed = list_files(tok)
    uploads = [path for path, _ in listed]
    os.makedirs(work, exist_ok=True)
    if not uploads:
        with open(os.path.join(work, "uploads.json"), "w") as f:
            json.dump(uploads, f)
        print("nothing uploaded")
        return
    saved, junk, resent, log = [], 0, 0, []
    for k, (path, size) in enumerate(listed, 1):
        if size > MAX_BYTES:
            junk += 1
            continue
        try:
            data = download(tok, path)
        except (OSError, http.client.HTTPException) as e:  # the network's fault, not the upload's: the next run tries again
            print("upload %d: download failed (%s), kept for the next run" % (k, type(e).__name__))
            uploads.remove(path)
            continue
        try:  # one broken upload is junk: it must not stop the others (or stay forever, failing every run)
            reports, pngs = [], {}
            for name, blob in members(path, data):
                kind, text = classify(name, blob)
                if kind == "png":
                    pngs[name] = blob
                elif kind:
                    reports.append((name, kind, text))
                else:
                    junk += 1
        except Exception as e:
            print("upload %d: junk (%s)" % (k, type(e).__name__))
            junk += 1
            continue
        for name, kind, text in reports:
            try:  # one broken report must not lose the others in its zip
                done = save_report(k, name, kind, text, pngs)
            except Exception as e:
                print("upload %d: a report is junk (%s)" % (k, type(e).__name__))
                junk += 1
                continue
            if done:
                saved += done[0]
                log.append(done[1])
            else:
                resent += 1
        junk += len(pngs)
    with open(os.path.join(work, "uploads.json"), "w") as f:  # what the delete step removes: only what was handled
        json.dump(uploads, f)
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M UTC")
    if saved:
        logpath = os.path.join(ROOT, "docs/notes/intake-log.md")
        new = not os.path.exists(logpath)
        with open(logpath, "a", encoding="utf-8", newline="\n") as f:
            if new:
                f.write("# Report intake log\n\nWritten by tools/site/report_intake.py (the report-intake workflow): what "
                        "came in, by hardware and driver only. Notes and analysis go to docs/notes/history.md.\n")
            f.write("\n## %s\n" % stamp + "\n".join(log) + "\n")
    print("%d upload(s), %d file(s) saved, %d resent report(s) skipped, %d junk file(s)" % (len(listed), len(saved), resent, junk))
    print("\n".join(log))
    with open(os.path.join(work, "saved.txt"), "w") as f:
        f.write("\n".join(saved))


def delete(work):
    with open(os.path.join(work, "uploads.json")) as f:
        uploads = json.load(f)
    if not uploads:
        return
    tok = token()
    for path in uploads:
        try:
            api(tok, "files/delete_v2", {"path": path})
        except urllib.error.HTTPError as e:
            if e.code != 409:  # 409: already gone
                raise
    print("deleted %d upload(s)" % len(uploads))


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] not in ("fetch", "delete"):
        raise SystemExit(__doc__)
    (fetch if sys.argv[1] == "fetch" else delete)(sys.argv[2])
