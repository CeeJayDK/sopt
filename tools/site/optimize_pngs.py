#!/usr/bin/env python3
"""Lossless background optimization of the web pages' PNGs (owner, 2026-10-05; .github/workflows/optimize-pngs.yml).

Manifest (docs/texbench/png-optimized.txt), one line per picture: <git blob id> <pixel hash> <file name>.
  - file fingerprint: the git blob id of the optimized file; a file whose blob is listed is done, so nothing is
    optimized twice;
  - image fingerprint: SHA-256 of the decoded RGBA pixels and the size; a file that is not done but shows the same
    image as a listed one (e.g. the same picture uploaded again, compressed worse) gets the earlier optimized file
    back from git history instead of a new optimization run.
Every result is checked: its pixels must equal the original's, else the original is kept.

  optimize_pngs.py --count                 number of pictures not done yet
  optimize_pngs.py --ect PATH [--level N] [--publish]
      --publish: commit to the checked-out branch and push it in batches (about every --every seconds and at the end),
      and start the pages workflow after each push (gh workflow run pages.yml).
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PNG_DIR = os.path.join(ROOT, "docs", "texbench")
MANIFEST = os.path.join(PNG_DIR, "png-optimized.txt")
HEADER = ("# Optimized PNGs (tools/site/optimize_pngs.py): git blob id of the optimized file, SHA-256 of its RGBA "
          "pixels and size, file name.\n")


def git(*args, capture=True, check=True, data=None):
    r = subprocess.run(["git", *args], cwd=ROOT, input=data, capture_output=capture, check=check)
    return r.stdout


def blob_id(path):
    return git("hash-object", path).decode().strip()


def pixel_hash(path):
    from PIL import Image
    with Image.open(path) as im:
        im.load()
        rgba = im.convert("RGBA")
        h = hashlib.sha256(f"{rgba.width}x{rgba.height}\n".encode())
        h.update(rgba.tobytes())
    return h.hexdigest()


def pixel_hash_of(data):
    with tempfile.NamedTemporaryFile(suffix=".png", delete=False) as f:
        f.write(data)
    try:
        return pixel_hash(f.name)
    finally:
        os.unlink(f.name)


def read_manifest():
    entries = []
    if os.path.exists(MANIFEST):
        for line in open(MANIFEST, encoding="utf-8"):
            parts = line.split()
            if len(parts) == 3 and not line.startswith("#"):
                entries.append(parts)
    return entries


def write_manifest(entries):
    with open(MANIFEST, "w", encoding="utf-8", newline="\n") as f:
        f.write(HEADER)
        for e in sorted(entries, key=lambda e: e[2]):
            f.write(" ".join(e) + "\n")


def pictures():
    return sorted(os.path.join(PNG_DIR, n) for n in os.listdir(PNG_DIR) if n.lower().endswith(".png"))


def pending(entries):
    done = {e[0] for e in entries}
    return [p for p in pictures() if blob_id(p) not in done]


def blob_exists(blob):
    return subprocess.run(["git", "cat-file", "-e", blob], cwd=ROOT, capture_output=True).returncode == 0


def optimize(path, ect, level):
    """Optimized copy of path with unchanged pixels, or None when ECT gains nothing or changes pixels."""
    tmp = tempfile.mkdtemp()
    try:
        out = os.path.join(tmp, os.path.basename(path))
        shutil.copyfile(path, out)
        if subprocess.run([ect, "-quiet", "-strip", f"-{level}", out]).returncode != 0:
            print(f"  {os.path.basename(path)}: ECT failed, original kept", flush=True)
            return None
        if os.path.getsize(out) >= os.path.getsize(path):
            return None
        if pixel_hash(out) != pixel_hash(path):
            print(f"  {os.path.basename(path)}: ECT changed pixels, original kept", flush=True)
            return None
        return open(out, "rb").read()
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def publish(message):
    git("add", "-A", os.path.relpath(PNG_DIR, ROOT))
    if subprocess.run(["git", "diff", "--cached", "--quiet"], cwd=ROOT).returncode == 0:
        return
    git("commit", "-q", "-m", message)
    branch = git("rev-parse", "--abbrev-ref", "HEAD").decode().strip()
    for attempt in range(4):
        if (subprocess.run(["git", "pull", "-q", "--rebase", "origin", branch], cwd=ROOT).returncode == 0 and
                subprocess.run(["git", "push", "-q", "origin", f"HEAD:{branch}"], cwd=ROOT).returncode == 0):
            break
        time.sleep(5 * (attempt + 1))
    else:
        sys.exit("push failed")
    if subprocess.run(["gh", "workflow", "run", "pages.yml", "--ref", branch], cwd=ROOT).returncode != 0:
        print("could not start the pages workflow", flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--count", action="store_true")
    ap.add_argument("--ect")
    ap.add_argument("--level", type=int, default=5)
    ap.add_argument("--publish", action="store_true")
    ap.add_argument("--every", type=int, default=600)
    a = ap.parse_args()

    entries = read_manifest()
    todo = pending(entries)
    if a.count:
        print(len(todo))
        return
    if not a.ect:
        sys.exit("--ect PATH needed")

    last = time.time()
    count = 0
    for path in todo:
        name = os.path.basename(path)
        before = os.path.getsize(path)
        pix = pixel_hash(path)
        how = "kept as is"
        # Same image already optimized under some name: take that file back from git history.
        data = None
        for blob, epix, _ in entries:
            if epix == pix and blob_exists(blob):
                old = git("cat-file", "blob", blob)
                if len(old) < before and pixel_hash_of(old) == pix:
                    data, how = old, "restored the earlier optimized file"
                    break
        if data is None:
            data = optimize(path, a.ect, a.level)
            if data is not None:
                how = f"ECT -{a.level}"
        if data is not None:
            with open(path, "wb") as f:
                f.write(data)
        entries = [e for e in entries if e[2] != name] + [[blob_id(path), pix, name]]
        write_manifest(entries)
        count += 1
        print(f"{name}: {before} -> {os.path.getsize(path)} bytes ({how})", flush=True)
        if a.publish and time.time() - last >= a.every:
            publish(f"Optimize PNGs losslessly ({count} so far)")
            last = time.time()
    if a.publish:
        publish(f"Optimize PNGs losslessly ({count} files)")


if __name__ == "__main__":
    main()
