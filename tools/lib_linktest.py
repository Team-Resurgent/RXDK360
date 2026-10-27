#!/usr/bin/env python3
"""Whole-archive link test across the shipped XDK libraries.

For each shipped .lib: convert it with coff2elf, force every member in with
--whole-archive, and link it against our runtime plus the union of all the other
converted libs (so legitimate cross-library references resolve). Then scan the
output for the two failure classes the vcomp bring-up surfaced:

  * NON-kernel undefined symbols -- a real gap in our runtime (a shim we owe),
    as opposed to a kernel import (satisfied later by gen_import_stubs) or a
    linker-script symbol (provided by mktitle's layout).
  * WEAK-undefined symbols that resolved to 0 -- these link silently but branch
    or dereference to address 0 if ever used at runtime (the `??_E` vtable
    deleting-destructor trap). coff2elf now aliases those to their default, so
    the expectation is zero; any that appear are new traps to investigate.

    python tools/lib_linktest.py [--all] [--only a,b,c] [--release]

Default scans the release libs, skipping a *d / *i / *ltcg debug/profile/LTCG
variant only when stripping that suffix leaves an existing base lib (so base
libs that merely end in the letter -- nuiapi, nuifitnessapi -- are kept). --all
scans everything; --only restricts to a comma list of basenames (without .lib).
"""
import argparse
import glob
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import gen_import_stubs as g  # noqa: E402

LLD = os.path.join(ROOT, "build", "llvm", "bin", "ld.lld.exe")
CLANG = os.path.join(ROOT, "build", "llvm", "bin", "clang.exe")
COFF2ELF = os.path.join(HERE, "coff2elf.py")
RUNTIME = [os.path.join(ROOT, "build", "libc", "libcpp.a"),
           os.path.join(ROOT, "build", "libc", "libc.a")]
XDK = os.environ.get("RXDK_XDK",
                     r"C:\Program Files (x86)\Microsoft Xbox 360 SDK\lib\xbox")
COFF_DIR = os.path.join(ROOT, "build", "coff")
WORK = os.path.join(ROOT, "build", "linktest")

# Symbols the mktitle linker script defines (PROVIDE_HIDDEN), so an undefined
# here is a test artifact, not a runtime gap.
LAYOUT_SYMS = {
    "__eh_frame_start", "__eh_frame_end", "__eh_frame_hdr_start",
    "__eh_frame_hdr_end", "__init_array_start", "__init_array_end",
    "__fini_array_start", "__fini_array_end", "__xc_a", "__xc_z",
}


