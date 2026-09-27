#!/usr/bin/env python3
"""Build an Xbox 360 title end to end: compile -> resolve kernel imports ->
link -> pack, in one command.

This folds the manual dance (zig cc -c / gen_import_stubs / zig cc link with a
hand-written layout script / elf2xex) into a single step. It:

  1. compiles each C/C++/asm source (prebuilt .o files pass through),
  2. writes a linker script that lays the image out the way the packer needs --
     room below the first section for the synthesised PE headers, the import
     thunks in their own section past a small gap (so no thunk abuts the entry's
     basic block), and the writable region (.data/.bss) aligned to its own page
     so per-section descriptors can map it read-write,
  3. trial-links to discover the still-undefined symbols (the kernel imports),
     resolves their ordinals from the XDK import libraries and emits the linkable
     thunks plus an import manifest,
  4. links the final ELF against the objects, the stubs and any libraries, and
  5. packs it into a XEX.

Usage:
    python tools/mktitle.py build/ctitle/apidata.c -o build/ctitle/apidata.xex \\
        --lib xapilib,libcMT

Libraries named without a path or extension are resolved to <coff-dir>/<name>.a
(the archives coff2elf.py produces from the XDK .libs).
"""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

TARGET = "powerpc-freestanding-none"
MS_TRIPLE = "powerpc-unknown-xbox360"                  # the patched-clang MS-ABI target
DEFAULT_XDK = r"C:\Program Files (x86)\Microsoft Xbox 360 SDK\lib\xbox"
DEFAULT_CLANG = os.environ.get("RXDK_CLANG",
                               os.path.join(ROOT, "build", "llvm", "bin", "clang.exe"))
DEFAULT_LLD = os.environ.get("RXDK_LLD",
                             os.path.join(ROOT, "build", "llvm", "bin", "ld.lld.exe"))
DEFAULT_BASE = 0x82000000
CFLAGS = ["-target", TARGET, "-O2", "-fno-sanitize=all"]


def zig():
    return os.environ.get("ZIG", "zig")


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def page_size_for(base):
    """Match the packer: 0x80000000-0x8FFFFFFF pages in 64KB, 0x90000000+ in 4KB."""
    return 0x10000 if base < 0x90000000 else 0x1000


