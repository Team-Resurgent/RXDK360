#!/usr/bin/env python3
"""Compile a subset of picolibc for the Xbox 360 target into an ELF archive.

The start of the modern C23/C++23 runtime that replaces the translated 2013 MS
CRT (libcMT). It builds picolibc sources from vendor/picolibc with the patched
clang (powerpc-unknown-xbox360, the real MS-PPC ABI) straight to PPC ELF objects
-- no COFF translation, since we have the source -- and archives them into
build/libc/libc.a.

Brought up piece by piece, keeping the corpus (tools/run_corpus.py) green:
  * string / mem / ctype / errno  -- done
  * stdio (self-contained sprintf, incl. the MSVC %S rewrite) -- next
  * malloc (console pool ExAllocatePool)  -- done
  * C++ runtime                    -- later

Usage:
    python tools/build_libc.py [-o build/libc/libc.a]
"""
import argparse
import glob
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PICO = os.path.join(ROOT, "vendor", "picolibc")
CONFIG = os.path.join(ROOT, "runtime", "config")

CLANG = os.environ.get("RXDK_CLANG", os.path.join(ROOT, "build", "llvm", "bin", "clang.exe"))
AR = os.environ.get("RXDK_AR", os.path.join(ROOT, "build", "llvm", "bin", "llvm-ar.exe"))
TRIPLE = "powerpc64-unknown-xbox360"

# picolibc's own flags (mirrors RXDK-Libs build/xbox_target.zig picolibcFlags):
# -fno-builtin is essential -- picolibc's memcpy/strlen ARE the builtins, so
# letting clang recognise their loops as a memcpy/strlen idiom is infinite
# self-recursion; -include picolibc.h force-includes the config.
FLAGS = [
    "--target=" + TRIPLE, "-std=c17", "-O2", "-ffreestanding",
    "-ffunction-sections", "-fdata-sections",
    # Xbox 360 WCHAR is 16-bit UTF-16 (MSVC ABI). Our libc's wide-char routines
    # (wcslen, wcscpy_s, wmem*, ...) MUST use a 2-byte wchar_t so they agree with
    # the XDK libraries that call them -- otherwise wcslen miscounts a UTF-16
    # string and callers like XUI's XuiCopyString under-size a buffer, and the
    # 4-byte-wchar copy overruns it into adjacent heap metadata (a heap-corrupting
    # bug that surfaced as XUI's XuiInit trashing the process-heap free list).
    "-fshort-wchar",
    "-fno-stack-protector", "-fno-zero-initialized-in-bss",
    "-fno-sanitize=all", "-fno-builtin", "-Wno-everything",
    "-D__Picolibc__", "-D__TINY_STDIO",
    "-include", "picolibc.h",
]
INCLUDES = [
    "-I" + CONFIG,
    # Machine headers first so <machine/fenv.h> resolves to the powerpc version
    # (which pulls machine/fenv-fp.h -- the hardware-FPSCR fegetround/fesetround/...
    # bodies). The generic libc/include/machine/fenv.h only defines the FE_*
    # constants, so without this the fenv.c instantiation TU emits nothing and
    # fegetround is undefined. Only machine/fenv.h and machine/_ssp_tls.h are
    # shadowed (stack-protector is off), so nothing else changes.
    "-I" + os.path.join(PICO, "libc", "machine", "powerpc"),
    "-I" + os.path.join(PICO, "libc", "include"),
    "-I" + os.path.join(PICO, "libc", "stdio"),
    "-I" + os.path.join(PICO, "libc", "locale"),   # locale_private.h, internal
    "-I" + os.path.join(PICO, "libc", "ctype"),    # ctype local.h, internal
    "-I" + os.path.join(PICO, "libm", "common"),   # math_config.h / fdlibm internals
]

# Source subdirectories globbed wholesale (with per-file excludes below). Grows
# as more of the runtime is brought up.
SUBDIRS = [
    "libc/string",
    "libc/ctype",
    "libc/errno",
    "libc/stdio",
    "libc/locale",   # setlocale/newlocale/uselocale/localeconv -- libc++ <locale>
    "libc/time",     # strftime_l + time formatting the locale facets use
    "libc/stdlib",   # abs/labs/getenv/strtol/div/mb<->wc ... (allocator + atexit
                     # + abort + exit excluded below; our runtime provides those)
    "libc/search",   # qsort/qsort_r/bsearch (ISO C) + tsearch tree family; the
                     # file-backed BSD hash/ndbm cluster is excluded below.
    "libc/posix",    # POSIX regex engine + exec* front-ends + strfmon + the
                     # passwd/group database + wait/getlogin; files our own
                     # runtime already provides are excluded below.
]

