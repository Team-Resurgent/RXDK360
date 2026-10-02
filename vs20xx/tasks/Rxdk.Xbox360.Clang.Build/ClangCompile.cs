// SPDX-License-Identifier: GPL-3.0-or-later
// Part of RXDK-360 - see LICENSE.md for the full GNU GPL v3.

using System;
using System.Collections.Generic;
using System.IO;
using Microsoft.Build.Framework;
using Microsoft.Build.Utilities;

namespace Rxdk.Xbox360.Clang.Build
{
    /// <summary>
    /// Compile C/C++/asm sources with the patched MS-PPC clang, one object per
    /// source. Mirrors mktitle.py's compile_sources + auto_clang_flags: C sources
    /// get the picolibc environment, C++ sources additionally get the exception/
    /// RTTI flags and the libc++/libc++abi headers (before picolibc so &lt;cstdlib&gt;
    /// etc. resolve to libc++'s wrappers). Prebuilt .o sources pass through.
    /// </summary>
    public sealed class ClangCompile : ClangTool
    {
        [Required] public string ClangPath { get; set; }

        /// <summary>Optional fallback root that holds the dev-tree runtime/, vendor/;
        /// used only to derive the modern include dirs below when they are not given
        /// explicitly (so a clean installed layout supplies its own dirs).</summary>
        public string RxdkRoot { get; set; }

        // The modern C/C++23 runtime include set, supplied explicitly by the toolset
        // so it works for any install layout. When empty they fall back to the dev
        // tree under RxdkRoot.
        public string PicolibcIncludeDir { get; set; }
        public string ConfigDir { get; set; }
        public string LibcxxIncludeDir { get; set; }
        public string LibcxxabiIncludeDir { get; set; }

        /// <summary>Where the .o files are written (the project IntDir).</summary>
        [Required] public string OutputDir { get; set; }

        [Required] public ITaskItem[] Sources { get; set; }

        public string MsTriple { get; set; } = "powerpc64-unknown-xbox360";
        public string Optimization { get; set; } = "-O2";
        /// <summary>Link-time optimisation (Release_LTCG): compile to LLVM bitcode with
        /// -flto so ld.lld optimises across the whole program at link time.</summary>
        public bool Lto { get; set; }
        /// <summary>Emit DWARF debug info (Debug configs), for source-level debugging.</summary>
        public bool DebugInformation { get; set; }

        /// <summary>Compile against the stock XDK headers (D3D9 / XGraphics / xtl.h)
        /// instead of the modern picolibc/libc++ environment - the MS-compat recipe
        /// that lets clang parse the Win32/MSVC-style XDK headers. The title still
        /// links the clang runtime; only the header set differs.</summary>
        public bool XdkHeaders { get; set; }

        /// <summary>Include dirs added in XDK-headers mode (the XDK's include\xbox).</summary>
        public string[] XdkIncludeDirectories { get; set; }
        public string LanguageStandardC { get; set; } = "c23";
        public string LanguageStandardCpp { get; set; } = "c++23";
        public string[] AdditionalIncludeDirectories { get; set; }
        public string[] PreprocessorDefinitions { get; set; }
        public string AdditionalOptions { get; set; }

        /// <summary>The compiled objects (fed to ClangLink).</summary>
        [Output] public ITaskItem[] ObjectFiles { get; set; }

