#!/usr/bin/env python3
"""Corpus regression harness for REAL HARDWARE (a 360 devkit), mirroring
tools/run_corpus.py's xenia flow over XBDM.

Why a separate runner: xenia is more permissive than a real kit on exactly the
issues the corpus guards -- it loads an unsigned XEX, and it does not reproduce
the fopen / D3DXCompileShader wedges. So a green xenia run can hide a real-kit
regression; this runs the same corpus, with the same expected.txt files, on the
kit. It reuses run_corpus.py's build, section-split and diff so the two backends
stay in lock-step -- only deploy/launch/capture differ.

Deploy + launch + capture go through the RxdkXbdm CLI
(vs20xx/debugger/Rxdk.Xbox360.Xbdm, built as Rxdk.Xbox360.Xbdm.exe) `debuglaunch <console> <xex> <bp>`: it
deploys the XEX, reboots the kit into it stopped at entry, then continues with a
debugger attached and streams notifications until its wait window ends. The
debugger MUST stay attached: a real kit routes a title's DbgPrint over XBDM only
to an attached debugger (a plain `run` + `watch` sees system messages but not the
title's output), so we pass a breakpoint at an address the title never executes
(its PE-header page) to hold the attach open. Each DbgPrint arrives as a
`debugstr` notification whose `string` field is the printed text; the corpus
driver ends with the sentinel ##RXDK-CORPUS-END##, and a run that never prints it
within the window is treated as a HANG.

Hang isolation (a wedge on a real kit kills XBDM -- hard power-cycle needed): a
test dir carrying an `hw-isolate` file (e.g. tests/corpus/fileio, the issue #4
repro) is left OUT of the main combined sample and run on its own AFTER it, so a
wedge costs only that test's result, not the whole suite. On a detected hang the
runner pauses and asks the operator to power-cycle the kit before continuing.

    python tools/run_corpus_hw.py                 # RXDK_CONSOLE or the kit default
    python tools/run_corpus_hw.py --only hello,vcall
    python tools/run_corpus_hw.py --console 192.168.1.50

Requires a devkit reachable over XBDM and the built RxdkXbdm.exe. This runner's
launch/capture path is HW-specific and cannot be exercised without a kit; the
build and diff logic it shares with run_corpus.py is what the xenia suite covers.
"""
import argparse
import os
import re
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import run_corpus as rc  # noqa: E402  (shared build / split / constants)

# Rxdk.Xbox360.Xbdm.exe: the managed XBDM CLI (deploy/launch/watch). Override with RXDK_XBDM.
XBDM = os.environ.get("RXDK_XBDM", os.path.join(
    ROOT, "vs20xx", "debugger", "Rxdk.Xbox360.Xbdm", "bin", "Release",
    "net10.0-windows", "win-x86", "Rxdk.Xbox360.Xbdm.exe"))
CONSOLE = os.environ.get("RXDK_CONSOLE", "")     # empty -> RxdkXbdm's DefaultConsole
HANG_TIMEOUT = int(os.environ.get("RXDK_HW_TIMEOUT", "90"))   # seconds per sample

# watch prints debug strings as: "<HH:MM:SS.fff>  debugstr thread=0x.. string=<text>"
DEBUGSTR_RE = re.compile(r"\bdebugstr\b.*?\bstring=(.*)$")


def is_isolated(srcdir):
    return os.path.exists(os.path.join(srcdir, "hw-isolate"))


def _hold_bp(xex):
    """A breakpoint address the title never executes -- its PE-header page, which
    is read-only data, not code. Planting it makes debuglaunch stay attached
    (WaitFor the BP) and stream notifications instead of detaching immediately."""
    try:
        b = open(xex, "rb").read()
        sec = struct.unpack_from(">I", b, 0x10)[0]
        base = struct.unpack_from(">I", b, sec + 8 + 0x108)[0]   # imageInfo.loadAddress
    except Exception:                                            # noqa: BLE001
        base = 0x82000000
    return "0x%08X" % (base + 0x40)                              # inside the DOS/PE header


def launch_and_capture(xex, timeout):
    """Deploy + reboot into `xex` stopped at entry, continue with the debugger
    attached, and stream the title's DbgPrint until the sentinel or `timeout`.
    Returns (lines, complete). One RxdkXbdm connection, so no run/watch race."""
    lines, complete = [], False
    cmd = [XBDM, "debuglaunch"] + ([CONSOLE] if CONSOLE else []) + [xex, _hold_bp(xex)]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    deadline = time.time() + timeout
    try:
        while time.time() < deadline:
            ln = proc.stdout.readline()
            if not ln:
                if proc.poll() is not None:
                    break
                continue
            m = DEBUGSTR_RE.search(ln)
            if not m:
                continue
            text = m.group(1).rstrip("\r\n")
            if text == rc.SENTINEL:
                complete = True
                break
            lines.append(text)
    finally:
        if proc.poll() is None:
            proc.terminate()
    return lines, complete


