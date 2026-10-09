#!/usr/bin/env python3
"""Bulk regression harness: build ALL corpus tests into one sample, run it once
in xenia, and diff each test's output.

Each tests/corpus/<name>/ holds main.c or main.cpp defining `void t_<name>(void)`
(C++ tests mark it extern "C") plus expected.txt (the lines it prints through
DbgPrint). The harness generates a driver that calls every t_<name>() behind a
`== <name> ==` marker, links them with the shared _prelude/start.c into ONE xex,
runs it once headless in xenia, splits the DbgPrint output on the markers, and
diffs each section against its expected.txt. One build and one xenia launch for
the whole corpus -- and it exercises every feature in a single address space,
like a real title.

A test with a `pending` file is known-blocked on the current runtime (e.g. it
needs the modern libc); it is left out of the combined sample and reported
PENDING (build-checked on its own so a newly-linkable one is surfaced).

A test with an `hw-only` file probes the console itself (e.g. which VMX
instructions Xenon implements) and is meaningful only on a devkit: xenia stops
the title on an instruction it does not implement. It is left out here and
run by tools/run_corpus_hw.py.

  python tools/run_corpus.py                 # run + report
  python tools/run_corpus.py --bless         # (re)write each expected.txt
  python tools/run_corpus.py --only hello,vcall
  python tools/run_corpus.py --lib libcMT,xapilib
"""
import argparse
import glob
import os
import re
import shlex
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CORPUS = os.path.join(ROOT, "tests", "corpus")
PRELUDE = os.path.join(CORPUS, "_prelude")
BUILD = os.path.join(ROOT, "build", "corpus")

XENIA = os.environ.get(
    "RXDK_XENIA", r"D:\Git\xenia-canary\build\bin\Windows\Release\xenia_canary.exe")
XENIA_CWD = os.environ.get("RXDK_XENIA_CWD", r"D:\Git\xenia_canary_windows")
XENIA_LOG = os.environ.get(
    "RXDK_XENIA_LOG", r"D:\Git\xenia-canary\build\bin\Windows\Release\xenia.log")

DBG_RE = re.compile(r"\(DbgPrint\)\s?(.*)")
SENTINEL = "##RXDK-CORPUS-END##"
MARK = "== %s =="                      # per-test section marker the driver prints


def find_tests(only):
    """Yield (name, srcdir, src, pending) for every corpus test."""
    for d in sorted(glob.glob(os.path.join(CORPUS, "*"))):
        name = os.path.basename(d)
        if name == "_prelude" or not os.path.isdir(d):
            continue
        if only and name not in only:
            continue
        src = None
        for cand in ("main.cpp", "main.c"):
            if os.path.exists(os.path.join(d, cand)):
                src = os.path.join(d, cand)
        if src:
            yield name, d, src, os.path.exists(os.path.join(d, "pending"))


def gen_driver(names):
    path = os.path.join(BUILD, "_driver.c")
    os.makedirs(BUILD, exist_ok=True)
    lines = ['#include "rt.h"', ""]
    for n in names:
        lines.append("void t_%s(void);" % n)
    lines += ["", "void title_main(void) {"]
    for n in names:
        lines.append('    DbgPrint("%s\\n"); t_%s();' % (MARK % n, n))
    lines += ["}", ""]
    with open(path, "w", newline="\n") as f:
        f.write("\n".join(lines))
    return path


def mktitle(sources, out, libs, extra):
    # The harness manages the lib set explicitly (the --lib default is the modern
    # runtime, but --lib libcMT,... selects the translated MS CRT instead), so opt
    # out of mktitle's default runtime auto-link to avoid pulling both.
    cmd = [sys.executable, os.path.join(HERE, "mktitle.py")] + sources + \
          ["-o", out, "--cc", "clang", "--no-default-libs", "--cflag=-I" + PRELUDE] + extra
    for lib in libs:
        cmd += ["--lib", lib]
    return subprocess.run(cmd, capture_output=True, text=True)


def run_once(xex):
    try:
        os.remove(XENIA_LOG)
    except OSError:
        pass
    try:
        subprocess.run([XENIA, xex, "--headless=true"], cwd=XENIA_CWD,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=25)
    except subprocess.TimeoutExpired:
        pass
    lines, complete = [], False
    try:
        with open(XENIA_LOG, "r", encoding="utf-8", errors="replace") as f:
            for ln in f:
                m = DBG_RE.search(ln)
                if not m:
                    continue
                t = m.group(1).rstrip()
                if t == SENTINEL:
                    complete = True
                else:
                    lines.append(t)
    except OSError:
        pass
    return lines, complete


