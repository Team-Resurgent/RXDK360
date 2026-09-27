#!/usr/bin/env python3
"""Build and run the scalar-xnamath test (tests/xnamath/t_xnamath.cpp) in xenia.

Unlike the pure-libc stdlib suite (tools/run_stdlib_tests.py), this test pulls
the XDK's xnamath.h through <xtl.h>, so it must compile like a real title: the
Win32/Xbox define set plus -D_XM_NO_INTRINSICS_ (the scalar XMVECTOR/XMMATRIX
path every RXDK-360 title uses -- see the clang Toolset.props) and the MS-compat
flags the stdlib harness deliberately omits. Those defines (_WIN32, _SIZE_T_...)
would break the other libc/libc++ tests, which is why this lives in its own
one-title runner rather than in the stdlib combined build.

It links via mktitle (--cc clang, default runtime + synthesised kernel imports),
runs the title headless once, and parses the rxdk_test.h [T] PASS/FAIL/DONE lines
from xenia.log -- the same protocol the stdlib/corpus harnesses use.

  python tools/run_xnamath_test.py
  python tools/run_xnamath_test.py --keep    # keep the build dir
"""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = os.path.join(ROOT, "tests", "xnamath", "t_xnamath.cpp")
TESTS_STDLIB = os.path.join(ROOT, "tests", "stdlib")     # rxdk_test.h lives here
BUILD = os.path.join(ROOT, "build", "xnamath")

XDK_INC = os.environ.get(
    "RXDK_XDK_INC", r"C:\Program Files (x86)\Microsoft Xbox 360 SDK\include\xbox")

XENIA = os.environ.get(
    "RXDK_XENIA", r"D:\Git\xenia-canary\build\bin\Windows\Release\xenia_canary.exe")
# Default the working dir to the emulator's own folder (always valid); xenia
# writes xenia.log there.
XENIA_CWD = os.environ.get("RXDK_XENIA_CWD", os.path.dirname(XENIA))
XENIA_LOG = os.environ.get("RXDK_XENIA_LOG", os.path.join(os.path.dirname(XENIA), "xenia.log"))

T_RE = re.compile(r"\[T\]\s+(PASS|FAIL|DONE)\b\s*(.*)")

# The title-compile environment, mirroring the clang PlatformToolset
# (vs20xx/.../clang/Toolset.props) so the scalar xnamath path is what a real
# sample compiles.
TITLE_CFLAGS = [
    "-fms-extensions", "-fms-compatibility", "-fdeclspec",
    "-D_WIN32=1", "-D_M_PPCBE=1", "-D_M_PPC=1", "-D_XBOX=1", "-D_XBOX_VER=200",
    "-D__export=", "-D_SIZE_T_DEFINED", "-D_XM_NO_INTRINSICS_",
    "-Wno-nonportable-include-path", "-Wno-pragma-pack", "-Wno-ignored-attributes",
    "-I" + XDK_INC, "-I" + TESTS_STDLIB,
]


def kill_stale_xenia():
    if os.name == "nt":
        subprocess.run(["taskkill", "/F", "/IM", "xenia_canary.exe"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def build():
    os.makedirs(BUILD, exist_ok=True)
    out = os.path.join(BUILD, "xnamath.xex")
    cmd = [sys.executable, os.path.join(HERE, "mktitle.py"), SRC, "-o", out,
           "--cc", "clang"] + ["--cflag=" + f for f in TITLE_CFLAGS]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(out):
        return None, (r.stderr or r.stdout).strip()
    return out, None


def run_xenia(xex, timeout=90):
    """Launch headless, return parsed [T] events (retry on a dropped last line)."""
    events = []
    for _ in range(4):
        kill_stale_xenia()
        try:
            os.remove(XENIA_LOG)
        except OSError:
            pass
        try:
            subprocess.run([XENIA, xex, "--headless=true"], cwd=XENIA_CWD,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                           timeout=timeout)
        except subprocess.TimeoutExpired:
            pass
        kill_stale_xenia()
        events = []
        try:
            with open(XENIA_LOG, "r", encoding="utf-8", errors="replace") as f:
                for ln in f:
                    m = T_RE.search(ln)
                    if m:
                        events.append((m.group(1), m.group(2).rstrip()))
        except OSError:
            pass
        if any(k == "DONE" for k, _ in events):
            return events
    return events


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep", action="store_true", help="keep the build dir")
    args = ap.parse_args()

    print("building tests/xnamath/t_xnamath.cpp (scalar _XM_NO_INTRINSICS_ path) ...")
    xex, err = build()
    if not xex:
        print("BUILD FAILED\n" + (err or ""))
        return 1

    events = run_xenia(xex)
    checks = [(k, t) for k, t in events if k in ("PASS", "FAIL")]
    done = next((t for k, t in events if k == "DONE"), None)
    npass = sum(1 for k, _ in checks if k == "PASS")
    nfail = sum(1 for k, _ in checks if k == "FAIL")

    print("== xnamath ==  (%d/%d)%s" % (npass, npass + nfail,
          "" if done else "  DID NOT COMPLETE (crash/timeout)"))
    for k, t in checks:
        print("  %-4s %s" % ("ok" if k == "PASS" else "FAIL", t))

    ok = done is not None and nfail == 0
    print("\nSUMMARY: %s (%d/%d checks passed)" %
          ("clean" if ok else "PROBLEMS", npass, npass + nfail))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
