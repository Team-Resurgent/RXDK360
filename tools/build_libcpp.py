#!/usr/bin/env python3
"""Build the C++ exception runtime for the Xbox 360 target into build/libc/libcpp.a.

DWARF/Itanium EH, the same shape RXDK-Libs uses on the original Xbox:
  * libunwind        -- the DWARF unwinder (baremetal: it reads .eh_frame
                        between the __eh_frame_start/_end markers).
  * libc++abi (core) -- __cxa_throw / __cxa_begin_catch / the personality routine
                        / type_info matching / std::terminate handlers.

Only the exception machinery is built, not the full libc++/STL. Titles that use
exceptions link this alongside libc.a; the linker script (mktitle.py) gathers
.eh_frame into one section bracketed by __eh_frame_start/__eh_frame_end, and
ASSERTs that the markers sit at the section's edges.
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def _llvm_root():
    env = os.environ.get("RXDK_LLVM")
    for candidate in (env, os.path.join(ROOT, "build", "llvm"),
                      os.path.join(ROOT, "vendor", "llvm-project")):
        if candidate and os.path.isdir(os.path.join(candidate, "libcxx", "include")):
            return candidate
    return os.path.join(ROOT, "build", "llvm")


LLVM = _llvm_root()
PICO = os.path.join(ROOT, "vendor", "picolibc")
CONFIG = os.path.join(ROOT, "runtime", "config")

CLANG = os.environ.get("RXDK_CLANG", os.path.join(ROOT, "build", "llvm", "bin", "clang.exe"))
AR = os.environ.get("RXDK_AR", os.path.join(ROOT, "build", "llvm", "bin", "llvm-ar.exe"))
TRIPLE = "powerpc64-unknown-xbox360"

# NOTE: the picolibc C-header include must come AFTER the C++ header dirs, or
# libc++'s <cstdlib> grabs picolibc's <stdlib.h> instead of libc++'s wrapper.
# So COMMON carries no include paths; each flag set orders its own.
COMMON = [
    "--target=" + TRIPLE, "-O2", "-ffreestanding",
    # 2-byte wchar_t to match the platform's 16-bit WCHAR (see build_libc.py):
    # std::wstring / char_traits<wchar_t> and the XDK libraries must agree.
    "-fshort-wchar",
    "-fno-stack-protector", "-fno-sanitize=all", "-fno-builtin", "-Wno-everything",
    "-D__Picolibc__", "-include", "picolibc.h",
    "-I" + CONFIG,
]
PICO_INC = ["-I" + os.path.join(PICO, "libc", "include")]

# libunwind: the DWARF unwinder. Baremetal DWARF branch (.eh_frame from its markers).
UNWIND_DIR = os.path.join(LLVM, "libunwind")
UNWIND_SRCS = [
    "src/libunwind.cpp", "src/UnwindLevel1.c", "src/UnwindLevel1-gcc-ext.c",
    "src/UnwindRegistersSave.S", "src/UnwindRegistersRestore.S",
]
UNWIND_FLAGS = COMMON + [
    "-D_LIBUNWIND_IS_BAREMETAL=1", "-D_LIBUNWIND_HAS_NO_THREADS=1", "-D_LIBUNWIND_XBOX360=1", "-D_LIBUNWIND_XBOX360_NO_EH_FRAME_HDR=1", "-DNDEBUG",
    "-funwind-tables",
    "-I" + os.path.join(UNWIND_DIR, "include"),
    "-I" + os.path.join(UNWIND_DIR, "src"),
] + PICO_INC
UNWIND_CXX = ["-std=c++23", "-fno-exceptions", "-fno-rtti"]

# libc++abi: the exception ABI core (type_info matching needs -frtti; all -fexceptions).
ABI_DIR = os.path.join(LLVM, "libcxxabi")
ABI_SRCS = [
    "cxa_exception.cpp", "cxa_personality.cpp", "cxa_exception_storage.cpp",
    "cxa_handlers.cpp", "cxa_default_handlers.cpp", "cxa_aux_runtime.cpp",
    # cxa_virtual (__cxa_pure_virtual/__cxa_deleted_virtual) and cxa_guard,
    # operator new/delete (stdlib_new_delete) are all provided by cxxrt.cpp in
    # libc.a, which every title links -- so they are omitted here to avoid
    # duplicate-symbol clashes.
    "private_typeinfo.cpp", "fallback_malloc.cpp",
    "stdlib_exception.cpp", "stdlib_stdexcept.cpp", "stdlib_typeinfo.cpp",
    "abort_message.cpp",
]
ABI_FLAGS = COMMON + [
    "-std=c++23", "-fexceptions", "-frtti",
    # libc++'s localization support uses the POSIX/GNU extended-locale API
    # (locale_t, uselocale/newlocale, the *_l ctype functions, LC_*_MASK), which
    # picolibc gates behind POSIX/GNU visibility -- off under strict -std=c++23.
    # Ask for it here so <locale>/<iostream> build (also exposes clock_gettime,
    # strerror_r ... but we still force-include the prereq for belt-and-braces).
    "-D_GNU_SOURCE",
    # libc++'s picolibc ctype_base.h (the _LIBCPP_LIBC_NEWLIB path) initializes
    # its ctype masks from the single-underscore newlib macros _S/_P/_U/_L/...
    # picolibc reserves those identifiers and only exposes them for C++ on i386
    # or behind this opt-in (see vendor/picolibc/libc/include/ctype.h) so title
    # code pulling the XDK headers (which use _X/_Y as parameter names) is not
    # polluted. On PowerPC the libc++ build must ask for them explicitly.
    "-D_PICOLIBC_LEGACY_CTYPE_MACROS",
    # (The old -D_SC_NPROCESSORS_ONLN=200 override is gone: the consolidated
    # picolibc 'xbox' branch defines _SC_NPROCESSORS_ONLN itself, and
    # runtime/xbox/sysconf.c now answers on picolibc's own value.)
    "-D_LIBCPP_BUILDING_LIBRARY", "-DLIBCXX_BUILDING_LIBCXXABI",
    "-DLIBCXXABI_BUILDING_LIBCXXABI", "-D_LIBCXXABI_XBOX360_TLS_KEY=1",
    "-include", "__config_site", "-include", "rxdk_libcpp_prereq.h",
    "-I" + os.path.join(LLVM, "libcxx", "include"),
    "-I" + os.path.join(LLVM, "libcxx", "src"),
    "-I" + os.path.join(ABI_DIR, "include"),
    "-I" + os.path.join(ABI_DIR, "src"),
] + PICO_INC


# libc++ threading + support: std::thread / mutex / condition_variable and the
# std::system_error they throw. Same flag set as libc++abi. (chrono/steady_clock
# is deferred -- it needs a monotonic clock_gettime; timed waits use cnd_timedwait
# directly, so untimed std::thread/mutex/condition_variable do not need it.)
LIBCXX_DIR = os.path.join(LLVM, "libcxx")
LIBCXX_SRCS = [
    "thread.cpp", "mutex.cpp", "mutex_destructor.cpp",
    "condition_variable.cpp", "condition_variable_destructor.cpp",
    "shared_mutex.cpp", "system_error.cpp", "verbose_abort.cpp",
    "future.cpp",   # __assoc_sub_state, pulled by thread.cpp's __thread_struct
    "atomic.cpp",   # atomic wait/notify (<latch>/<semaphore>/jthread, atomic::wait);
                    # no futex on 360, so its generic path rides on our cv/mutex
    "barrier.cpp",  # std::barrier algorithm base (out-of-line)
    "print.cpp",    # std::print/println (vprint_unicode/nonunicode -> stdout)
    # The out-of-line STL bits <system_error>/<future>/<thread> drag in:
    "stdexcept.cpp",   # logic_error/runtime_error ctors + their typeinfo/vtables
    "string.cpp",      # basic_string out-of-line members (what the errors carry)
    "memory.cpp",      # __shared_count::~__shared_count (shared_ptr refcount)
    "exception.cpp",   # std::exception_ptr / rethrow_exception glue over libc++abi
    "error_category.cpp",  # base error_category virtuals + its typeinfo
    "functional.cpp",  # __hash_memory (std::hash for the error machinery)
    "new_helpers.cpp", # __throw_bad_alloc + new-handler helpers (not operator new)
    "call_once.cpp",   # std::__call_once (std::call_once, used by <locale>)
    "chrono.cpp",      # system_clock/steady_clock::now (over our clock_gettime)
    "hash.cpp",        # __next_prime -- unordered_map/set bucket sizing
    "algorithm.cpp",   # explicit __sort/__stable_sort instantiations std::sort uses
    # <iostream>: the stream objects + locale/facet machinery.
    "iostream.cpp",    # std::cout/cerr/cin/clog + ios_base::Init
    "ios.cpp",         # ios_base
    "ios.instantiations.cpp",  # basic_ios/basic_ostream/... explicit instantiations
    "locale.cpp",      # locale + facets (num_put/num_get, ctype, ...)
    # <filesystem> -- also what <fstream> is gated behind in this libc++.
    "filesystem/operations.cpp",
    "filesystem/directory_iterator.cpp",
    "filesystem/path.cpp",
    "filesystem/filesystem_error.cpp",
    "filesystem/directory_entry.cpp",
    "filesystem/filesystem_clock.cpp",
    # vocabulary-type out-of-line bits (bad_*_access / bad_any_cast vtables+what),
    # <charconv> (to_chars, used by <format>), and <regex>.
    "any.cpp",
    "optional.cpp",
    "variant.cpp",
    "regex.cpp",
    "charconv.cpp",    # to_chars/from_chars -- what <format> and float parsing need
    "ryu/d2s.cpp",     # Ryu double/float -> shortest string, backs to_chars<float>
    "ryu/f2s.cpp",
    "ryu/d2fixed.cpp",
]

# Per-file extra compile flags (appended after ABI_FLAGS). charconv.cpp's float
# from_chars pulls llvm-libc's shared FPBits.h: it needs the llvm-libc include
# root (shared/fp_bits.h) and, since that shared code routes LIBC_ASSERT through
# _LIBCPP_ASSERT -> _LIBCPP_VERBOSE_ABORT, the <__verbose_abort> header that
# defines that macro (from_chars_floating_point.h includes <__assert> but not it).
LIBCXX_EXTRA = {
    "charconv.cpp": ["-I" + os.path.join(LLVM, "libc"), "-include", "__verbose_abort"],
}


def compile_one(src, flags, objdir, tag):
    obj = os.path.join(objdir, tag + "_" + os.path.splitext(os.path.basename(src))[0] + ".o")
    cmd = [CLANG] + flags + ["-c", src, "-o", obj]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        errs = [l for l in r.stderr.splitlines() if "error:" in l]
        return None, (src, errs[:3] or r.stderr.strip().splitlines()[-1:] or [""])
    return obj, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", "--out", default=os.path.join(ROOT, "build", "libc", "libcpp.a"))
    ap.add_argument("--debug", action="store_true",
                    help="build the _DEBUG variant (define _DEBUG, -O0 -g, drop NDEBUG "
                         "so libc++ assertions are active) -- for the libcpMTd variant")
    args = ap.parse_args()
    if args.debug:
        global UNWIND_FLAGS, ABI_FLAGS
        def dbg(fl):
            return (["-O0" if f == "-O2" else f for f in fl if f != "-DNDEBUG"]
                    + ["-g", "-D_DEBUG"])
        UNWIND_FLAGS = dbg(UNWIND_FLAGS)
        ABI_FLAGS = dbg(ABI_FLAGS)
    if not os.path.exists(CLANG):
        sys.exit("patched clang not found: %s" % CLANG)

    objdir = os.path.join(os.path.dirname(args.out), "obj_cpp")
    os.makedirs(objdir, exist_ok=True)
    objs, failed = [], []

    for s in UNWIND_SRCS:
        src = os.path.join(UNWIND_DIR, s)
        flags = UNWIND_FLAGS + (UNWIND_CXX if src.endswith(".cpp") else [])
        obj, err = compile_one(src, flags, objdir, "uw")
        (objs if obj else failed).append(obj or err)

    for s in ABI_SRCS:
        obj, err = compile_one(os.path.join(ABI_DIR, "src", s), ABI_FLAGS, objdir, "abi")
        (objs if obj else failed).append(obj or err)

    for s in LIBCXX_SRCS:
        flags = ABI_FLAGS + LIBCXX_EXTRA.get(s, [])
        obj, err = compile_one(os.path.join(LIBCXX_DIR, "src", s), flags, objdir, "cxx")
        (objs if obj else failed).append(obj or err)

    if failed:
        print("%d source(s) failed:" % len(failed))
        for src, why in failed:
            print("  %s:" % os.path.relpath(src, ROOT))
            for line in (why or []):
                print("      " + line)
        sys.exit(1)

    if os.path.exists(args.out):
        os.remove(args.out)
    r = subprocess.run([AR, "rcs", args.out] + objs, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("archive failed:\n" + r.stderr)
    print("wrote %s: %d objects" % (os.path.relpath(args.out, ROOT), len(objs)))


if __name__ == "__main__":
    main()
