"""Checks for report_intake.py (no network). Run: python3 tools/site/test_report_intake.py (or pytest)."""
import glob
import io
import json
import os
import sys
import tempfile
import urllib.error
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import report_intake as ri  # noqa: E402


def test_slug():
    assert ri.slug("NVIDIA GeForce RTX 4090", 0x10DE, 0x2684) == "nvidia-rtx-4090"
    assert ri.slug("NVIDIA GeForce GTX 1660", 0x10DE, 0x2184) == "nvidia-gtx-1660"
    assert ri.slug("Intel(R) UHD Graphics 630", 0x8086, 0x3E92) == "intel-uhd-630"
    assert ri.slug("Intel(R) HD Graphics 530", 0x8086, 0x1912) == "intel-hd-graphics-530"
    assert ri.slug("AMD Radeon RX 9070 XT", 0x1002, 0x7550) == "amd-radeon-rx-9070-xt"
    assert ri.slug("AMD Radeon(TM) Graphics", 0x1002, 0x164E) == "amd-radeon-0x164e"


def test_classify():
    op = sorted(glob.glob(os.path.join(ri.ROOT, "docs", "opbench", "*-2.csv")))[0]
    data = open(op, "rb").read()
    assert ri.classify("x.csv", data)[0] == "opbench"
    assert ri.classify("x.CSV", b"\xef\xbb\xbf" + data)[0] == "opbench"  # BOM
    assert ri.classify("x.exe", data) == (None, None)
    assert ri.classify("x.csv", data.splitlines(True)[0] + b"name,a\n" * 30) == (None, None)  # no numbers
    assert ri.classify("x.csv", b"\xff\xfe junk") == (None, None)
    assert ri.classify("x.txt", b"GPU: Foo\n")[0] == "shaderinfo"
    assert ri.classify("x.csv", b"GPU: Foo\n") == (None, None)
    stage = b"# StageBench 0.7.9\n# stage: ps\n# gpu: Foo\nconfig,test,units,units_vs_base\n" + b"tput,a,1.0,1.0\n" * 30
    assert ri.classify("stagebench-ps-Foo.csv", stage)[0] == "stagebench"
    assert ri.classify("x.csv", stage.replace(b"# stage: ps\n", b""))[0] is None
    stopped = b"# OpBench 0.7.15\n# gpu: Foo\n# driver: 1\n# stopped: the GPU has not finished (while measuring \"x\")\n"
    assert ri.classify("x.csv", stopped)[0] == "stopped"
    assert ri.classify("x.csv", b"# stopped: no header\n") == (None, None)  # 0.7.14 and older: no card, junk
    assert ri.classify("x.csv", stage + b"# stopped: late\n")[0] == "stagebench"  # a long stopped run stays a report
    assert ri.classify("x.png", b"\x89PNG\r\n\x1a\nrest")[0] == "png"
    assert ri.classify("x.png", b"<html>") == (None, None)


def test_members():
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        z.writestr("a/b/r.csv", "small")
        z.writestr("big.txt", "x" * 100)
        z.writestr("dir/", "")
    old = ri.MAX_BYTES
    ri.MAX_BYTES = 50
    try:
        assert ri.members("u.zip", buf.getvalue()) == [("r.csv", b"small")]
    finally:
        ri.MAX_BYTES = old
    old = ri.MAX_TOTAL
    ri.MAX_TOTAL = 104
    try:
        assert ri.members("u.zip", buf.getvalue()) == []  # too much in total
    finally:
        ri.MAX_TOTAL = old
    assert ri.members("u.zip", b"not a zip") == []
    assert ri.members("dir/r.csv", b"d") == [("r.csv", b"d")]



HEAD = "# OpBench 0.10.5\n# gpu: NVIDIA GeForce GTX 1660\n# vendor: 0x10DE\n# device: 0x2184\n# driver: 32.0.15.6614\n"
ROWS = "config,test,base,units,units_vs_base\n" + "".join("tput,t%d,mad,%d.0,%d.0\n" % (i, i, i) for i in range(30))
OPBENCH = (HEAD + ROWS).encode()
STAGE = OPBENCH.replace(b"OpBench", b"StageBench").replace(b"# gpu", b"# stage: ps\n# gpu")
SHADERINFO = b"GPU: Intel(R) UHD Graphics 630\nvendor 32902, device 16018\ndriver: 32.0.101.7082\n"


def test_rows_checked():
    assert ri.classify("x.csv", OPBENCH)[0] == "opbench"
    assert ri.classify("x.csv", OPBENCH + b"tput\n") == (None, None)  # a row cut short
    assert ri.classify("x.csv", OPBENCH + b"tput,neg,mad,abc,abc\n") == (None, None)  # text in place of a number
    assert ri.classify("x.csv", OPBENCH + b"tput,neg,mad,nan,inf\n") == (None, None)  # not valid in the site's JSON
    assert ri.classify("x.csv", OPBENCH + b"tput,neg,mad,,\n") == (None, None)  # empty: only TexBench may
    tex = OPBENCH.replace(b"OpBench", b"TexBench")
    assert ri.classify("x.csv", tex + b"tput,neg,mad,,\n")[0] == "texbench"
    assert ri.classify("x.csv", tex + b"tput,neg,mad,abc,\n") == (None, None)
    stopped = OPBENCH + b"tput\n# stopped: the GPU has not finished\n"
    assert ri.classify("x.csv", stopped)[0] == "stopped"  # kept apart from the reports