def write_layout(path, base, page):
    """The layout the packer expects, parameterised by load base and page size."""
    with open(path, "w", newline="\n") as f:
        f.write(f"""ENTRY(_start)
SECTIONS {{
  . = 0x{base:08X};
  . += 0x1000;                       /* room for the synthesised PE headers.
                                        First 64KB page is headers + RO data (the
                                        ImageXex .rdata layout). Putting .text here
                                        mixed CODE with the header page; HW imagexex
                                        then reports IM1031 and LDRX C000007B. */
  .rodata : {{
    *(.rodata*)
    /* Merge the per-function LSDA sections (-fexceptions emits one
       .gcc_except_table.<mangled> per function). Without an explicit rule lld
       leaves each as its own orphan section -- a C++/EH-heavy title (e.g. one
       using <iostream>) then has hundreds of sections, and the synthesised PE
       header outgrows the headroom below the first section. Gathering them here
       (read-only, addressed by the FDE, so any loaded section works) keeps the
       section count -- and the header -- small for every title, automatically. */
    *(.gcc_except_table .gcc_except_table.*)
  }}
  .pdata : {{                        /* RUNTIME_FUNCTION table from the retail MS
                                        libs. Gather it into one named .pdata
                                        section so the packer fills the PE exception
                                        directory and xenia -- and the HW unwinder --
                                        get exact function boundaries. Without it
                                        xenia's heuristic analyser merges functions
                                        into multi-MB blobs: ~40x slower JIT warmup
                                        and the arena "oversized alloc" overflows.
                                        No KEEP: each .pdata is grouped with its
                                        function's .text, so --gc-sections keeps it
                                        iff the function survives (KEEP would instead
                                        root every function's .pdata and drag in dead
                                        code that calls unlinked libs). */
    *(.pdata)
    *(.pdata.*)
  }}
  .xdata : {{                        /* unwind info the .pdata entries point at */
    *(.xdata)
    *(.xdata.*)
  }}
  .eh_frame : {{                     /* distinct RO section (not merged with .rodata):
                                        libunwind recovers length from the PE section
                                        table at runtime. Same attributes as .rodata
                                        so it may share the header/RO 64KB page. */
    PROVIDE_HIDDEN(__eh_frame_start = .);
    KEEP(*(.eh_frame))
    KEEP(*(.eh_frame.*))
    PROVIDE_HIDDEN(__eh_frame_end = .);
    /* No .eh_frame_hdr (lld does not synthesise one for this PE target); give
       libunwind's DWARF-index path empty start==end markers so it falls back to
       a linear .eh_frame scan. */
    PROVIDE_HIDDEN(__eh_frame_hdr_start = .);
    PROVIDE_HIDDEN(__eh_frame_hdr_end = .);
  }}
  . = ALIGN(0x{page:X});             /* CODE after RO so xenia does not disassemble
                                        format strings, and so HW does not mix RX
                                        with the header page (IM1031 / C000007B). */
  .text   : {{ *(.text*) }}
  . = ALIGN(0x{page:X});             /* import thunks on their own CODE page, clear
                                        of both the entry code and the rodata */
  .kthunks : ALIGN(16) {{ KEEP(*(.kthunks)) }}
  . = ALIGN(0x{page:X});             /* writable region. .kvars is the IAT: HV patches
                                        it in place (LDRX C0000225 STATUS_NOT_FOUND if
                                        the records sit on a Header/Resource page). */
  .kvars  : {{ KEEP(*(.kvars)) }}
  .init_array : {{                   /* C++ static constructors, run pre-main by start.c */
    PROVIDE_HIDDEN(__init_array_start = .);
    KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
    KEEP(*(.init_array))
    PROVIDE_HIDDEN(__init_array_end = .);
  }}
  .fini_array : {{                   /* destructors -- gather here (writable) so they
                                        do not land in the .kthunks CODE page and flip
                                        it to a data page (which breaks bl <thunk>) */
    PROVIDE_HIDDEN(__fini_array_start = .);
    KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
    KEEP(*(.fini_array))
    PROVIDE_HIDDEN(__fini_array_end = .);
  }}
  .CRT : {{                          /* MS CRT initializer table (.CRT$XCA..XCZ),
                                        how prebuilt MS libs register pre-main
                                        constructors; run by start.c via __xc_a/z */
    PROVIDE_HIDDEN(__xc_a = .);
    KEEP(*(SORT_BY_NAME(.CRT$XC*)))
    PROVIDE_HIDDEN(__xc_z = .);
  }}
  .data : {{ *(.data*) }}
  .bss  : {{ *(.bss*) *(COMMON) }}
  /DISCARD/ : {{ *(.comment) *(.note*) }}
}}
""")


