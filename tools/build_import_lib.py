#!/usr/bin/env python3
"""Build the RXDK-360 global kernel import library.

A stock XDK title links the console kernel the way any program links an OS: it
calls DbgPrint / KeSetEvent / ... and the linker resolves them from an import
library. The public XDK ships those as COFF short-import members (name -> module
+ ordinal). This tool translates that WHOLE surface (xboxkrnl.exe, xam.xex,
xbdm.xex, ... -- see gen_import_stubs.build_ordinal_index) into ONE ELF archive,
kernel_import.a, that every title links against. Each import is its own COMDAT
group of two sections:

    .kthunks.<name>  (code)  the 16-byte call thunk defining <name>, so a
                             `bl <name>` resolves; the loader rewrites it.
    .kvars.<name>    (data)  the __imp_<name> import record (and, for a data
                             export, <name> aliased onto that slot).

Because each import is its own gc-droppable group, --gc-sections keeps exactly
the thunks a title's code reaches and drops the other ~2500 -- so linking is a
single pass with no per-title genstubs and no trial link. The record words carry
the ordinal but leave the module_index field zero; elf2xex fills the real index
in once it has grouped the surviving imports into the XEX import name table.

    python tools/build_import_lib.py --xdk <lib\\xbox> -o build/libc/kernel_import.a

Record word layout (matches gen_import_stubs, which this replaces):
    (kind << 24) | (module_index << 16) | ordinal
    kind 1 = thunk first word, kind 2 = thunk second word, 0 = IAT slot.
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import gen_import_stubs as g  # noqa: E402

CLANG = os.environ.get("RXDK_CLANG", os.path.join(ROOT, "build", "llvm", "bin", "clang.exe"))
AR = os.environ.get("RXDK_AR", os.path.join(ROOT, "build", "llvm", "bin", "llvm-ar.exe"))
TRIPLE = "powerpc64-unknown-xbox360"


def emit_asm(index):
    """Assembly for the whole import surface: one COMDAT group per symbol."""
    out = ["# RXDK-360 global kernel import library -- generated, do not edit.",
           "# module_index is left 0; elf2xex patches it per the XEX name table.", ""]
    for name in sorted(index):
        module, ordinal, is_var = index[name]
        if not module or not ordinal:
            continue
        if not name.isascii() or not all(c.isalnum() or c in "_@$." for c in name):
            continue                       # skip malformed/non-identifier members
        # A function needs a call thunk AND an IAT record; a data export needs
        # only the IAT slot, with the base symbol aliased onto it.
        if not is_var:
            out += [
                f'    .section .kthunks.{name},"axG",@progbits,{name},comdat',
                f"    .globl {name}",
                f"{name}:",
                f"    .long 0x{0x01000000 | ordinal:08X}, 0x{0x02000000 | ordinal:08X}, 0x7D6903A6, 0x4E800420",
            ]
        out += [
            f'    .section .kvars.{name},"awG",@progbits,{name},comdat',
            f"    .globl __imp_{name}",
        ]
        if is_var:
            out += [f"    .globl {name}", "    .p2align 2", f"{name}:"]
        out += [f"__imp_{name}:", f"    .long 0x{ordinal:08X}", ""]
    return "\n".join(out) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--xdk", default=os.environ.get(
        "RXDK_XDK", r"C:\Program Files (x86)\Microsoft Xbox 360 SDK\lib\xbox"))
    ap.add_argument("-o", "--out", default=os.path.join(ROOT, "build", "libc", "kernel_import.a"))
    args = ap.parse_args()

    index = g.build_ordinal_index(args.xdk)
    asm = emit_asm(index)
    work = os.path.join(ROOT, "build", "libc")
    os.makedirs(work, exist_ok=True)
    s_path = os.path.join(work, "kernel_import.s")
    o_path = os.path.join(work, "kernel_import.o")
    open(s_path, "w", newline="\n", encoding="utf-8").write(asm)

    r = subprocess.run([CLANG, "--target=" + TRIPLE, "-c", s_path, "-o", o_path],
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stderr)
        sys.exit("assemble failed")
    if os.path.exists(args.out):
        os.remove(args.out)
    r = subprocess.run([AR, "rcs", args.out, o_path], capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stderr)
        sys.exit("ar failed")
    n = sum(1 for _n, (_m, o, _v) in index.items() if o)
    print(f"{args.out}: {n} kernel imports (one COMDAT group each)")


if __name__ == "__main__":
    main()