def split_sections(lines, names):
    """Split the flat DbgPrint output into {name: [lines]} on the markers."""
    marks = {MARK % n: n for n in names}
    out = {n: [] for n in names}
    cur = None
    for ln in lines:
        if ln in marks:
            cur = marks[ln]
        elif cur is not None:
            out[cur].append(ln)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bless", action="store_true",
                    help="write each expected.txt from this run instead of diffing")
    ap.add_argument("--only", default="", help="comma-separated test names")
    ap.add_argument("--lib",
                    default=os.path.join(ROOT, "build", "libc", "libcpp.a") + "," +
                            os.path.join(ROOT, "build", "libc", "libc.a") + "," +
                            os.path.join(ROOT, "build", "coff", "xapilib.a"),
                    help="comma-separated runtime libraries to link (default: the "
                         "modern picolibc build/libc/libc.a; pass libcMT for the "
                         "translated MS CRT)")
    args = ap.parse_args()

    only = set(n for n in args.only.split(",") if n)
    libs = [n for n in args.lib.split(",") if n]

    tests = [t for t in find_tests(only)
             if not os.path.exists(os.path.join(t[1], "hw-only"))]
    runnable = [(n, d, s) for n, d, s, p in tests if not p]
    pending = [(n, d, s) for n, d, s, p in tests if p]

    rows = []
    fails = 0

    # one sample, one run
    if runnable:
        names = [n for n, _, _ in runnable]
        driver = gen_driver(names)
        sources = [s for _, _, s in runnable] + [os.path.join(PRELUDE, "start.c"), driver]
        # C++ exceptions on: DWARF EH via libunwind + libc++abi (build/libc/
        # libcpp.a). -funwind-tables so every frame has an FDE for the unwinder;
        # harmless for the C tests.
        extra = ["--cflag=-fexceptions", "--cflag=-funwind-tables"]
        out = os.path.join(BUILD, "corpus.xex")
        br = mktitle(sources, out, libs, extra)
        if br.returncode != 0 or not os.path.exists(out):
            tail = (br.stderr or br.stdout).strip().splitlines()[-3:]
            print("combined build FAILED:\n  " + "\n  ".join(tail))
            return 1
        actual, complete = run_once(out)
        for _ in range(4):
            if complete:
                break
            actual, complete = run_once(out)
        if args.bless and not complete:
            # never freeze a truncated run as golden (a dropped section would
            # bless empty and then falsely "pass" against itself)
            print("run never completed (no sentinel) -- refusing to bless")
            return 1
        sections = split_sections(actual, names)
        for name, srcdir, _ in runnable:
            got = sections[name]
            exp_path = os.path.join(srcdir, "expected.txt")
            if args.bless:
                with open(exp_path, "w", newline="\n") as f:
                    f.write("\n".join(got) + ("\n" if got else ""))
                rows.append((name, "BLESSED", []))
                continue
            expected = open(exp_path).read().splitlines() if os.path.exists(exp_path) else []
            if got == expected:
                rows.append((name, "PASS", []))
            else:
                fails += 1
                detail = []
                for i in range(max(len(got), len(expected))):
                    a = got[i] if i < len(got) else "<none>"
                    e = expected[i] if i < len(expected) else "<none>"
                    if a != e:
                        detail += ["exp: %s" % e, "got: %s" % a]
                rows.append((name, "MISMATCH", detail))

    # pending tests: build-check each on its own so a newly-linkable one shows up
    for name, srcdir, src in pending:
        out = os.path.join(BUILD, "pending_" + name + ".xex")
        argfile = os.path.join(srcdir, "build.args")
        extra = shlex.split(open(argfile).read()) if os.path.exists(argfile) else []
        br = mktitle([src, os.path.join(PRELUDE, "start.c")], out, libs, extra)
        reason = open(os.path.join(srcdir, "pending")).read().strip()
        if br.returncode == 0 and os.path.exists(out):
            rows.append((name, "PENDING?", ["builds now -- give it a t_%s() and drop the pending marker" % name]))
        else:
            rows.append((name, "PENDING", [reason]))

    width = max((len(n) for n, _, _ in rows), default=4)
    print()
    for name, status, detail in rows:
        print("  %-*s  %s" % (width, name, status))
        for d in detail:
            print("      " + d)
    npend = sum(1 for _, s, _ in rows if s.startswith("PENDING"))
    total = len(rows)
    if args.bless:
        print("\n%d test(s) blessed, %d pending." % (total - npend, npend))
        return 0
    passed = total - fails - npend
    print("\n%d/%d passed, %d failed, %d pending." % (passed, total, fails, npend))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