        public override bool Execute()
        {
            Directory.CreateDirectory(OutputDir);
            var objects = new List<ITaskItem>();
            bool ok = true;

            foreach (var item in Sources)
            {
                string src = item.GetMetadata("FullPath");
                string ext = Path.GetExtension(src).ToLowerInvariant();
                if (ext == ".o" || ext == ".obj")
                {
                    objects.Add(new TaskItem(src));
                    continue;
                }

                string obj = Path.Combine(OutputDir, ObjectName(src));
                objects.Add(new TaskItem(obj));

                if (UpToDate(src, obj))
                {
                    Log.LogMessage(MessageImportance.Low, "  up to date: " + Path.GetFileName(src));
                    continue;
                }

                Log.LogMessage(MessageImportance.High, "  " + Path.GetFileName(src));
                bool isAsm = ext == ".s" || ext == ".asm";
                bool isCpp = ext == ".cpp" || ext == ".cc" || ext == ".cxx" || ext == ".c++";

                var args = new List<string> { "--target=" + MsTriple, "-c", src, "-o", obj };
                if (!isAsm)
                {
                    // Insert compile flags before the -c (matching mktitle's ordering).
                    var pre = new List<string> { Optimization, "-std=" + (isCpp ? LanguageStandardCpp : LanguageStandardC) };
                    if (DebugInformation) pre.Add("-gdwarf-4");
                    if (Lto) pre.Add("-flto");
                    pre.AddRange(XdkHeaders ? XdkHeaderFlags(isCpp) : AutoClangFlags(isCpp));
                    if (AdditionalIncludeDirectories != null)
                        foreach (var inc in AdditionalIncludeDirectories)
                            if (!string.IsNullOrWhiteSpace(inc)) pre.Add("-I" + inc.Trim());
                    if (PreprocessorDefinitions != null)
                        foreach (var d in PreprocessorDefinitions)
                            if (!string.IsNullOrWhiteSpace(d)) pre.Add("-D" + d.Trim());
                    if (!string.IsNullOrWhiteSpace(AdditionalOptions))
                        foreach (var opt in SplitOptions(AdditionalOptions))
                        {
                            // Drop MSVC-style (/...) options: this is a gcc-style clang
                            // toolset, and the stock platform props inject cl.exe defaults
                            // (e.g. the /FI IntelliSense forced-include) that clang rejects.
                            if (opt.StartsWith("/"))
                                Log.LogMessage(MessageImportance.Low, "  (ignoring MSVC-style option " + opt + ")");
                            else
                                pre.Add(opt);
                        }
                    args.InsertRange(1, pre);   // after "--target=..."
                }

                var r = Run(ClangPath, args);
                LogDiagnostics(r.Combined);
                if (r.ExitCode != 0)
                {
                    Log.LogError("clang: compile failed for {0}", Path.GetFileName(src));
                    ok = false;
                }
            }

            ObjectFiles = objects.ToArray();
            return ok && !Log.HasLoggedErrors;
        }

        /// <summary>
        /// The standard modern-runtime compile environment (see mktitle.auto_clang_flags).
        /// Placed before the user's options so those still win on any conflict.
        /// </summary>
        private IEnumerable<string> AutoClangFlags(bool isCpp)
        {
            string cfg = Or(ConfigDir, Path.Combine(RxdkRoot ?? "", "runtime", "config"));
            string pico = Or(PicolibcIncludeDir, Path.Combine(RxdkRoot ?? "", "vendor", "picolibc", "libc", "include"));
            // Xbox 360 WCHAR is 16-bit UTF-16 (the MSVC/XDK ABI). picolibc, libc++
            // and the stock XDK import libraries are all built with a 2-byte
            // wchar_t, so title code MUST match: without -fshort-wchar clang
            // defaults to a 4-byte wchar_t, and every L"..." / wchar_t value a
            // title hands to the runtime or an XDK lib is misread (wide printf,
            // OutputDebugStringW, wcslen, ... silently see a truncated/empty
            // string). Applies to both the modern and XDK-headers compiles.
            // -ffunction-sections/-fdata-sections put every function and datum in
            // its own section so the link's --gc-sections can drop what the title
            // does not use. Load-bearing for the ATG Common static library: it is
            // linked as one relocatable object (the toolset ships no llvm-ar), so
            // without per-function sections the whole framework - and every XDK
            // API it references (XHttp/XSession/XUI/NUI/XAPO/FXL/...) - is pulled
            // in even by a sample that only uses ATG::Console.
            if (!isCpp)
                return new[] { "-fshort-wchar", "-ffunction-sections", "-fdata-sections",
                               "-D__Picolibc__", "-D_GNU_SOURCE", "-I" + cfg, "-I" + pico,
                               "-include", "picolibc.h" };
            string lx = Or(LibcxxIncludeDir, Path.Combine(RxdkRoot ?? "", "vendor", "llvm-project", "libcxx", "include"));
            string la = Or(LibcxxabiIncludeDir, Path.Combine(RxdkRoot ?? "", "vendor", "llvm-project", "libcxxabi", "include"));
            return new[] { "-fshort-wchar", "-ffunction-sections", "-fdata-sections",
                           "-fexceptions", "-funwind-tables", "-frtti",
                           "-D__Picolibc__", "-D_GNU_SOURCE",
                           "-I" + lx, "-I" + la, "-I" + cfg,
                           "-include", "__config_site", "-include", "rxdk_libcpp_prereq.h",
                           "-I" + pico, "-include", "picolibc.h" };
        }

