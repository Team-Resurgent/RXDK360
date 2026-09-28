#!/usr/bin/env python3
"""Build the RXDK-360 compat library, libcompat.a.

Clean clang reimplementations of the XDK C++ helper classes that titles subclass
(XAPOBase now; more later). Unlike a coff2elf translation of the MSVC .lib, these
give the base class the modern *Itanium* C++ ABI -- vtable, RTTI, this-adjusting
thunks, mangled names -- that a clang-compiled subclass links against. Compiled
in XDK-headers mode (against the XDK's own headers, exactly as a title is), then
archived. libcompat is OUR code, so it ships prebuilt in the clang runtime and
nothing runs on the user's machine.

    python tools/build_libcompat.py            -> build/libc/libcompat.a

Needs the XDK headers at build time (set RXDK_XDK_INC, default the installed SDK).
"""
import glob
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

CLANG = os.environ.get("RXDK_CLANG", os.path.join(ROOT, "build", "llvm", "bin", "clang.exe"))
AR = os.environ.get("RXDK_AR", os.path.join(ROOT, "build", "llvm", "bin", "llvm-ar.exe"))
TRIPLE = "powerpc64-unknown-xbox360"

XDK_INC = os.environ.get("RXDK_XDK_INC",
                         r"C:\Program Files (x86)\Microsoft Xbox 360 SDK\include\xbox")
CFG = os.path.join(ROOT, "runtime", "config")
PICO = os.path.join(ROOT, "vendor", "picolibc", "libc", "include")
LLVM = os.environ.get("RXDK_LLVM") or os.path.join(ROOT, "build", "llvm")
if not os.path.isdir(os.path.join(LLVM, "libcxx", "include")):
    LLVM = os.path.join(ROOT, "vendor", "llvm-project")
LX = os.path.join(LLVM, "libcxx", "include")
LA = os.path.join(LLVM, "libcxxabi", "include")

SRC_DIR = os.path.join(ROOT, "runtime", "compat")
OUT = os.path.join(ROOT, "build", "libc", "libcompat.a")


def flags():
    """The XDK-headers C++ compile environment, mirroring the ClangCompile task's
    AutoClangFlags(C++) + XdkHeaderFlags so libcompat parses the stock XDK headers
    the same way a title does (2-byte wchar, MS extensions, the Win32/Xbox gates)."""
    base = [
        "--target=" + TRIPLE, "-O2",
        "-fshort-wchar", "-ffunction-sections", "-fdata-sections",
        "-fexceptions", "-funwind-tables", "-frtti",     # RTTI: emits _ZTI... the subclass needs
        "-D__Picolibc__", "-D_GNU_SOURCE",
        "-I" + LX, "-I" + LA, "-I" + CFG,
        "-include", "__config_site", "-include", "rxdk_libcpp_prereq.h",
        "-I" + PICO, "-include", "picolibc.h",
    ]
    ms = [
        "-fms-extensions", "-fms-compatibility", "-fdeclspec",
        "-fms-compatibility-version=1920",
        "-D_INTPTR_T_DEFINED", "-D_UINTPTR_T_DEFINED",
        "-D__gnuc_va_list=__builtin_va_list", "-D_HAS_CHAR16_T_LANGUAGE_SUPPORT=1",
        "-D__STDC_WANT_LIB_EXT1__=1", "-DDECLSPEC_UUID(x)=__declspec(uuid(x))",
        "-D_MSC_FULL_VER=140050727", "-fdelayed-template-parsing",
        "-Wno-invalid-token-paste", "-Wno-narrowing", "-fwritable-strings", "-fno-autolink",
        "-D_WIN32=1", "-D_M_PPCBE=1", "-D_M_PPC=1", "-D_XBOX=1", "-D_XBOX_VER=200",
        "-D__export=", "-D_SIZE_T_DEFINED", "-D_XM_NO_INTRINSICS_",
        "-Wno-everything",
    ]
    force = [
        "-include", "stdint.h", "-include", "__stddef_max_align_t.h",
        "-include", "rxdk_msvcrt_compat.h", "-include", "rxdk_secure_overloads.h",
    ]
    return base + ms + force + ["-isystem", XDK_INC]


def main():
    if not os.path.isdir(XDK_INC):
        sys.exit("XDK headers not found: %s (set RXDK_XDK_INC)" % XDK_INC)
    sources = sorted(glob.glob(os.path.join(SRC_DIR, "*.cpp")))
    if not sources:
        sys.exit("no sources in %s" % SRC_DIR)
    work = os.path.join(ROOT, "build", "libc")
    os.makedirs(work, exist_ok=True)
    objs = []
    fl = flags()
    for src in sources:
        obj = os.path.join(work, "compat_" + os.path.splitext(os.path.basename(src))[0] + ".o")
        cmd = [CLANG] + fl + ["-c", src, "-o", obj]
        print("==> " + os.path.basename(src))
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            sys.stderr.write(r.stderr)
            sys.exit("compile failed: " + src)
        objs.append(obj)
    if os.path.exists(OUT):
        os.remove(OUT)
    r = subprocess.run([AR, "rcs", OUT] + objs, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stderr)
        sys.exit("ar failed")
    print("%s: %d object(s)" % (OUT, len(objs)))


if __name__ == "__main__":
    main()