# libm: full double + float math (libm/common + libm/math), the same wholesale
# glob RXDK-Libs uses on the original Xbox. The C++ STL needs it (<cmath>, and
# libc++'s hash table sizing calls ceilf). Only the soft long-double helpers
# (sl_* prefix) are skipped; the l-suffixed long-double entry points are kept, as
# on the original Xbox. Object files are parent-qualified (in main) so the two
# dirs' same-named members (and any libc clashes) don't collide in the flat objdir.
LIBM_SUBDIRS = [
    "libm/common",
    "libm/math",
    # libm/fenv: fenv.c re-includes the machine fenv inlines (powerpc FPSCR) as
    # out-of-line definitions, so callers that reference fegetround/fesetround/...
    # externally (libc++'s <cfenv>, float rounding) resolve. The consolidated
    # picolibc 'xbox' branch dropped these as "RXDK never builds" (true on the OG
    # side); the 360 needs them, so fenv.c is recovered from the xbox360 branch.
    "libm/fenv",
]

# Mirrors RXDK-Libs' libm excludes. In libm/common the transcendental *f and
# double cores duplicate the authoritative implementations in libm/math, so drop
# the common copies; gamma has no standalone TU.
LIBM_EXCLUDE = set([
    "cosf.c", "sinf.c", "sincosf.c",
    "exp.c", "exp2.c", "log.c", "log2.c", "pow.c", "s_log2.c",
    "sf_exp.c", "sf_exp2.c", "sf_log.c", "sf_log2.c", "sf_pow.c",
    "s_gamma.c", "sf_gamma.c",
])
# Float cores that only exist as sf_*.c: keep libm/math's copy (the authoritative
# one) even though the basename is in LIBM_EXCLUDE, so the symbol exists at all.
# exp2f (sf_exp2.c) is referenced by several graphics samples; without this it is
# an unresolved external (the bare-basename exclude dropped BOTH the common and
# the math copy).
LIBM_EXCLUDE_MATH_KEEP = set(["sf_exp2.c"])