def build_sample(runnable, out_name):
    names = [n for n, _, _ in runnable]
    driver = rc.gen_driver(names)
    sources = [s for _, _, s in runnable] + [os.path.join(rc.PRELUDE, "start.c"), driver]
    extra = ["--cflag=-fexceptions", "--cflag=-funwind-tables"]
    out = os.path.join(rc.BUILD, out_name)
    br = rc.mktitle(sources, out, DEFAULT_LIBS, extra)
    if br.returncode != 0 or not os.path.exists(out):
        tail = (br.stderr or br.stdout).strip().splitlines()[-3:]
        return None, "\n  ".join(tail)
    return out, None


def run_sample(runnable, out_name, rows):
    """Build, deploy, run and diff one combined sample of `runnable` tests."""
    xex, err = build_sample(runnable, out_name)
    if not xex:
        for n, _, _ in runnable:
            rows.append((n, "BUILD-FAIL", [err]))
        return False
    names = [n for n, _, _ in runnable]
    actual, complete = launch_and_capture(xex, HANG_TIMEOUT)
    sections = rc.split_sections(actual, names)
    for name, srcdir, _ in runnable:
        exp_path = os.path.join(srcdir, "expected.txt")
        expected = open(exp_path).read().splitlines() if os.path.exists(exp_path) else []
        got = sections[name]
        if not complete and not got:
            rows.append((name, "HANG?", ["no output and no sentinel -- kit may be wedged"]))
        elif got == expected:
            rows.append((name, "PASS", []))
        else:
            detail = []
            for i in range(max(len(got), len(expected))):
                a = got[i] if i < len(got) else "<none>"
                e = expected[i] if i < len(expected) else "<none>"
                if a != e:
                    detail += ["exp: %s" % e, "got: %s" % a]
            rows.append((name, "MISMATCH", detail))
    return complete


DEFAULT_LIBS = [os.path.join(ROOT, "build", "libc", "libcpp.a"),
                os.path.join(ROOT, "build", "libc", "libc.a"),
                os.path.join(ROOT, "build", "coff", "xapilib.a")]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    global CONSOLE, HANG_TIMEOUT
    ap.add_argument("--only", default="", help="comma-separated test names")
    ap.add_argument("--console", default=None, help="kit IP/name (default: RXDK_CONSOLE / kit default)")
    ap.add_argument("--timeout", type=int, default=None, help="per-sample seconds (default %d)" % HANG_TIMEOUT)
    args = ap.parse_args()
    if args.console:
        CONSOLE = args.console
    if args.timeout:
        HANG_TIMEOUT = args.timeout
    if not os.path.exists(XBDM):
        print("RxdkXbdm.exe not found at %s (build the debugger, or set RXDK_XBDM)." % XBDM)
        return 2

    only = set(n for n in args.only.split(",") if n)
    tests = [(n, d, s) for n, d, s, p in rc.find_tests(only) if not p]
    main_pass = [(n, d, s) for n, d, s in tests if not is_isolated(d)]
    isolated = [(n, d, s) for n, d, s in tests if is_isolated(d)]

    rows = []
    # 1) main sample: all non-isolated tests in one build/deploy/run
    if main_pass:
        print("== main sample: %d test(s) ==" % len(main_pass))
        run_sample(main_pass, "corpus_hw.xex", rows)

    # 2) isolated pass: hang-prone tests, one at a time, AFTER the main results
    #    are safely captured. A wedge here needs a manual power-cycle to recover.
    for n, d, s in isolated:
        print("== isolated: %s (hang-prone) ==" % n)
        complete = run_sample([(n, d, s)], "corpus_hw_%s.xex" % n, rows)
        if not complete:
            try:
                input("  kit may be wedged -- power-cycle it, wait for it to boot, "
                      "then press Enter to continue... ")
            except EOFError:
                print("  (non-interactive: skipping remaining isolated tests)")
                break

    width = max((len(n) for n, _, _ in rows), default=4)
    print()
    fails = 0
    for name, status, detail in rows:
        print("  %-*s  %s" % (width, name, status))
        for dline in detail:
            print("      " + dline)
        if status not in ("PASS",):
            fails += 1
    print("\n%d/%d passed on hardware." % (len(rows) - fails, len(rows)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