def auto_clang_flags(is_cpp):
    """The standard modern-runtime compile environment injected for clang titles,
    so a C or C++ title -- including one using the STL / <iostream> -- builds
    without spelling out the include set. Returned flags are placed BEFORE the
    user's --cflag entries, so those still win on any conflict (e.g. -O0,
    -fno-exceptions).

    C sources get the picolibc environment. C++ sources additionally get the
    exception/RTTI flags, the libc++/libc++abi headers (BEFORE picolibc so
    <cstdlib> etc. resolve to libc++'s wrappers, matching build_libcpp.py), the
    __config_site + prereq force-includes, and -D_GNU_SOURCE -- picolibc gates
    the POSIX/GNU locale API (locale_t, uselocale ...) that libc++'s <locale> /
    <iostream> need behind it, off under strict -std. """
    cfg = os.path.join(ROOT, "runtime", "config")
    pico = os.path.join(ROOT, "vendor", "picolibc", "libc", "include")
    if not is_cpp:
        # _GNU_SOURCE makes the POSIX/GNU surface visible (dup/pread/access/
        # posix_memalign/nanosleep ... are gated behind it in picolibc's headers).
        return ["-D__Picolibc__", "-D_GNU_SOURCE", "-I" + cfg, "-I" + pico,
                "-include", "picolibc.h"]
    llvm = os.environ.get("RXDK_LLVM") or os.path.join(ROOT, "build", "llvm")
    if not os.path.isdir(os.path.join(llvm, "libcxx", "include")):
        llvm = os.path.join(ROOT, "vendor", "llvm-project")
    lx = os.path.join(llvm, "libcxx", "include")
    la = os.path.join(llvm, "libcxxabi", "include")
    return ["-fexceptions", "-funwind-tables", "-frtti",
            "-D__Picolibc__", "-D_GNU_SOURCE",
            "-I" + lx, "-I" + la, "-I" + cfg,
            "-include", "__config_site", "-include", "rxdk_libcpp_prereq.h",
            "-I" + pico, "-include", "picolibc.h"]


def compile_sources(sources, workdir, cc, clang, cflags):
    """Compile each source to an object; pass prebuilt .o through.

    cc == "zig"   -> zig cc, the PPC EABI (works for simple titles);
    cc == "clang" -> the patched clang targeting powerpc-unknown-xbox360, which
                     emits the real MS-PPC ABI the shipped libraries expect.

    cflags are extra compile flags (include dirs, -fno-exceptions, ...) appended
    to the C/C++ compile of every non-assembly source.
    """
    objects = []
    for src in sources:
        ext = os.path.splitext(src)[1].lower()
        if ext == ".o":
            objects.append(src)
            continue
        # unique object name: many titles have several sources all called main.c
        # (one per test dir), so qualify the object with its parent directory.
        stem = os.path.splitext(os.path.basename(src))[0]
        parent = os.path.basename(os.path.dirname(os.path.abspath(src)))
        obj = os.path.join(workdir, (parent + "_" + stem if parent else stem) + ".o")
        is_asm = ext in (".s", ".asm")
        is_cpp = ext in (".cpp", ".cc", ".cxx", ".c++")
        if cc == "clang":
            cmd = [clang, "--target=" + MS_TRIPLE, "-c", src, "-o", obj]
            if not is_asm:
                # titles get the modern standard the runtime targets (C23/C++23);
                # picolibc itself is built separately at c17 (build_libc.py).
                # -fshort-wchar: the Xbox 360 MSVC ABI uses a 2-byte wchar_t
                # (UTF-16), and the runtime (build_libc.py) is built the same way.
                # Clang defaults this target to a 4-byte wchar_t, which would make
                # every wide string a title passes to the libc (wcstol, wcstod,
                # char_traits<wchar_t>, ...) unreadable -- the 2-byte reader hits
                # the high zero half of the first character and sees an empty
                # string. Match the runtime so wide chars work.
                std = "-std=c++23" if is_cpp else "-std=c23"
                cmd[2:2] = ["-O2", std, "-fshort-wchar"] + auto_clang_flags(is_cpp) + cflags
        elif is_asm:                                   # zig: assembly, no C-only flags
            cmd = [zig(), "cc", "-target", TARGET, "-c", src, "-o", obj]
        else:
            cmd = [zig(), "cc"] + CFLAGS + cflags + ["-c", src, "-o", obj]
        r = run(cmd)
        if r.returncode != 0:
            sys.exit(f"compile failed for {src}:\n{r.stderr}")
        objects.append(obj)
    return objects