def sh(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def convert(lib_path, out_a):
    # Pass the runtime archives so coff2elf's external-strong set is correct: an
    # XDK COMDAT signature whose name the runtime owns strong (libc/libcpp) stays
    # strong, while pure header-inlines (FXL's fxl.inl family, XG* tiling helpers)
    # weaken -- so a title's own linkonce copy wins the COMDAT dedup instead of
    # resolution binding to the discarded XDK group (the "discarded section" dangle).
    runtime = []
    for rl in RUNTIME:
        if os.path.exists(rl):
            runtime += ["--runtime", rl]
    r = sh([sys.executable, COFF2ELF, "archive", lib_path, "-o", out_a,
            "--ppc64"] + runtime)
    return r.returncode == 0


def undefined_syms(elf_path):
    """(global_undef, weak_undef) name lists from an ELF's .symtab.

    Handles both ELFCLASS32 (Elf32_Shdr 40B / Elf32_Sym 16B) and ELFCLASS64
    (Elf64_Shdr 64B / Elf64_Sym 24B, with the reordered name/info/other/shndx/
    value/size layout) -- coff2elf --ppc64 emits the latter."""
    f = open(elf_path, "rb").read()
    elf64 = f[4] == 2
    if elf64:
        shoff = struct.unpack(">Q", f[0x28:0x30])[0]
        shent = struct.unpack(">H", f[0x3a:0x3c])[0]
        shnum = struct.unpack(">H", f[0x3c:0x3e])[0]
        # Elf64_Shdr: name,type,flags,addr,offset,size,link,info,align,entsize
        secs = [struct.unpack(">IIQQQQIIQQ", f[shoff + i * shent:shoff + i * shent + 64])
                for i in range(shnum)]
        symsz = 24
    else:
        shoff = struct.unpack(">I", f[0x20:0x24])[0]
        shent = struct.unpack(">H", f[0x2e:0x30])[0]
        shnum = struct.unpack(">H", f[0x30:0x32])[0]
        secs = [struct.unpack(">IIIIIIIIII", f[shoff + i * shent:shoff + i * shent + 40])
                for i in range(shnum)]
        symsz = 16
    symtabs = [s for s in secs if s[1] == 2]
    if not symtabs:
        return [], []
    # sh_offset=idx4, sh_size=idx5, sh_link=idx6 in both Shdr layouts.
    soff, ssize, link = symtabs[0][4], symtabs[0][5], symtabs[0][6]
    stroff = secs[link][4]
    glob, weak = [], []
    for j in range(ssize // symsz):
        rec = f[soff + j * symsz:soff + j * symsz + symsz]
        if elf64:
            nm, info, _other, shndx = struct.unpack(">IBBH", rec[:8])
        else:
            nm, _val, _sz, info, _other, shndx = struct.unpack(">IIIBBH", rec)
        if not nm or shndx != 0:
            continue
        end = f.index(b"\0", stroff + nm)
        name = f[stroff + nm:end].decode("latin1")
        b = info >> 4
        if b == 1:
            glob.append(name)
        elif b == 2:
            weak.append(name)
    return glob, weak


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--all", action="store_true", help="every lib, incl. d/i/ltcg")
    ap.add_argument("--only", help="comma list of basenames (no .lib)")
    args = ap.parse_args()

    os.makedirs(WORK, exist_ok=True)
    os.makedirs(COFF_DIR, exist_ok=True)

    libs = sorted(glob.glob(os.path.join(XDK, "*.lib")))
    names = {os.path.splitext(os.path.basename(p))[0]: p for p in libs}
    if args.only:
        want = set(args.only.split(","))
        names = {n: p for n, p in names.items() if n in want}
    elif not args.all:
        # A lib is a Debug/Profile/LTCG variant only if stripping its suffix
        # leaves an existing base lib -- otherwise a plain suffix test wrongly
        # drops base libs that merely END in the suffix letter (nuiapi/
        # nuifitnessapi end in "i" but are not Profile "i" variants).
        allnames = set(names)
        def _is_variant(n):
            for suf in ("ltcg", "d", "i"):
                if n.endswith(suf) and n[:-len(suf)] in allnames:
                    return True
            return False
        names = {n: p for n, p in names.items() if not _is_variant(n)}

    kernel = set(g.build_ordinal_index(XDK).keys()) | set(
        getattr(g, "SUPPLEMENTAL_ORDINALS", {}).keys())

    # a trivial entry object
    s0c = os.path.join(WORK, "s0.c")
    s0o = os.path.join(WORK, "s0.o")
    open(s0c, "w").write("void _start(void){}\n")
    sh([CLANG, "--target=powerpc64-unknown-xbox360", "-c", s0c, "-o", s0o])

    # convert every lib once into the union
    union = {}
    print("converting %d libs..." % len(names))
    for n, p in names.items():
        a = os.path.join(COFF_DIR, n + ".a")
        if convert(p, a):
            union[n] = a
        else:
            print("  CONVERT FAILED: %s" % n)

    print("linking each whole-archive against runtime + %d sibling libs...\n"
          % len(union))
    total_gaps, total_weak, bad = set(), set(), []
    for n, a in sorted(union.items()):
        others = [x for m, x in union.items() if m != n]
        elf = os.path.join(WORK, n + ".elf")
        cmd = ([LLD, "-e", "_start", "--noinhibit-exec",
                "--unresolved-symbols=ignore-all", "--no-demangle", s0o,
                "--whole-archive", a, "--no-whole-archive"]
               + others + RUNTIME + ["-o", elf])
        r = sh(cmd)
        if not os.path.exists(elf):
            print("  %-22s LINK ERROR" % n)
            bad.append(n)
            continue
        glob_u, weak_u = undefined_syms(elf)
        gaps = sorted(s for s in glob_u
                      if s not in kernel and s not in LAYOUT_SYMS)
        weak = sorted(s for s in weak_u if s not in LAYOUT_SYMS)
        total_gaps |= set(gaps)
        total_weak |= set(weak)
        flag = "" if not gaps and not weak else "  <==="
        print("  %-22s gaps=%-3d weak0=%-3d%s" % (n, len(gaps), len(weak), flag))
        if gaps:
            for s in gaps:
                print("        gap : %s" % s)
        if weak:
            for s in weak:
                print("        weak: %s" % s)

    print("\n==== AGGREGATE ====")
    print("distinct NON-kernel gaps across all libs: %d" % len(total_gaps))
    print("distinct WEAK-undef->0 across all libs:   %d" % len(total_weak))
    if bad:
        print("libs that failed to link: %s" % ", ".join(bad))


if __name__ == "__main__":
    main()