# Extra (non-picolibc) glue. The .c files compile with the picolibc flags; the
# .cpp C++ runtime compiles with the C++ flag set below.
XBOX_GLUE = [
    os.path.join(ROOT, "runtime", "xbox", "ms_printf.c"),           # MSVC %S/%C/%I64 rewrite
    os.path.join(ROOT, "runtime", "xbox", "ms_crt_compat.c"),       # MS CRT compat: _stricmp/_snprintf/_wtoi/_s wrappers/security cookie (parity: shipped XDK libs link against ours)
    os.path.join(ROOT, "runtime", "xbox", "rt_support.c"),          # 64-bit div + console-pool malloc
    os.path.join(ROOT, "runtime", "xbox", "cxxrt.cpp"),             # operator new/delete + __cxa_*
    os.path.join(ROOT, "runtime", "xbox", "msvc_eh.c"),             # clean-room MSVC C++ EH (__CxxFrameHandler/_CxxThrowException) for shipped libs
    os.path.join(ROOT, "runtime", "xbox", "libc_hooks.c"),          # stdin/output/exec hook storage
    os.path.join(ROOT, "runtime", "xbox", "fileio.c"),              # POSIX file I/O over the kernel (open/read/write/lseek/close/stat...)
    os.path.join(ROOT, "runtime", "xbox", "dirio.c"),               # <dirent.h> + rename/truncate/scandir... for <filesystem>
    os.path.join(ROOT, "runtime", "xbox", "pathres.c"),             # cwd + relative-path resolver (getcwd/chdir/realpath) shared by fileio/dirio
    os.path.join(ROOT, "runtime", "xbox", "posix_ext.c"),           # sleep family, posix_memalign, isatty, access
    os.path.join(ROOT, "runtime", "xbox", "posix_stubs.c"),         # process/mmap/tty/perm stubs (no console concept)
    os.path.join(ROOT, "runtime", "xbox", "signals.c"),             # cooperative signal facility + ITIMER_REAL (setitimer/alarm)
    os.path.join(ROOT, "runtime", "xbox", "posix_unsupported.c"),   # linkable RXDK-STUB set: spawn/pipe/pty/ipc/timers/sched/identity
    os.path.join(ROOT, "runtime", "xbox", "syslog.c"),              # syslog -> cache:\syslog.log + DbgPrint mirror
    os.path.join(ROOT, "runtime", "xbox", "timer.c"),               # POSIX timer_create (SIGEV_THREAD/NONE) over a helper thread
    os.path.join(ROOT, "runtime", "xbox", "posix_glob.c"),          # fnmatch + glob + select/pselect + poll/ppoll (fd never blocks)
    os.path.join(ROOT, "runtime", "xbox", "ftw.c"),                 # ftw/nftw file-tree walk over opendir/readdir
    os.path.join(ROOT, "runtime", "xbox", "posix_string_ext.c"),    # basename/dirname (XPG) + ffs/ffsl/ffsll
    os.path.join(ROOT, "runtime", "xbox", "posix_stdio_streams.c"), # stdin/stdout/stderr FILE globals
    os.path.join(ROOT, "runtime", "xbox", "locks.c"),              # kernel critical-section retargetable locks
    os.path.join(ROOT, "runtime", "xbox", "threads.c"),            # C11 <threads.h> over the kernel (thrd/mtx/tss/once)
    os.path.join(ROOT, "runtime", "xbox", "sem.c"),                # POSIX unnamed semaphores over C11 mtx/cnd
    os.path.join(ROOT, "runtime", "xbox", "pthread.c"),            # POSIX threads over the C11 thread/mutex/cond primitives
    os.path.join(ROOT, "runtime", "xbox", "aio.c"),               # POSIX async I/O, implemented synchronously
    os.path.join(ROOT, "runtime", "xbox", "crt_start.c"),          # standard main() CRT startup (_start)
    os.path.join(ROOT, "runtime", "xbox", "tzinit.c"),             # default TZ from the console's time zone (GetTimeZoneInformation)
    os.path.join(ROOT, "runtime", "xbox", "clock.c"),              # clock_gettime over KeQuerySystemTime (for <chrono>)
    os.path.join(ROOT, "runtime", "xbox", "sysconf.c"),            # sysconf(_SC_NPROCESSORS_ONLN)=6 -> std::thread::hardware_concurrency
    os.path.join(ROOT, "runtime", "xbox", "c23_alloc.c"),          # C23 free_sized/free_aligned_sized
    os.path.join(ROOT, "runtime", "xbox", "atomic64.c"),           # __atomic_*_8 (64-bit atomics: <barrier>/<semaphore>/atomic-wait)
    os.path.join(ROOT, "runtime", "xbox", "setjmp.S"),             # setjmp/longjmp (PPC callee-saved GPR/FPR/LR/CR/SP)
    os.path.join(ROOT, "runtime", "xbox", "ke_perf.S"),            # 64-bit-safe KeQueryPerformanceFrequency wrapper (single-r3 MS return -> pointer)
    os.path.join(ROOT, "runtime", "xbox", "vmx_intrinsics.c"),     # MS PPC/VMX intrinsics clang lacks (__lvlx/__vaddfp/__mftb/byte-reverse/...)
    os.path.join(ROOT, "runtime", "xbox", "win32_wide.c"),         # complete Win32 *W family (narrow to the XDK's *A entries); XDK ships *A only

    os.path.join(ROOT, "runtime", "xbox", "fxl_bridge.cpp"),       # Itanium->MSVC forwarder for FXL's non-extern-"C" routines (FXLSetShaders)
    os.path.join(ROOT, "runtime", "xbox", "xdk_cpp_bridge.cpp"),   # Itanium->MSVC forwarders for XDK C++ APIs (XJSON/ASF/XAV/QNet)
]

# C++ runtime glue: no picolibc config force-include; freestanding, no EH/RTTI yet.
CPP_FLAGS = [
    "--target=" + TRIPLE, "-std=c++23", "-O2", "-ffreestanding",
    "-ffunction-sections", "-fdata-sections",
    "-fshort-wchar",   # 2-byte wchar_t to match the platform WCHAR (see FLAGS)
    "-fno-exceptions", "-fno-rtti", "-fno-stack-protector",
    "-fno-sanitize=all", "-fno-builtin", "-Wno-everything",
]