        private static string Or(string a, string b) => string.IsNullOrWhiteSpace(a) ? b : a.Trim();

        // The MS-compatibility recipe that lets clang parse the stock XDK headers
        // (see tests/gfx/build_tri.py / docs/sdk-headers-plan.md): satisfy the
        // Win32/Xbox gates and MSVC extensions, and put xnamath/xboxmath in scalar
        // mode so d3dx9math.h / xgraphics.h compile without VMX128 intrinsics. The
        // XDK include\xbox dirs are added last.
        private IEnumerable<string> XdkHeaderFlags(bool isCpp)
        {
            // Always-modern: build the stock XDK headers ON TOP of the modern
            // picolibc + libc++ runtime (one config, no separate mode). Start with
            // the modern base (libc++/picolibc includes + force-includes), then add
            // the MS-compat recipe + the reconciliations that let the XDK headers
            // sit on that runtime (see runtime/config/rxdk_*_compat + vadefs.h and
            // the picolibc ctype.h/cdefs.h gates).
            var f = new List<string>(AutoClangFlags(isCpp));
            f.AddRange(new[]
            {
                "-fms-extensions", "-fms-compatibility", "-fdeclspec",
                // The XDK headers are written for cl.exe v16.00; ms-compat-version
                // 1920 keeps char16_t a keyword (libc++ needs it) while enabling the
                // MSVC lookup the XDK/ATG templates rely on.
                "-fms-compatibility-version=1920",
                // Reconcile the standard types/functions to the clang runtime so
                // the XDK CRT headers don't redefine picolibc's (size_t/intptr_t),
                // give picolibc's stdio its __gnuc_va_list, keep char16_t native,
                // and expose picolibc's Annex K secure-CRT (__STDC_WANT_LIB_EXT1__).
                "-D_INTPTR_T_DEFINED", "-D_UINTPTR_T_DEFINED",
                "-D__gnuc_va_list=__builtin_va_list", "-D_HAS_CHAR16_T_LANGUAGE_SUPPORT=1",
                "-D__STDC_WANT_LIB_EXT1__=1",
                // XObjBase.h gates DECLSPEC_UUID -> __declspec(uuid(x)) on
                // _MSC_VER>=1100, so without _MSC_VER the COM interfaces get no GUID
                // and __uuidof(IUnknown) fails. Define _MSC_VER globally would make
                // the force-included libc++/picolibc take their MSVC paths and break;
                // instead attach the uuid surgically by pre-defining DECLSPEC_UUID.
                "-DDECLSPEC_UUID(x)=__declspec(uuid(x))",
                // ppcintrinsics.h (and a few others) gate whole intrinsic families
                // -- e.g. the doubleword byte-reverse loads/stores FastUntile uses --
                // behind `#if _MSC_FULL_VER >= 14002303` (VS2005 SP1). clang defines
                // no _MSC_FULL_VER for this non-MSVC triple, so those decls vanish and
                // the intrinsic is "undeclared". Define it to the authentic XDK cl
                // build (14.00.50727, the build-11886 compiler these headers ship for),
                // which clears every XDK gate (all <= 14006016). Safe unlike a global
                // _MSC_VER: nothing in libc++/picolibc/config tests _MSC_FULL_VER, so
                // the clang runtime headers are unaffected.
                "-D_MSC_FULL_VER=140050727",
                // The stock secure CRT (stdio.h) declares vsprintf_s/vswprintf_s
                // AFTER the inline templates that call them; MSVC's late template
                // parsing resolves that, two-phase lookup does not. Match MSVC.
                "-fdelayed-template-parsing",
                // ATG's template loop-unroller pastes 'name##[CurrentIndex()]' to
                // form 'arg0[...]'; MSVC is lenient about pastes that don't make a
                // single token, standard clang errors. Match MSVC's leniency.
                "-Wno-invalid-token-paste",
                // XDK/ATG code brace-initializes signed fields with 0xFFFFFFFF etc.;
                // MSVC allows it, C++11 makes narrowing in braced-init an error.
                "-Wno-narrowing",
                // Warnings inherent to compiling the stock XDK/ATG sample framework
                // (ATG Common and the samples) with clang -- benign under MSVC, pure
                // noise here. Suppressed centrally so a real diagnostic on new title
                // code stays visible:
                //  - switch: stock enums omit ForceDWORD/sentinel cases by design.
                //  - ignored-attributes: __declspec(passinreg)/__stdcall/__cdecl have
                //    no effect on the PPC ABI; the calls are still correct.
                //  - nonportable-include-path: stock #includes differ in case / have a
                //    trailing dot; harmless on Windows' case-insensitive FS.
                //  - microsoft-extra-qualification: ATG headers write Foo::Foo::member.
                //  - ignored-pragma-intrinsic: #pragma intrinsic(_WriteBarrier) etc.
                //  - nontrivial-memcall: ATG memcpy/memset POD-ish vertex structs.
                //  - delete-incomplete: ATG deletes through void*/LPCVOID (frees).
                //  - tautological compares: ATG's -1 vs unsigned-enum range checks.
                "-Wno-switch",
                "-Wno-ignored-attributes",
                "-Wno-nonportable-include-path",
                "-Wno-microsoft-extra-qualification",
                "-Wno-ignored-pragma-intrinsic",
                "-Wno-nontrivial-memcall",
                "-Wno-delete-incomplete",
                "-Wno-tautological-constant-out-of-range-compare",
                // XDK/ATG code assigns string literals to non-const CHAR*/WCHAR*
                // (ATG HELP_CALLOUT tables, AddRowElement(L"..."), ...). MSVC keeps
                // literals non-const (/Zc:strictStrings-); C++11 makes the implicit
                // const->non-const a hard error. -fwritable-strings gives literals a
                // non-const type, matching MSVC, so those assignments compile.
                "-fwritable-strings",
                // #pragma comment(lib, "d3d9.lib") in XDK code emits a COFF auto-link
                // directive ld.lld cannot resolve; the modern link names the ELF
                // libraries explicitly instead, so drop the directive.
                "-fno-autolink",
                "-D_WIN32=1", "-D_M_PPCBE=1", "-D_M_PPC=1", "-D_XBOX=1", "-D_XBOX_VER=200",
                "-D__export=", "-D_SIZE_T_DEFINED", "-D_XM_NO_INTRINSICS_",
            });
            // Reconciliation force-includes, AFTER the clang runtime's picolibc.h:
            //  - stdint.h so picolibc's intptr_t/uintptr_t are in scope (we suppress
            //    the XDK's above), __stddef_max_align_t.h because the XDK stddef.h
            //    lacks max_align_t (libc++ <memory_resource> needs it), and the two
            //    RXDK compat headers (MS CRT extensions + secure-CRT overloads). These
            //    resolve on the config include dir (added by AutoClangFlags).
            f.AddRange(new[]
            {
                "-include", "stdint.h", "-include", "__stddef_max_align_t.h",
                "-include", "rxdk_msvcrt_compat.h", "-include", "rxdk_secure_overloads.h",
            });
            // Add the XDK headers as SYSTEM includes (-isystem) so clang suppresses
            // the many warnings from the stock MS headers themselves (ignored
            // __stdcall, case-mismatched #includes, #endif tokens, ...) while still
            // warning on the title's own code.
            if (XdkIncludeDirectories != null)
                foreach (var d in XdkIncludeDirectories)
                    if (!string.IsNullOrWhiteSpace(d)) { f.Add("-isystem"); f.Add(d.Trim()); }
            return f;
        }

        // Qualify the object with its parent directory: many titles carry several
        // sources all named main.c (one per subdir), which would otherwise collide.
        private static string ObjectName(string src)
        {
            string stem = Path.GetFileNameWithoutExtension(src);
            string parent = Path.GetFileName(Path.GetDirectoryName(Path.GetFullPath(src)));
            return (string.IsNullOrEmpty(parent) ? stem : parent + "_" + stem) + ".o";
        }

        private static bool UpToDate(string src, string obj)
        {
            return File.Exists(obj) && File.GetLastWriteTimeUtc(obj) >= File.GetLastWriteTimeUtc(src);
        }

        private static IEnumerable<string> SplitOptions(string s)
        {
            // Split on whitespace outside quotes.
            var list = new List<string>();
            var cur = new System.Text.StringBuilder();
            bool q = false;
            foreach (char c in s)
            {
                if (c == '"') q = !q;
                else if (char.IsWhiteSpace(c) && !q)
                {
                    if (cur.Length > 0) { list.Add(cur.ToString()); cur.Clear(); }
                }
                else cur.Append(c);
            }
            if (cur.Length > 0) list.Add(cur.ToString());
            return list;
        }
    }
}
