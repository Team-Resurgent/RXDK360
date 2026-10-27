#!/usr/bin/env python3
"""Regenerate build/coff: translate the XDK import/static libraries to ppc64
ELFv2 archives the clang toolset links against.

The modern toolchain is powerpc64-unknown-xbox360 (ILP32-on-ppc64, ELFv2), so
every XDK lib\\xbox\\*.lib is translated with coff2elf's --ppc64 mode into
build/coff/<name>.a plus a <name>.imports.json manifest. The runtime archives
(libcpp.a, libc.a) supply coff2elf's external-strong set so XDK COMDATs the
runtime owns stay strong and header-inline COMDATs weaken (see coff2elf.py).

This had been done by hand at the ppc64 cutover; a stale build/coff (ELF64 but
e_flags=0, pre the coff2elf ELFv2 fix) links with "incompatible" errors against
the ELFv2 sample objects, so keep it reproducible here.

    python tools/build_coff.py [--only a,b,c]

Requires the clang bundle (build/llvm) and the runtime archives (build/libc)
already built -- run build_variants.py / build_libc.py first.
"""
import argparse
import glob
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
COFF2ELF = os.path.join(HERE, "coff2elf.py")
COFF_DIR = os.path.join(ROOT, "build", "coff")
RUNTIME = [os.path.join(ROOT, "build", "libc", "libcpp.a"),
           os.path.join(ROOT, "build", "libc", "libc.a")]
XDK = os.environ.get("RXDK_XDK",
                     r"C:\Program Files (x86)\Microsoft Xbox 360 SDK\lib\xbox")


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--only", help="comma list of basenames (no .lib)")
    ap.add_argument("--xdk", default=XDK, help="XDK lib\\xbox dir")
    args = ap.parse_args()

    if not os.path.isdir(args.xdk):
        sys.exit("XDK lib dir not found: %s (set RXDK_XDK or --xdk)" % args.xdk)
    os.makedirs(COFF_DIR, exist_ok=True)

    runtime = []
    for rl in RUNTIME:
        if os.path.exists(rl):
            runtime += ["--runtime", rl]

    names = sorted(os.path.splitext(os.path.basename(p))[0]
                   for p in glob.glob(os.path.join(args.xdk, "*.lib")))
    if args.only:
        want = set(args.only.split(","))
        names = [n for n in names if n in want]
    if not names:
        sys.exit("no matching .lib under %s" % args.xdk)

    ok, fail = 0, []
    for n in names:
        lib = os.path.join(args.xdk, n + ".lib")
        out_a = os.path.join(COFF_DIR, n + ".a")
        manifest = os.path.join(COFF_DIR, n + ".imports.json")
        r = subprocess.run(
            [sys.executable, COFF2ELF, "archive", lib,
             "-o", out_a, "-m", manifest, "--ppc64"] + runtime)
        if r.returncode == 0 and os.path.exists(out_a):
            ok += 1
        else:
            fail.append(n)

    print("build/coff: %d ok, %d failed%s"
          % (ok, len(fail), (" (" + ",".join(fail) + ")") if fail else ""))
    sys.exit(1 if fail else 0)


if __name__ == "__main__":
    main()