# Files in the globbed subdirs we do not want.
EXCLUDE = set([
    # tinystdio's vfprintf.c #includes these split parts, so they are not
    # standalone translation units.
    "conv_flt.c", "ultoa_invert.c",
    "vfprintf_char.c", "vfprintf_float.c", "vfprintf_int.c",
    "vfprintf_n.c", "vfprintf_str.c",
    # the Ryu float<->string engines duplicate the classic dtoa/ftoa engines
    # (which back the double printf variant), so keep only the classic ones.
    "ftoa_ryu.c", "dtoa_ryu.c", "atod_ryu.c", "atof_ryu.c",
    "ryu_divpow2.c", "ryu_log10.c", "ryu_log2pow5.c", "ryu_pow5bits.c",
    "ryu_table.c", "ryu_umul128.c",
    # replaced by runtime/xbox/ms_printf.c, which translates the MSVC format
    # (%S/%C/%I64 ...) before formatting over the same vfprintf engine.
    "sprintf.c", "snprintf.c", "swprintf.c", "vsnprintf.c",
    # picolibc's stdin/stdout/stderr use __weak_reference aliases lld does not
    # apply; runtime/xbox/posix_stdio_streams.c provides strong FILE* globals.
    "posixiob_stdin.c", "posixiob_stdout.c", "posixiob_stderr.c",
    # runtime/xbox/clock.c provides clock() (and clock_gettime over the PPC time
    # base); drop picolibc's libc/time/clock.c so the two don't both define clock.
    "clock.c",
    # libc/stdlib: our runtime already provides the allocator (rt_support.c over
    # ExAllocatePool), abort, and atexit/__cxa_atexit + exit sequencing
    # (cxxrt.cpp / crt_start.c), so drop picolibc's versions to avoid duplicate
    # symbols. The rest of stdlib (abs/labs/getenv/strtol/div/mb<->wc/...) is kept.
    "malloc.c", "free.c", "calloc.c", "realloc.c", "aligned_alloc.c",
    "memalign.c", "posix-memalign.c", "valloc.c", "pvalloc.c",
    "reallocarray.c", "reallocf.c", "mallinfo.c", "mallopt.c",
    "malloc-error.c", "malloc-stats.c", "malloc-usable-size.c",
    "abort.c", "assert_no_arg.c",
    "atexit.c", "cxa-atexit.c", "onexit.c", "exitprocs.c", "exit.c", "_Exit.c",
    # libc/search: keep qsort/qsort_r/bsearch and the in-memory tsearch tree
    # family; drop the on-disk BSD hash database (hsearch/ndbm), which is backed
    # by file I/O the console does not need.
    "hash.c", "hash_bigkey.c", "hash_buf.c", "hash_func.c", "hash_log2.c",
    "hash_page.c", "hcreate.c", "hcreate_r.c", "ndbm.c",
    # libc/posix: our runtime already provides these (basename/dirname in
    # posix_string_ext.c, fnmatch in posix_glob.c, creat in fileio.c, sleep in
    # posix_ext.c), so drop picolibc's copies to avoid duplicate symbols.
    "basename.c", "dirname.c", "fnmatch.c", "creat.c", "sleep.c",
    # engine.c is the regex matcher template #included (twice) by regexec.c, not
    # a standalone translation unit.
    "engine.c",
])


def sources():
    out = []
    for sub in SUBDIRS:
        for f in sorted(glob.glob(os.path.join(PICO, sub, "*.c"))):
            if os.path.basename(f) in EXCLUDE:
                continue
            out.append(f)
    for sub in LIBM_SUBDIRS:
        for f in sorted(glob.glob(os.path.join(PICO, sub, "*.c"))):
            b = os.path.basename(f)
            # LIBM_EXCLUDE drops the libm/common duplicates of cores whose
            # authoritative implementation lives in libm/math. Excluding by bare
            # basename also dropped libm/math's own copy, so a float core that
            # ONLY exists as sf_*.c (exp2f -> sf_exp2.c) vanished entirely. Keep
            # the libm/math copy of those; only the libm/common duplicate is a
            # real clash to drop.
            if b in LIBM_EXCLUDE_MATH_KEEP and sub.endswith("math"):
                out.append(f)
                continue
            if b in LIBM_EXCLUDE or b.startswith("sl_"):
                continue
            out.append(f)
    out += XBOX_GLUE
    return out