def test_hardware_only():
    assert ri.classify("x.csv", OPBENCH.replace(b"\n", b"\r\n"))[0] == "opbench"
    assert ri.classify("x.csv", STAGE.replace(b"\n", b"\r\n"))[0] == "stagebench"
    assert ri.classify("x.txt", SHADERINFO.replace(b"32.0.101.7082", b"AMD proprietary driver 2.0.3 (LLPC) (2.0.3)"))[0] == "shaderinfo"
    assert ri.classify("x.csv", OPBENCH.replace(b"GTX 1660", b"me@example.com")) == (None, None)
    assert ri.classify("x.csv", OPBENCH.replace(b"GTX 1660", b"x" * 120)) == (None, None)
    assert ri.classify("x.csv", OPBENCH.replace(b"32.0.15.6614", b"call me: 555")) == (None, None)
    assert ri.classify("x.csv", OPBENCH.replace(b"# gpu", b"#GPU: a@b\n# gpu")) == (None, None)
    assert ri.classify("x.csv", OPBENCH + b"tput,z,https://example.com,1,1\n") == (None, None)
    assert ri.classify("x.txt", SHADERINFO.replace(b"UHD", b"<b>UHD</b>")) == (None, None)


def run_fetch(uploads, fail=()):
    """fetch() with a fake Dropbox and a temporary repository: (the files saved, the uploads the delete step removes)."""
    old = ri.ROOT, ri.token, ri.list_files, ri.download

    def download(tok, path):
        if path in fail:
            raise urllib.error.URLError("no network")
        return uploads[path]

    with tempfile.TemporaryDirectory() as root, tempfile.TemporaryDirectory() as work:
        os.makedirs(os.path.join(root, "docs", "notes"))
        ri.ROOT, ri.download = root, download
        ri.token = lambda: "token"
        ri.list_files = lambda tok: [(p, len(d)) for p, d in uploads.items()]
        try:
            ri.fetch(work)
        finally:
            ri.ROOT, ri.token, ri.list_files, ri.download = old
        with open(os.path.join(work, "uploads.json")) as f:
            remove = json.load(f)
        saved = []
        for d, _, files in os.walk(os.path.join(root, "docs")):
            saved += [f for f in files if f != "intake-log.md"]
    return sorted(saved), remove


def zipped(**files):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        for name, data in files.items():
            z.writestr(name, data)
    return buf.getvalue()


def test_fetch():
    # A ShaderInfo report alone, and after a StageBench report (whose stage is not its own).
    assert run_fetch({"/a.txt": SHADERINFO}) == (["intel-uhd-630.txt"], ["/a.txt"])
    assert run_fetch({"/a.csv": STAGE, "/b.txt": SHADERINFO})[0] == ["intel-uhd-630.txt", "nvidia-gtx-1660-ps.csv"]
    # A failed download stays for the next run; the others go on, junk is removed.
    saved, remove = run_fetch({"/a.csv": OPBENCH, "/b.csv": OPBENCH, "/c.bin": b"junk"}, fail=("/b.csv",))
    assert saved == ["nvidia-gtx-1660.csv"] and remove == ["/a.csv", "/c.bin"]
    # One broken report does not lose the others in its zip.
    broken = OPBENCH.replace(b"0x10DE", b"xyz")
    saved, remove = run_fetch({"/z.zip": zipped(**{"a.csv": broken, "b.txt": SHADERINFO})})
    assert saved == ["intel-uhd-630.txt"] and remove == ["/z.zip"]
    assert run_fetch({}) == ([], [])


def test_known():
    path = sorted(glob.glob(os.path.join(ri.ROOT, "docs", "opbench", "*-2.csv")))[0]
    lines = open(path, encoding="utf-8").read().splitlines()
    assert ri.known("docs/opbench", "\n".join(lines))
    assert ri.known("docs/opbench", "\r\n".join(lines) + "\r\n")
    assert not ri.known("docs/opbench", "\n".join(lines) + "x")


def test_pick():
    # build.pick: the newest report, or the least drift among those with its version and driver.
    import build
    r = lambda v, drv, d: {"key": (v, 0), "driver": drv, "drift": d}
    reps = [r((0, 9, 2), "1.0", "140%"), r((0, 9, 2), "1.0", "0.1%"), r((0, 9, 2), "2.0", "5%"), r((0, 9, 0), "1.0", "0%")]
    k = lambda x: x["key"]
    assert build.pick(reps[:2], k, lambda x: x)["drift"] == "0.1%"  # same version and driver: the cleaner run
    assert build.pick([reps[0], reps[3]], k, lambda x: x)["drift"] == "140%"  # an older version is not a candidate
    assert build.pick([reps[0], reps[2]], k, lambda x: x)["drift"] == "140%"  # another driver is not a candidate
    # by_cleanest (per test, later wins): other drivers first, then the newest's driver from most to least drift,
    # older versions included (the Renoir: a clean 0.7.0 run and a noisy 0.9.2 one with one driver).
    order = [x["drift"] for x in build.by_cleanest([reps[0], reps[3], reps[2]], k, lambda x: x)]
    assert order == ["5%", "140%", "0%"], order


if __name__ == "__main__":
    for name, f in list(globals().items()):
        if name.startswith("test_"):
            f()
    print("report_intake checks passed")
