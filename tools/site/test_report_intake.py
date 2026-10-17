"""Checks for report_intake.py (no network). Run: python3 tools/site/test_report_intake.py (or pytest)."""
import glob
import io
import os
import sys
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
    stage = b"# StageBench 0.7.9\n# stage: ps\n# gpu: Foo\nconfig,test\n" + b"tput,a,1.0\n" * 30
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
    assert ri.members("u.zip", b"not a zip") == []
    assert ri.members("dir/r.csv", b"d") == [("r.csv", b"d")]



def test_known():
    path = sorted(glob.glob(os.path.join(ri.ROOT, "docs", "opbench", "*-2.csv")))[0]
    lines = open(path, encoding="utf-8").read().splitlines()
    assert ri.known("docs/opbench", "\n".join(lines))
    assert ri.known("docs/opbench", "\r\n".join(lines) + "\r\n")
    assert not ri.known("docs/opbench", "\n".join(lines) + "x")


if __name__ == "__main__":
    for name, f in list(globals().items()):
        if name.startswith("test_"):
            f()
    print("report_intake checks passed")