# Leaf ABI/runtime glue that MS-compiled prebuilt libs call and picolibc does not
# provide. Pull the verified objects from the translated libcMT rather than
# hand-writing them -- reusing the shipped glue, like the rest of the toolchain.
# Each member listed defines ONLY these helpers (no collision with our symbols;
# checked with tools/lib_parity.py):
#   crtgpr.o  __savegprlr_N / __restgprlr_N   (GPR save/restore)
#   crtfpr.o  __savefpr_N / __restfpr_N       (FPR save/restore)
#   crtvmx.o  __savevmx_N / __restvmx_N       (VMX/AltiVec save/restore)
#   u64tod.o  __u64tod                        (uint64 -> double)
#   chkstk.o  _RtlCheckStack / _RtlCheckStack12 (stack probe)
#   jmpuwind.o __jump_unwind                  (SEH longjmp-unwind helper)
# (_blkmov is NOT reused here -- its member memcpyp.o also defines memcpy, which
# would collide with ours; it is hand-written in runtime/xbox/ms_crt_compat.c.)
LIBCMT = os.path.join(ROOT, "build", "coff", "libcMT.a")
MS_GLUE_MEMBERS = ("crtgpr.o", "crtfpr.o", "crtvmx.o",
                   "u64tod.o", "chkstk.o", "jmpuwind.o")


def extract_ms_glue(objdir):
    if not os.path.exists(LIBCMT):
        print("  note: %s not built, MS register helpers not bundled" % LIBCMT)
        return []
    sys.path.insert(0, HERE)
    from coff2elf import read_archive
    out = []
    for member, _ln in read_archive(open(LIBCMT, "rb").read()):
        base = member.name.rstrip("/")
        if base in MS_GLUE_MEMBERS:
            p = os.path.join(objdir, "msglue_" + base)
            with open(p, "wb") as f:
                f.write(member.data)
            out.append(p)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-o", "--out", default=os.path.join(ROOT, "build", "libc", "libc.a"))
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("--debug", action="store_true",
                    help="build the _DEBUG variant (define _DEBUG, -O0 -g, assertions "
                         "active) -- for the libcMTd/libcpMTd drop-in variants")
    args = ap.parse_args()

    if args.debug:
        global FLAGS, CPP_FLAGS
        FLAGS = ["-O0" if f == "-O2" else f for f in FLAGS] + ["-g", "-D_DEBUG"]
        CPP_FLAGS = ["-O0" if f == "-O2" else f for f in CPP_FLAGS] + ["-g", "-D_DEBUG"]

    if not os.path.exists(CLANG):
        sys.exit("patched clang not found: %s (build tools/build-llvm.bat)" % CLANG)

    objdir = os.path.join(os.path.dirname(args.out), "obj")
    os.makedirs(objdir, exist_ok=True)

    objs, failed = [], []
    for src in sources():
        # Qualify the object with its parent directory: several picolibc trees
        # (libm/common vs libm/math especially) carry same-named members that
        # would otherwise overwrite each other in the flat objdir.
        parent = os.path.basename(os.path.dirname(os.path.abspath(src)))
        obj = os.path.join(objdir, parent + "_" + os.path.splitext(os.path.basename(src))[0] + ".o")
        if os.path.splitext(src)[1].lower() in (".cpp", ".cc", ".cxx"):
            cmd = [CLANG] + CPP_FLAGS + ["-c", src, "-o", obj]
        else:
            cmd = [CLANG] + FLAGS + INCLUDES + ["-c", src, "-o", obj]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            failed.append((src, r.stderr.strip().splitlines()[-1:] or [""]))
            if args.verbose:
                print("FAIL %s\n%s" % (src, r.stderr))
        else:
            objs.append(obj)

    if failed:
        print("%d source(s) failed to compile:" % len(failed))
        for src, why in failed:
            print("  %s: %s" % (os.path.relpath(src, ROOT), why[0] if why else ""))
        sys.exit(1)

    objs += extract_ms_glue(objdir)

    if os.path.exists(args.out):
        os.remove(args.out)
    # The object list is well past the Windows command-line length limit now, so
    # pass it to llvm-ar through a response file (@file) rather than argv.
    rsp = os.path.join(objdir, "ar.rsp")
    with open(rsp, "w", newline="\n") as f:
        f.write("\n".join('"%s"' % o.replace("\\", "/") for o in objs))
    r = subprocess.run([AR, "rcs", args.out, "@" + rsp], capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("archive failed:\n" + r.stderr)
    print("wrote %s: %d objects" % (os.path.relpath(args.out, ROOT), len(objs)))


if __name__ == "__main__":
    main()
