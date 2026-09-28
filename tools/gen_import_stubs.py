#!/usr/bin/env python3
"""Generate XEX import thunks for the kernel functions a title calls.

Compiled code that calls, say, DbgPrint emits a `bl DbgPrint` with DbgPrint left
undefined. This reads those undefined symbols from the object, looks each up in
the XDK's import libraries (xboxkrnl.lib etc., whose short-import members carry
the real console ordinals), and emits:

  * a PPC assembly stub the title links against -- one 16-byte thunk named after
    each function (so the `bl` resolves) plus a 4-byte variable record; the
    loader rewrites the thunk to a syscall and resolves the ordinal, and
  * a JSON manifest naming the records per library, for elf2xex --import-manifest.

The thunks go in their own section, away from the entry code, because xenia
declares each thunk as its own function and one abutting the entry's basic block
makes the entry look like an undefined extern.

Usage:
    python tools/gen_import_stubs.py <title.o> --xdk <lib dir> \\
        -o stubs.s --manifest stubs.json
"""
import argparse
import glob
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from coff2elf import read_archive, parse_short_import, IMAGE_FILE_MACHINE_POWERPCBE


# module (DLL) name the XEX import header should carry, by import-lib basename.
MODULE_NAMES = {
    "xboxkrnl": "xboxkrnl.exe",
    "xbdm": "xbdm.xex",
}


# Kernel exports the console has but the public XDK import libraries do not
# expose (Microsoft did not ship import stubs for them). The ordinals are real
# xboxkrnl.exe ordinals -- verified against xenia's export table -- so importing
# them by ordinal resolves on both xenia and hardware. Used only as a fallback
# when a symbol is missing from the XDK libs.
SUPPLEMENTAL_ORDINALS = {
    "KeTlsAlloc":    ("xboxkrnl.exe", 0x152),
    "KeTlsFree":     ("xboxkrnl.exe", 0x153),
    "KeTlsGetValue": ("xboxkrnl.exe", 0x154),
    "KeTlsSetValue": ("xboxkrnl.exe", 0x155),
}


def build_ordinal_index(xdk_lib_dir):
    """name -> (module_name, ordinal, is_var) across the XDK import libraries.

    The importing module is taken from each short-import's own DLL field (e.g.
    "xam.xex@21256.0+1861.0" -> xam.xex), not the containing .lib -- a single lib
    such as xapilib.lib carries stubs for several modules (xam.xex functions like
    XGetLanguage live inside xapilib.lib, not a xam.lib). is_var is true for a
    data export (a variable, e.g. ExLoadedCommandLine): it needs the variable
    import form, not a call thunk."""
    index = {name: (mod, ordv, False) for name, (mod, ordv) in SUPPLEMENTAL_ORDINALS.items()}
    for path in glob.glob(os.path.join(xdk_lib_dir, "*.lib")):
        base = os.path.splitext(os.path.basename(path))[0].lower()
        blob = open(path, "rb").read()
        for member, _longnames in read_archive(blob):
            if member.name in ("/", "//"):
                continue
            if member.data[:2] == struct.pack("<H", IMAGE_FILE_MACHINE_POWERPCBE):
                continue
            try:
                sym, dll, ordinal, _nt, itype = parse_short_import(member.data)
            except Exception:
                continue
            if not ordinal:
                continue
            module = dll.split("@")[0].strip()      # "xam.xex@..." -> "xam.xex"
            module = module or MODULE_NAMES.get(base)
            if module:
                # IMPORT_OBJECT_DATA(1)/_CONST(2) => a variable, not a function.
                index.setdefault(sym, (module, ordinal, itype in (1, 2)))
    return index