def link(objects, libs, stubs, layout, out_elf, lld=DEFAULT_LLD, gc=True,
         ldflags=()):
    """Link objects (+ optional assembled stubs .o + libs) into the ELF.

    Links with the LLVM lld we ship (ld.lld), not an external driver -- the
    productised toolchain is self-contained. ld.lld links objects, so the import
    thunks must already be assembled (see main); the layout script's ENTRY(_start)
    plus -e _start sets the entry. A caller-supplied -Wl,a,b flag (the old zig
    driver form) is unwrapped into raw linker arguments for compatibility."""
    # --error-limit=0: the trial link deliberately fails, and its full undefined
    # list IS the kernel-import discovery. ld.lld stops after 20 errors by
    # default, which silently truncates that list -- a title with more than ~20
    # kernel imports would get an incomplete stub set and still fail to link.
    # --no-demangle: ld.lld demangles C++ names in "undefined symbol:" errors by
    # default, which splits a name like foo(unsigned int, void*) across spaces --
    # undefined_from() then parses fragments ("unsigned", "void") and
    # gen_import_stubs looks those up as symbols. Keep the raw mangled names so
    # the kernel-import discovery and the error text are both correct.
    # --no-dependent-libraries: XDK headers (use_ansi.h et al) bake
    # #pragma comment(lib, "libcpmt") into every object that includes them, which
    # clang emits as a .deplibs record; ld.lld would otherwise follow it and fail
    # to find MSVC's C++ static runtime, which the clang toolchain deliberately
    # does not ship (issue #3). Every archive we need is named explicitly, so
    # dropping the auto-pull loses nothing. Matches the VS path (ClangLink.cs).
    cmd = [lld, "-T", layout, "-e", "_start", "--error-limit=0", "--no-demangle",
           "--no-dependent-libraries"]
    if gc:
        cmd.append("--gc-sections")
    for f in ldflags:
        cmd += f[len("-Wl,"):].split(",") if f.startswith("-Wl,") else [f]
    cmd += objects
    if stubs:
        cmd.append(stubs)
    # Wrap the archives in a group so lld re-scans them to a fixpoint: the title
    # libs, the C++ runtime and libc are mutually dependent (e.g. a title lib
    # object pulled late references __cxa_* in libcpp.a, which a single
    # left-to-right pass would leave unresolved).
    if libs:
        cmd += ["--start-group"] + list(libs) + ["--end-group"]
    cmd += ["-o", out_elf]
    return run(cmd)


UNDEFINED_RE = re.compile(r"undefined symbol: (\S+)")


def undefined_from(link_result):
    """Parse the undefined-symbol names from a failed link."""
    text = link_result.stdout + link_result.stderr
    seen = []
    for name in UNDEFINED_RE.findall(text):
        if name not in seen:
            seen.append(name)
    return seen


