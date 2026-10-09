#!/usr/bin/env python3
"""GPU Blueprint report intake from Dropbox (owner, 2026-10-09: run it as a GitHub Action, it costs no tokens).

Reads the uploads in the Dropbox folder (DROPBOX_FOLDER, default "/Uploads/GPU Blueprint"), keeps the valid reports and
saves them into the repository, then (only after the caller has pushed) deletes the uploads.

  report_intake.py fetch <work dir>    download + validate + save into docs/; writes <work dir>/uploads.json (paths to
                                       delete) and appends to docs/notes/intake-log.md; exit 0 with nothing saved when
                                       the folder is empty
  report_intake.py delete <work dir>   delete every upload listed in uploads.json

Valid: OpBench / TexBench CSVs ("# OpBench <v>" / "# TexBench <v>" header, numeric rows), ShaderInfo reports (start with
"GPU: "), TexBench pixel order PNGs that come with a valid TexBench CSV. Everything else is junk and is deleted too.
Upload names start with the uploader's name: they never go into the repository or the log (GDPR; the Actions log of a
public repository is public), only hardware and driver do.

Environment: DROPBOX_APP_KEY, DROPBOX_APP_SECRET, DROPBOX_REFRESH_TOKEN (repository secrets).
"""
import datetime
import io
import json
import os
import re
import sys
import urllib.parse
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FOLDER = os.environ.get("DROPBOX_FOLDER", "/Uploads/GPU Blueprint")
VENDORS = {0x10DE: "nvidia", 0x1002: "amd", 0x8086: "intel"}
MAX_BYTES = 50_000_000  # bigger uploads / zip members are junk (zip bombs, wrong files)


def token():
    data = urllib.parse.urlencode({
        "grant_type": "refresh_token",
        "refresh_token": os.environ["DROPBOX_REFRESH_TOKEN"],
        "client_id": os.environ["DROPBOX_APP_KEY"],
        "client_secret": os.environ["DROPBOX_APP_SECRET"],
    }).encode()
    with urllib.request.urlopen("https://api.dropboxapi.com/oauth2/token", data) as r:
        return json.load(r)["access_token"]


def api(tok, endpoint, args):
    req = urllib.request.Request("https://api.dropboxapi.com/2/" + endpoint, json.dumps(args).encode(),
                                 {"Authorization": "Bearer " + tok, "Content-Type": "application/json"})
    with urllib.request.urlopen(req) as r:
        return json.load(r)


def download(tok, path):
    req = urllib.request.Request("https://content.dropboxapi.com/2/files/download", b"",
                                 {"Authorization": "Bearer " + tok, "Dropbox-API-Arg": json.dumps({"path": path})})
    with urllib.request.urlopen(req) as r:
        return r.read()


def list_files(tok):
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


def classify(name, data):
    """('opbench' | 'texbench' | 'shaderinfo' | 'png' | None, text or None)."""
    low = name.lower()
    if low.endswith(".png"):
        return ("png", None) if data[:8] == b"\x89PNG\r\n\x1a\n" else (None, None)
    if not (low.endswith(".csv") or low.endswith(".txt")):
        return None, None
    try:
        text = data.decode("utf-8-sig")
    except UnicodeDecodeError:
        return None, None
    if re.match(r"# OpBench \d", text) and "# gpu:" in text and numeric_rows(text, 20):
        return "opbench", text
    if re.match(r"# TexBench \d", text) and "# gpu:" in text and numeric_rows(text, 20):
        return "texbench", text
    if text.startswith("GPU: ") and low.endswith(".txt"):
        return "shaderinfo", text
    return None, None


def members(name, data):
    """Files of one upload: a zip's members (any folder depth) or the file itself."""
    if name.lower().endswith(".zip"):
        try:
            with zipfile.ZipFile(io.BytesIO(data)) as z:
                return [(os.path.basename(i.filename), z.read(i)) for i in z.infolist()
                        if not i.is_dir() and i.file_size <= MAX_BYTES]
        except zipfile.BadZipFile:
            return []
    return [(os.path.basename(name), data)]


def fetch(work):
    tok = token()
    listed = list_files(tok)
    uploads = [path for path, _ in listed]
    os.makedirs(work, exist_ok=True)
    with open(os.path.join(work, "uploads.json"), "w") as f:
        json.dump(uploads, f)
    if not uploads:
        print("nothing uploaded")
        return
    saved, junk, log = [], 0, []
    for k, (path, size) in enumerate(listed, 1):
        if size > MAX_BYTES:
            junk += 1
            continue
        try:  # one broken upload is junk: it must not stop the others (or stay forever, failing every run)
            files = members(path, download(tok, path))
            reports, pngs = [], {}
            for name, data in files:
                kind, text = classify(name, data)
                if kind == "png":
                    pngs[name] = data
                elif kind:
                    reports.append((name, kind, text))
                else:
                    junk += 1
            for name, kind, text in reports:
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
                base = free_name(folder, slug(gpu, vid, dev), ".txt" if kind == "shaderinfo" else ".csv")
                ext = ".txt" if kind == "shaderinfo" else ".csv"
                out = [os.path.join(folder, base + ext)]
                with open(os.path.join(ROOT, out[0]), "w", encoding="utf-8", newline="\n") as f:
                    f.write(text)
                if kind == "texbench":  # its pixel order pictures: <csv stem>-order.png / -order-zoom.png
                    stem = os.path.splitext(name)[0]
                    for suffix in ("-order.png", "-order-zoom.png"):
                        if stem + suffix in pngs:
                            p = os.path.join(folder, base + suffix)
                            with open(os.path.join(ROOT, p), "wb") as f:
                                f.write(pngs.pop(stem + suffix))
                            out.append(p)
                saved += out
                log.append("- upload %d: %s, %s (driver %s): %s" % (k, kind, gpu, driver,
                                                                    ", ".join("`%s`" % p for p in out) +
                                                                    (" (%s)" % version if version else "")))
            junk += len(pngs)
        except Exception as e:
            print("upload %d: junk (%s)" % (k, type(e).__name__))
            junk += 1
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M UTC")
    if saved:
        logpath = os.path.join(ROOT, "docs/notes/intake-log.md")
        new = not os.path.exists(logpath)
        with open(logpath, "a", encoding="utf-8", newline="\n") as f:
            if new:
                f.write("# Report intake log\n\nWritten by tools/site/report_intake.py (the report-intake workflow): what "
                        "came in, by hardware and driver only. Notes and analysis go to docs/notes/history.md.\n")
            f.write("\n## %s\n" % stamp + "\n".join(log) + "\n")
    print("%d upload(s), %d file(s) saved, %d junk file(s)" % (len(uploads), len(saved), junk))
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