def read_undefined_symbols(blob):
    """Undefined global symbol names from a big-endian PPC ELF object.

    Handles both ELFCLASS32 (powerpc-unknown-xbox360) and ELFCLASS64
    (powerpc64-unknown-xbox360, ILP32-on-ppc64) objects.
    """
    if blob[:4] != b"\x7fELF":
        sys.exit("not an ELF object")
    is64 = blob[4] == 2
    if is64:
        (e_shoff,) = struct.unpack_from(">Q", blob, 0x28)
        e_shentsize, e_shnum, e_shstrndx = struct.unpack_from(">HHH", blob, 0x3A)

        def sh(i):
            o = e_shoff + i * e_shentsize
            name, typ = struct.unpack_from(">II", blob, o)
            off, size = struct.unpack_from(">QQ", blob, o + 0x18)
            link, = struct.unpack_from(">I", blob, o + 0x28)
            entsize, = struct.unpack_from(">Q", blob, o + 0x38)
            return (name, typ, off, size, link, entsize)

        def read_sym(o):
            # Elf64_Sym: name(4), info(1), other(1), shndx(2), value(8), size(8)
            st_name, st_info, _o, st_shndx = struct.unpack_from(">IBBH", blob, o)
            return st_name, st_info, st_shndx
    else:
        (e_shoff,) = struct.unpack_from(">I", blob, 0x20)
        e_shentsize, e_shnum, e_shstrndx = struct.unpack_from(">HHH", blob, 0x2E)

        def sh(i):
            n, typ, _fl, _ad, off, size, link, _inf, _al, es = \
                struct.unpack_from(">IIIIIIIIII", blob, e_shoff + i * e_shentsize)
            return (n, typ, off, size, link, es)

        def read_sym(o):
            # Elf32_Sym: name(4), value(4), size(4), info(1), other(1), shndx(2)
            st_name, _v, _sz, st_info, _o, st_shndx = \
                struct.unpack_from(">IIIBBH", blob, o)
            return st_name, st_info, st_shndx

    symtab = None
    for i in range(e_shnum):
        if sh(i)[1] == 2:                              # SHT_SYMTAB
            symtab = sh(i)
    if not symtab:
        return []
    strtab = sh(symtab[4])                             # sh_link -> strtab
    off, size, entsize, stroff = symtab[2], symtab[3], symtab[5], strtab[2]
    out = []
    for k in range(size // entsize):
        st_name, st_info, st_shndx = read_sym(off + k * entsize)
        bind = st_info >> 4
        if st_shndx == 0 and st_name and bind != 0:    # SHN_UNDEF, global/weak
            end = blob.index(b"\0", stroff + st_name)
            out.append(blob[stroff + st_name:end].decode("utf-8", "replace"))
    return out


def main():
    import json
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("obj", nargs="?",
                    help="compiled title object (.o) with undefined kernel symbols")
    ap.add_argument("--xdk", required=True, help="XDK lib\\xbox directory")
    ap.add_argument("--names", default=None,
                    help="comma-separated import names instead of reading an object "
                         "(e.g. the undefined symbols a full link reports)")
    ap.add_argument("-o", "--out", default="stubs.s")
    ap.add_argument("--manifest", default=None)
    args = ap.parse_args()

    index = build_ordinal_index(args.xdk)
    if args.names:
        undefined = [n for n in args.names.split(",") if n]
    elif args.obj:
        undefined = read_undefined_symbols(open(args.obj, "rb").read())
    else:
        ap.error("provide an object or --names")

    resolved = {}                          # module -> [(name, ordinal, is_var)]
    unresolved = []
    for name in undefined:
        if name in index:
            module, ordinal, is_var = index[name]
            resolved.setdefault(module, []).append((name, ordinal, is_var))
        else:
            unresolved.append(name)

    # Emit the stub assembly. Two import forms (xex_module.cc / ImageXex):
    #
    #   record = (kind << 24) | (module_index << 16) | ordinal
    #     kind 0 = IAT slot, kind 1 = thunk first word, kind 2 = thunk second word
    #     module_index is the library's index in the XEX name table (ImageXex).
    #     xenia ignores bits 16-23; HvxResolveImports uses them (C0000225 if 0
    #     on a non-first library, e.g. XGetVideoMode looked up in xboxkrnl).
    #
    #   function -> 16-byte .kthunks (kind1, kind2, mtctr, bctr) PLUS .kvars IAT
    #   variable -> ONE 4-byte .kvars IAT, base symbol aliased onto that slot
    lines = ["# Generated import thunks -- do not edit.", "    .section .kthunks,\"ax\"", ""]
    for module_index, (module, funcs) in enumerate(resolved.items()):
        for name, ordinal, is_var in funcs:
            if is_var:
                continue
            rec = (module_index << 16) | ordinal
            lines += [f"    .globl {name}", f"{name}:",
                      f"    .long 0x{0x01000000 | rec:08X}, 0x{0x02000000 | rec:08X}, 0x7D6903A6, 0x4E800420", ""]
    lines += ["    .section .kvars,\"aw\"", ""]
    for module_index, (module, funcs) in enumerate(resolved.items()):
        rec_mod = module_index << 16
        for name, ordinal, is_var in funcs:
            lines += [f"    .globl __imp_{name}"]
            if is_var:                     # base symbol reads the patched pointer
                lines += [f"    .globl {name}", "    .p2align 2", f"{name}:"]
            lines += [f"__imp_{name}:", f"    .long 0x{rec_mod | ordinal:08X}", ""]
    with open(args.out, "w", newline="\n") as f:
        f.write("\n".join(lines))

    # Manifest: one entry per import record address, exactly once (xenia patches
    # a slot the first time it sees it; a second listing would re-read the patched
    # value as a bogus record). A function has two records (var slot + thunk); a
    # variable has one (the shared slot).
    def records_for(funcs):
        out = []
        for name, _o, is_var in funcs:
            out.append(f"__imp_{name}")
            if not is_var:
                out.append(name)
        return out

    manifest_path = args.manifest or (args.out.rsplit(".", 1)[0] + ".imports.json")
    manifest = {"libraries": [
        {"module": module, "records": records_for(funcs)}
        for module, funcs in resolved.items()]}
    with open(manifest_path, "w") as f:
        json.dump(manifest, f, indent=2)

    total = sum(len(v) for v in resolved.values())
    nvar = sum(1 for v in resolved.values() for _n, _o, iv in v if iv)
    print(f"{args.out}: {total} imports across {len(resolved)} module(s)"
          + (f" ({nvar} variable)" if nvar else ""))
    for module, funcs in resolved.items():
        print(f"  {module}: " + ", ".join(
            f"{n}(0x{o:X}{'/var' if iv else ''})" for n, o, iv in funcs))
    if unresolved:
        print(f"  unresolved (not kernel imports): {', '.join(unresolved)}")


if __name__ == "__main__":
    main()