def resolve_libs(names, coff_dir):
    out = []
    for n in names:
        if os.sep in n or n.endswith(".a") or os.path.isabs(n):
            out.append(n)
        else:
            out.append(os.path.join(coff_dir, n + ".a"))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("sources", nargs="+", help="C/C++/asm sources or prebuilt .o")
    ap.add_argument("-o", "--out", required=True, help="output .xex")
    ap.add_argument("--base", type=lambda s: int(s, 0), default=DEFAULT_BASE,
                    help="load base address (default 0x82000000)")
    ap.add_argument("--lib", action="append", default=[],
                    help="extra library archive(s), comma-separated; a bare name "
                         "-> <coff-dir>/<name>.a. Added on TOP of the default "
                         "runtime libs (see --no-default-libs)")
    ap.add_argument("--no-default-libs", action="store_true",
                    help="do not auto-link the clang runtime (libc.a, xapilib.a, "
                         "and libcpp.a for C++). Use when linking libcMT instead, "
                         "or managing the lib set by hand")
    ap.add_argument("--coff-dir", default=os.path.join(ROOT, "build", "coff"),
                    help="where bare --lib names resolve (coff2elf archives)")
    ap.add_argument("--xdk", default=DEFAULT_XDK, help="XDK lib\\xbox directory")
    ap.add_argument("--cc", choices=("zig", "clang"), default="clang",
                    help="compiler: clang (patched MS-PPC ABI -- the real toolchain, "
                         "default) or zig (legacy PPC EABI, simple titles only)")
    ap.add_argument("--clang", default=DEFAULT_CLANG,
                    help="patched clang path (for --cc clang)")
    ap.add_argument("--lld", default=DEFAULT_LLD,
                    help="ld.lld path (the LLVM linker we ship)")
    ap.add_argument("--cflag", action="append", default=[],
                    help="extra compile flag (repeatable), e.g. --cflag -Iinc "
                         "--cflag -fno-exceptions")
    ap.add_argument("--ldflag", action="append", default=[],
                    help="extra linker flag (repeatable), passed to the driver, "
                         "e.g. --ldflag=-Wl,--allow-multiple-definition")
    ap.add_argument("--keep-elf", action="store_true",
                    help="keep the intermediate .elf next to the output")
    ap.add_argument("--no-sign", dest="sign", action="store_false",
                    help="leave the packed XEX unsigned and unencrypted (default: "
                         "debug-sign + -encrypt it so a real kit will load it -- "
                         "see tools/xex_debugsign.py)")
    ap.add_argument("--no-encrypt", dest="encrypt", action="store_false",
                    help="debug-sign but do not encrypt (loads in xenia; a stock "
                         "devkit needs the encrypted form)")
    args = ap.parse_args()

    if args.cc == "clang" and not os.path.exists(args.clang):
        sys.exit(f"patched clang not found: {args.clang} (build tools/build-llvm.bat)")

    out_base = os.path.splitext(args.out)[0]
    workdir = os.path.dirname(os.path.abspath(args.out)) or "."
    os.makedirs(workdir, exist_ok=True)
    page = page_size_for(args.base)

    # The low-level "just works" default for a clang title: our modern CRT --
    # libc.a (the libcMT replacement, auto-linked like the XDK's /MT) plus
    # libcpp.a for C++ -- and xapilib.a, the one XDK component always present in
    # every official configuration and already translated. xboxkrnl's imports are
    # synthesised by the import-stub step below, so it needs no static lib.
    #
    # This is deliberately minimal: the official XDK links a much larger default
    # set that varies per solution configuration (d3d9/xgraphics/xnet/xaudio2/
    # xact3/x3daudio/xmcore/vcomp/xbdm, with debug/profile/ltcg suffixes). That
    # full per-config list is the project template's job once those components are
    # brought up for the clang toolchain; a title needing them adds them via
    # --lib in the meantime. --no-default-libs opts out entirely (to link libcMT
    # instead, or to manage the set by hand). User --lib entries go first, the
    # runtime after, matching the official link order (title libs, then CRT).
    default_libs = []
    if args.cc == "clang" and not args.no_default_libs:
        libc_dir = os.path.join(ROOT, "build", "libc")
        # Always include libcpp.a: libc.a itself references the C++ runtime (e.g.
        # xbox_cxxrt.o -> __cxa_uncaught_exceptions), so the two are mutually
        # dependent and are linked as a group below. On-demand archive extraction
        # plus --gc-sections means a pure-C title pulls nothing extra from it, so
        # this cannot be gated on the source extensions (a prebuilt C++ .o has
        # none to detect anyway).
        default_libs.append(os.path.join(libc_dir, "libcpp.a"))
        default_libs.append(os.path.join(libc_dir, "libc.a"))
        default_libs.append(os.path.join(args.coff_dir, "xapilib.a"))

    libnames = [n for spec in args.lib for n in spec.split(",") if n]
    libs = resolve_libs(libnames, args.coff_dir) + default_libs
    for lib in libs:
        if not os.path.exists(lib):
            sys.exit(f"library not found: {lib}")

    objects = compile_sources(args.sources, workdir, args.cc, args.clang, args.cflag)

    # [interim ABI bridge] Some translated-MSVC d3d9 entry points take a 64-bit
    # integer BY VALUE, which the Xenon MS ABI passes in ONE 64-bit register. Our
    # 32-bit clang splits it across a register PAIR, so the callee loses the low
    # 32 bits (root cause of the AdvancedLighting vertex-fetch spikes). Link the
    # asm shims (runtime/xbox/d3d9_abi_shims.s) and --wrap the affected symbols so
    # the pair is re-packed into a single 64-bit register before the real call.
    # Harmless when a title links no d3d9 (the shim GC's, --wrap is a no-op).
    # REMOVE this and the shims once clang moves to powerpc64-unknown-xbox360
    # (docs/ilp32-ppc64-abi-plan.md) -- the fixed clang makes the re-pack wrong.
    d3d9_wraps = []
    if args.cc == "clang" and not args.no_default_libs:
        shim_s = os.path.join(ROOT, "runtime", "xbox", "d3d9_abi_shims.s")
        shim_o = out_base + "_d3d9shims.o"
        rs = run([args.clang, "--target=" + MS_TRIPLE, "-c", shim_s, "-o", shim_o])
        if rs.returncode != 0:
            sys.exit(f"assembling d3d9 ABI shims failed:\n{rs.stderr}")
        objects.append(shim_o)
        d3d9_wraps = ["-Wl,--wrap,D3DDevice_SetStreamSource"]
    ldflags_all = list(args.ldflag) + d3d9_wraps

    layout = out_base + ".ld"
    write_layout(layout, args.base, page)

    elf = out_base + ".elf"

    # trial link (no stubs) to discover the undefined kernel imports
    trial = link(objects, libs, None, layout, elf, lld=args.lld, gc=True,
                 ldflags=ldflags_all)
    undefined = undefined_from(trial) if trial.returncode != 0 else []

    manifest = None
    stubs = None
    if undefined:
        stubs_s = out_base + "_stubs.s"
        manifest = out_base + "_stubs.json"
        r = run([sys.executable, os.path.join(HERE, "gen_import_stubs.py"),
                 "--xdk", args.xdk, "--names", ",".join(undefined),
                 "-o", stubs_s, "--manifest", manifest])
        sys.stdout.write(r.stdout)
        if r.returncode != 0:
            sys.exit(r.stderr or "gen_import_stubs failed")
        # gen_import_stubs prints "unresolved (not kernel imports): a, b" for
        # any undefined that is not a kernel export -- a real link error.
        m = re.search(r"unresolved \(not kernel imports\): (.+)", r.stdout)
        if m:
            sys.exit(f"unresolved symbols (not kernel imports): {m.group(1).strip()}")
        # ld.lld links objects, not assembly: assemble the thunks with our clang.
        stubs = out_base + "_stubs.o"
        ra = run([args.clang, "--target=" + MS_TRIPLE, "-c", stubs_s, "-o", stubs])
        if ra.returncode != 0:
            sys.exit(f"assembling import stubs failed:\n{ra.stderr}")

    # final link
    final = link(objects, libs, stubs, layout, elf, lld=args.lld, gc=True,
                 ldflags=ldflags_all)
    if final.returncode != 0:
        sys.exit(f"link failed:\n{final.stdout}{final.stderr}")

    # pack
    cmd = [sys.executable, os.path.join(HERE, "elf2xex.py"), elf, "-o", args.out]
    if manifest:
        cmd += ["--import-manifest", manifest]
    if not args.sign:
        cmd += ["--no-sign"]
    elif not args.encrypt:
        cmd += ["--no-encrypt"]
    r = run(cmd)
    sys.stdout.write(r.stdout)
    if r.returncode != 0:
        sys.exit(r.stderr or "elf2xex failed")

    if not args.keep_elf and os.path.exists(elf):
        os.remove(elf)


if __name__ == "__main__":
    main()
