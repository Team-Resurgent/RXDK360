// SPDX-License-Identifier: GPL-3.0-or-later
// Part of RXDK-360 - see LICENSE.md for the full GNU GPL v3.
//
// RXDK-360 XDK unpacker. See RxdkXdkUnpacker.csproj for the format notes: an
// Xbox 360 XDK setup EXE is a PE stub followed by a chain of concatenated
// standard MS cabinets. This walks the chain and extracts each cabinet in
// process via the Windows FDI API (cabinet.dll).

using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;

namespace Rxdk.Xdk.Unpacker
{
    internal static class Program
    {
        private const string UndoLog = "rxdk360-uninstall.log";

        [DllImport("kernel32.dll")]
        private static extern bool AttachConsole(int dwProcessId);

        private static int Main(string[] args)
        {
            AttachConsole(-1);
            int rc = 1;
            try { rc = Run(args); }
            finally
            {
                // completion marker so a host installer polling --progress knows we
                // finished (and with what exit code), even on failure.
                if (Report.File != null)
                    try { System.IO.File.AppendAllText(Report.File, "###DONE " + rc + "###\r\n"); } catch { }
            }
            return rc;
        }

        // Finish the mirrored install tree IN PLACE. The install mirrors the real XDK
        // (no modern\/legacy\ split): the XDK headers ({app}\include\xbox) and import
        // .libs ({app}\lib\xbox\*.lib) are a single copy at the product root, placed by
        // the manifest engine. We patch the headers and translate the libs where they
        // are, and generate our archives (kernel_import.a, the coff2elf ELF .a,
        // libcompat.a) next to them in {app}\lib\xbox. clang + llvm-ar live in the
        // compiler bundle at {app}\bin\clang. Args: appRoot ({app}), clangRoot
        // ({app}\bin\clang).
        private static void StageClang(string appRoot, string clangRoot)
        {
            var undo = new List<string>();
            string dstInc = Path.Combine(appRoot, "include", "xbox");
            if (Directory.Exists(dstInc))
            {
                // The XDK headers are the single product-root copy; patch them in place
                // (idempotent) rather than staging a duplicate under the compiler tree.
                PatchXdkHeaders(dstInc);
                Report.Line("patched XDK headers in place -> {0}", dstInc);
            }

            // Point libc++'s newlib ctype_base at picolibc's non-colliding __CTYPE_*
            // names so the 360 never needs the bare _U.._X legacy macros (which clash
            // with XDK identifiers like float.h's _chgsign(double _X)). Values are
            // identical, so this is ABI-neutral -- no library rebuild required.
            if (PatchLibcxxCtype(clangRoot))
                Report.Line("patched libc++ ctype_base -> __CTYPE_* (no legacy ctype macros)");
            string dstLib = Path.Combine(appRoot, "lib", "xbox");
            if (Directory.Exists(dstLib))
            {
                // Build the global kernel import library from the in-place XDK libs, so
                // every title links one kernel_import.a (like an SDK's xboxkrnl.lib)
                // instead of discovering imports per-title. clang + llvm-ar come from
                // the compiler bundle; the console ordinals come from the XDK libs.
                string clang = Path.Combine(clangRoot, "bin", "clang.exe");
                string ar = Path.Combine(clangRoot, "bin", "llvm-ar.exe");
                string kimp = Path.Combine(dstLib, "kernel_import.a");
                if (File.Exists(clang) && File.Exists(ar))
                {
                    try
                    {
                        int ni = KernelImportLib.Generate(dstLib, clang, ar, "powerpc-unknown-xbox360", kimp);
                        undo.Add("file|" + kimp);
                        Report.Line("built kernel_import.a ({0} imports) -> {1}", ni, kimp);
                    }
                    catch (Exception ex) { Report.Line("WARNING: kernel_import.a not built: {0}", ex.Message); }
                }
                else
                    Report.Line("WARNING: clang/llvm-ar missing under {0}\\bin; kernel_import.a not built", clangRoot);

                // Translate every XDK static .lib (big-endian PPC COFF, which lld cannot
                // read) into a PPC32 ELF .a lld links natively (Coff2Elf, the byte-for-
                // byte port of tools/coff2elf.py). These are XDK-derived (not
                // redistributable), so they are generated here from the user's own libs
                // rather than shipped -- the .a land beside the .lib in {app}\lib\xbox.
                // The runtime archives (libcpp.a/libc.a, laid down by the installer in
                // the same dir) form the external-strong set so a COMDAT signature the
                // runtime owns strong stays strong.
                var runtimes = new List<string>();
                foreach (var rl in new[] { "libcpp.a", "libc.a" })
                {
                    var p = Path.Combine(dstLib, rl);
                    if (File.Exists(p)) runtimes.Add(p);
                }
                int nlibs = 0;
                foreach (var f in Directory.GetFiles(dstLib, "*.lib"))
                {
                    string outA = Path.Combine(dstLib, Path.GetFileNameWithoutExtension(f) + ".a");
                    try
                    {
                        Coff2Elf.Translate(f, outA, runtimes.ToArray());
                        undo.Add("file|" + outA);
                        var man = Path.ChangeExtension(outA, ".imports.json");
                        if (File.Exists(man)) undo.Add("file|" + man);
                        nlibs++;
                    }
                    catch (Exception ex) { Report.Line("WARNING: coff2elf {0}: {1}", Path.GetFileName(f), ex.Message); }
                }
                Report.Line("translated {0} XDK libs -> ELF .a", nlibs);

                // Build the compat library (our clang reimplementations of XDK C++
                // helper classes titles subclass -- XAPOBase, AsfWriterPropertyValue)
                // HERE rather than shipping it prebuilt: its sources #include the XDK's
                // own headers, so it can only be compiled where the XDK exists -- the
                // install machine, not CI. Same self-contained rule as kernel_import.a
                // and the coff translations. Output goes to {app}\lib\xbox.
                if (File.Exists(clang) && File.Exists(ar))
                {
                    try
                    {
                        int nc = BuildCompatLib(clangRoot, dstInc, dstLib, clang, ar);
                        if (nc > 0) { undo.Add("file|" + Path.Combine(dstLib, "libcompat.a")); Report.Line("built libcompat.a ({0} sources) -> {1}", nc, dstLib); }
                    }
                    catch (Exception ex) { Report.Line("WARNING: libcompat.a not built: {0}", ex.Message); }
                }
            }
            // {app}\rxdk360-uninstall.log sits at the product root.
            string undoLog = Path.Combine(appRoot, UndoLog);
            if (undo.Count > 0 && File.Exists(undoLog))
                File.AppendAllLines(undoLog, undo);
        }

        // Compile the compat sources (bin\clang\compat\*.cpp) into {app}\lib\xbox\
        // libcompat.a with the shipped clang, in XDK-headers mode against the XDK
        // headers at the product root (mirrors tools/build_libcompat.py's flag recipe).
        // Returns the source count, or 0 when there are no compat sources to build.
        // clangRoot = {app}\bin\clang; xdkInc = {app}\include\xbox; dstLib = {app}\lib\xbox.
        private static int BuildCompatLib(string clangRoot, string xdkInc, string dstLib, string clang, string ar)
        {
            string srcDir = Path.Combine(clangRoot, "compat");
            if (!Directory.Exists(srcDir)) return 0;
            var sources = Directory.GetFiles(srcDir, "*.cpp");
            if (sources.Length == 0) return 0;

            string inc = Path.Combine(clangRoot, "include");
            string cfg = Path.Combine(inc, "config");
            string pico = Path.Combine(inc, "picolibc");
            string lx = Path.Combine(inc, "libcxx");
            string la = Path.Combine(inc, "libcxxabi");
            string xdk = xdkInc;   // {app}\include\xbox -- the single product-root copy
            // AutoClangFlags(C++) + the XDK-headers MS-compat recipe (ClangCompile's
            // XdkHeaderFlags / build_libcompat.py). -frtti so the base classes' _ZTI
            // typeinfo the subclass needs is emitted.
            var flags = new List<string> {
                "--target=powerpc-unknown-xbox360", "-O2",
                "-fshort-wchar", "-ffunction-sections", "-fdata-sections",
                "-fexceptions", "-funwind-tables", "-frtti",
                "-D__Picolibc__", "-D_GNU_SOURCE",
                // The compat sources include XDK headers but no libc++ ctype facets,
                // and their XAPOBase.h chain pulls <ctype.h> before float.h. Opt out of
                // picolibc's legacy single-letter ctype macros so _X (etc.) doesn't
                // collide with float.h's `_chgsign(double _X)` parameter.
                "-D_RXDK_NO_LEGACY_CTYPE",
                "-I" + lx, "-I" + la, "-I" + cfg,
                "-include", "__config_site", "-include", "rxdk_libcpp_prereq.h",
                "-I" + pico, "-include", "picolibc.h",
                "-fms-extensions", "-fms-compatibility", "-fdeclspec", "-fms-compatibility-version=1920",
                "-D_INTPTR_T_DEFINED", "-D_UINTPTR_T_DEFINED",
                "-D__gnuc_va_list=__builtin_va_list", "-D_HAS_CHAR16_T_LANGUAGE_SUPPORT=1",
                "-D__STDC_WANT_LIB_EXT1__=1", "-DDECLSPEC_UUID(x)=__declspec(uuid(x))",
                "-D_MSC_FULL_VER=140050727", "-fdelayed-template-parsing",
                "-Wno-invalid-token-paste", "-Wno-narrowing", "-fwritable-strings", "-fno-autolink",
                "-D_WIN32=1", "-D_M_PPCBE=1", "-D_M_PPC=1", "-D_XBOX=1", "-D_XBOX_VER=200",
                "-D__export=", "-D_SIZE_T_DEFINED", "-D_XM_NO_INTRINSICS_", "-Wno-everything",
                "-include", "stdint.h", "-include", "__stddef_max_align_t.h",
                "-include", "rxdk_msvcrt_compat.h", "-include", "rxdk_secure_overloads.h",
                "-isystem", xdk,
            };
            var objs = new List<string>();
            foreach (var src in sources)
            {
                string obj = Path.Combine(dstLib, "compat_" + Path.GetFileNameWithoutExtension(src) + ".o");
                var a = new List<string>(flags) { "-c", src, "-o", obj };
                RunOrThrow(clang, a, "compat compile " + Path.GetFileName(src));
                objs.Add(obj);
            }
            string outA = Path.Combine(dstLib, "libcompat.a");
            if (File.Exists(outA)) File.Delete(outA);
            var arArgs = new List<string> { "rcs", outA };
            arArgs.AddRange(objs);
            RunOrThrow(ar, arArgs, "compat archive");
            foreach (var o in objs) { try { File.Delete(o); } catch { } }
            return sources.Length;
        }

        // Reassemble the split sample-asset parts (<samplesRoot>\assets\
        // samples-assets.zip.part.NNN) into one zip and extract every entry into
        // <samplesRoot> in place. The C# twin of samples\tools\Manage-Assets.ps1
        // unpack, so the installer needs no PowerShell. Returns the file count.
        private static int UnpackSampleAssets(string samplesRoot)
        {
            string assetsDir = Path.Combine(samplesRoot, "assets");
            if (!Directory.Exists(assetsDir)) return 0;
            var parts = Directory.GetFiles(assetsDir, "samples-assets.zip.part.*");
            if (parts.Length == 0) return 0;
            Array.Sort(parts, StringComparer.Ordinal);   // .part.000, .001, ... in order

            string tmp = Path.Combine(Path.GetTempPath(), "rxdk_assets_" + Guid.NewGuid().ToString("N") + ".zip");
            int n = 0;
            try
            {
                using (var outFs = new FileStream(tmp, FileMode.Create, FileAccess.Write))
                    foreach (var p in parts)
                        using (var inFs = new FileStream(p, FileMode.Open, FileAccess.Read))
                            inFs.CopyTo(outFs);
                using (var zip = System.IO.Compression.ZipFile.OpenRead(tmp))
                    foreach (var entry in zip.Entries)
                    {
                        if (entry.FullName.EndsWith("/")) continue;               // directory entry
                        string safe = entry.FullName.Replace('/', '\\');
                        if (safe.StartsWith("\\") || safe.Contains("..")) continue; // path-traversal guard
                        string dest = Path.Combine(samplesRoot, safe);
                        Directory.CreateDirectory(Path.GetDirectoryName(dest));
                        System.IO.Compression.ZipFileExtensions.ExtractToFile(entry, dest, true);
                        n++;
                    }
            }
            finally { try { File.Delete(tmp); } catch { } }
            return n;
        }

        private static void RunOrThrow(string exe, List<string> args, string what)
        {
            // net472 has no ProcessStartInfo.ArgumentList; build a quoted command
            // line (paths under Program Files contain spaces).
            var parts = new List<string>();
            foreach (var a in args) parts.Add(a.IndexOf(' ') >= 0 ? "\"" + a + "\"" : a);
            var psi = new System.Diagnostics.ProcessStartInfo(exe, string.Join(" ", parts))
            {
                UseShellExecute = false, RedirectStandardOutput = true,
                RedirectStandardError = true, CreateNoWindow = true,
            };
            using (var p = System.Diagnostics.Process.Start(psi))
            {
                string err = p.StandardError.ReadToEnd();
                p.StandardOutput.ReadToEnd();
                p.WaitForExit();
                if (p.ExitCode != 0) throw new Exception(what + " failed: " + err);
            }
        }

        // Small, idempotent fixups to stock XDK headers that only clang (not the
        // XDK's cl.exe) rejects. Each is a prepend/replace guarded by a marker so
        // re-running stageclang is a no-op. Kept here (not as patch files) so the
        // set is visible and travels with the unpacker.
        private const string HdrMarker = "/* RXDK360-patched */";

        private static void PatchXdkHeaders(string xboxInc)
        {
            // XStudioApi.h declares fields of enum _NUI_IMAGE_TYPE but #includes
            // nothing, relying on the TU having pulled the NUI headers first. clang
            // rejects a field of a forward-declared enum (MSVC treats it as int), so
            // make the header self-sufficient by pulling nuiapi.h (the umbrella that
            // defines NUIAPI and then includes NuiImageCamera.h, which #errors if
            // included on its own). nuiapi.h has its own include guard, so a TU that
            // already included it is unaffected.
            PrependInclude(Path.Combine(xboxInc, "XStudioApi.h"), "nuiapi.h");

            // stdio.h/wchar.h/mbstring.h each define `typedef struct _iobuf FILE;`
            // under `#ifndef _FILE_DEFINED`. picolibc already provides FILE (as
            // struct __file) under its own guard `_FILE_DECLARED`, which the XDK
            // guard does not know about -- so the block fires and clang reports a
            // typedef redefinition (_iobuf vs __file). Teach the XDK guard about
            // picolibc's so the block is skipped whenever picolibc's FILE is in
            // scope, leaving one FILE (picolibc's) for the whole TU.
            foreach (var h in new[] { "stdio.h", "wchar.h", "mbstring.h" })
                ReplaceOnce(Path.Combine(xboxInc, h),
                    "#ifndef _FILE_DEFINED\r\nstruct _iobuf",
                    "#if !defined(_FILE_DEFINED) && !defined(_FILE_DECLARED) /* RXDK360 */\r\nstruct _iobuf",
                    "#ifndef _FILE_DEFINED\nstruct _iobuf",
                    "#if !defined(_FILE_DEFINED) && !defined(_FILE_DECLARED) /* RXDK360 */\nstruct _iobuf");

            // __lvx/__stvx (vectorintrinsics.h) are `static __forceinline`, bodied as
            // `*(__vector4*)((char*)base + offset)`. On the real XDK compiler that maps
            // to the lvx/stvx instruction, which IGNORES the low 4 address bits (it
            // truncates the effective address to the 16-byte boundary). clang instead
            // lowers the 16-byte __vector4 aggregate load/store to two doubleword
            // ld/std at the exact EA, which (a) raise STATUS_DATATYPE_MISALIGNMENT on an
            // under-aligned pointer (the Xenon has no misaligned ld/std) and (b) touch
            // the wrong bytes -- source code (e.g. the unaligned store-via-read-modify-
            // write in FastCPU's VMXSkinning1: __lvx(p,11)/__lvx(p,0) + __lvsr + __vperm
            // + __stvx) relies on the lvx truncation. Mask EA down to the 16-byte
            // boundary in the body so clang matches the instruction: the loads are then
            // naturally aligned (no fault) and the bytes read/written are correct. The
            // anchor is single-line, so one spelling serves both line endings.
            string vint = Path.Combine(xboxInc, "vectorintrinsics.h");
            ReplaceOnce(vint,
                "    return *(const __vector4*)((const char*)base + offset);",
                "    return *(const __vector4*)((const char*)base + offset - (int)(((__SIZE_TYPE__)((const char*)base + offset)) & 15)); /* RXDK360: lvx truncates EA to 16B boundary */",
                "    return *(const __vector4*)((const char*)base + offset);",
                "    return *(const __vector4*)((const char*)base + offset - (int)(((__SIZE_TYPE__)((const char*)base + offset)) & 15)); /* RXDK360: lvx truncates EA to 16B boundary */");
            ReplaceOnce(vint,
                "    *(__vector4*)((char*)base + offset) = vSrc;",
                "    *(__vector4*)((char*)base + offset - (int)(((__SIZE_TYPE__)((char*)base + offset)) & 15)) = vSrc; /* RXDK360: stvx truncates EA to 16B boundary */",
                "    *(__vector4*)((char*)base + offset) = vSrc;",
                "    *(__vector4*)((char*)base + offset - (int)(((__SIZE_TYPE__)((char*)base + offset)) & 15)) = vSrc; /* RXDK360: stvx truncates EA to 16B boundary */");
        }

        // Rewrite libc++'s _LIBCPP_LIBC_NEWLIB ctype_base branch to build its masks
        // from picolibc's __CTYPE_* macros instead of the single-letter _U.._X legacy
        // macros. The two sets have identical values (e.g. _X == __CTYPE_HEX), so the
        // static const mask constants are unchanged -- purely a source-level rename to
        // stop the legacy names polluting the global namespace and colliding with the
        // stock XDK headers. Idempotent (literal replace; second run is a no-op).
        private static bool PatchLibcxxCtype(string clangRoot)
        {
            string path = Path.Combine(clangRoot, "include", "libcxx", "__locale_dir", "ctype_base.h");
            if (!File.Exists(path))
                return false;
            string txt = File.ReadAllText(path);
            // Each pattern is a full `static_cast<mask>(...)` expression whose ')' anchors
            // it, so replacements can't overlap; ordered longest-first for clarity.
            string[][] map = new string[][]
            {
                new[] { "static_cast<mask>(_P | _U | _L | _N | _B)", "static_cast<mask>(__CTYPE_PUNCT | __CTYPE_UPPER | __CTYPE_LOWER | __CTYPE_DIGIT | __CTYPE_BLANK)" },
                new[] { "static_cast<mask>(_X | _N)",                "static_cast<mask>(__CTYPE_HEX | __CTYPE_DIGIT)" },
                new[] { "static_cast<mask>(_U | _L)",               "static_cast<mask>(__CTYPE_UPPER | __CTYPE_LOWER)" },
                new[] { "static_cast<mask>(_S)",                    "static_cast<mask>(__CTYPE_SPACE)" },
                new[] { "static_cast<mask>(_C)",                    "static_cast<mask>(__CTYPE_CNTRL)" },
                new[] { "static_cast<mask>(_U)",                    "static_cast<mask>(__CTYPE_UPPER)" },
                new[] { "static_cast<mask>(_L)",                    "static_cast<mask>(__CTYPE_LOWER)" },
                new[] { "static_cast<mask>(_N)",                    "static_cast<mask>(__CTYPE_DIGIT)" },
                new[] { "static_cast<mask>(_P)",                    "static_cast<mask>(__CTYPE_PUNCT)" },
                new[] { "static_cast<mask>(_B)",                    "static_cast<mask>(__CTYPE_BLANK)" },
            };
            int n = 0;
            foreach (var pair in map)
                if (txt.Contains(pair[0])) { txt = txt.Replace(pair[0], pair[1]); n++; }
            if (n > 0) File.WriteAllText(path, txt);
            return n > 0;
        }

        // Replace the first occurrence of a fixed anchor (CRLF and LF forms tried
        // in turn), idempotent because the replacement no longer contains the
        // anchor. A file already patched (or without the anchor) is left as-is.
        private static void ReplaceOnce(string path, string crlfFrom, string crlfTo,
                                        string lfFrom, string lfTo)
        {
            if (!File.Exists(path))
                return;
            string txt = File.ReadAllText(path);
            int i = txt.IndexOf(crlfFrom, StringComparison.Ordinal);
            if (i >= 0) { File.WriteAllText(path, txt.Substring(0, i) + crlfTo + txt.Substring(i + crlfFrom.Length)); return; }
            i = txt.IndexOf(lfFrom, StringComparison.Ordinal);
            if (i >= 0) { File.WriteAllText(path, txt.Substring(0, i) + lfTo + txt.Substring(i + lfFrom.Length)); }
        }

        private static void PrependInclude(string header, string include)
        {
            if (!File.Exists(header))
                return;
            string txt = File.ReadAllText(header);
            if (txt.StartsWith(HdrMarker, StringComparison.Ordinal))
                return;                                   // already patched
            File.WriteAllText(header,
                HdrMarker + "\r\n#include \"" + include + "\"\r\n" + txt);
        }

        private static void CopyDir(string src, string dst, List<string> undo)
        {
            Directory.CreateDirectory(dst);
            foreach (var d in Directory.GetDirectories(src, "*", SearchOption.AllDirectories))
                Directory.CreateDirectory(d.Replace(src, dst));
            foreach (var f in Directory.GetFiles(src, "*", SearchOption.AllDirectories))
            {
                string dest = f.Replace(src, dst);
                File.Copy(f, dest, true);
                undo.Add("file|" + dest);
            }
        }

        private static int Run(string[] args)
        {
            try
            {
                // verbs: unpack | install | uninstall | stageclang | vsinstall | vsuninstall
                if (args.Length >= 1 && args[0].Equals("uninstall", StringComparison.OrdinalIgnoreCase))
                {
                    if (args.Length < 2) return Usage();
                    ManifestInstaller.Uninstall(Path.Combine(args[1], UndoLog));
                    Report.Line("uninstalled from " + args[1]);
                    return 0;
                }

                if (args.Length >= 1 && (args[0].Equals("vsinstall", StringComparison.OrdinalIgnoreCase)
                    || args[0].Equals("vsuninstall", StringComparison.OrdinalIgnoreCase)))
                {
                    var rest = new string[Math.Max(0, args.Length - 1)];
                    if (rest.Length > 0) Array.Copy(args, 1, rest, 0, rest.Length);
                    return VsIntegration.Run(args[0].Equals("vsuninstall", StringComparison.OrdinalIgnoreCase), rest);
                }

                if (args.Length >= 1 && args[0].Equals("stageclang", StringComparison.OrdinalIgnoreCase))
                {
                    if (args.Length < 3) return Usage();
                    StageClang(args[1], args[2]);
                    return 0;
                }

                // Test/CI helper: build kernel_import.a directly from a lib dir.
                //   genkernellib <libDir> <clang.exe> <llvm-ar.exe> <out.a>
                if (args.Length >= 1 && args[0].Equals("genkernellib", StringComparison.OrdinalIgnoreCase))
                {
                    if (args.Length < 5) return Usage();
                    int ni = KernelImportLib.Generate(args[1], args[2], args[3], "powerpc-unknown-xbox360", args[4]);
                    Report.Line("kernel_import.a: {0} imports -> {1}", ni, args[4]);
                    return 0;
                }

                // Materialise the RXDK360-Samples binary assets (split-zip parts) in
                // place -- the C# twin of samples\tools\Manage-Assets.ps1 unpack, run
                // at install so the shipped samples have their runtime media.
                if (args.Length >= 1 && args[0].Equals("unpacksamples", StringComparison.OrdinalIgnoreCase))
                {
                    if (args.Length < 2) return Usage();
                    int nu = UnpackSampleAssets(args[1]);
                    Report.Line("unpacked {0} sample asset(s) -> {1}", nu, args[1]);
                    return 0;
                }

                // Test/CI helper: translate one PPC-COFF .lib to an ELF .a.
                //   coffarchive <lib> <out.a> [<runtime1.a> <runtime2.a> ...]
                if (args.Length >= 1 && args[0].Equals("coffarchive", StringComparison.OrdinalIgnoreCase))
                {
                    if (args.Length < 3) return Usage();
                    var runtimes = new string[Math.Max(0, args.Length - 3)];
                    if (runtimes.Length > 0) Array.Copy(args, 3, runtimes, 0, runtimes.Length);
                    int ns = Coff2Elf.Translate(args[1], args[2], runtimes);
                    Report.Line("coffarchive: {0} indexed symbols -> {1}", ns, args[2]);
                    return 0;
                }

                bool dryRun = false;
                var pos = new List<string>();
                for (int i = 0; i < args.Length; i++)
                {
                    if (args[i].Equals("--dry-run", StringComparison.OrdinalIgnoreCase)) dryRun = true;
                    else if (args[i].Equals("--progress", StringComparison.OrdinalIgnoreCase) && i + 1 < args.Length)
                        Report.File = args[++i];
                    else pos.Add(args[i]);
                }
                args = pos.ToArray();

                bool install = args.Length >= 1 && args[0].Equals("install", StringComparison.OrdinalIgnoreCase);
                int a = install ? 1 : 0;
                if (args.Length - a < 2) return Usage();
                string setupExe = args[a];
                string dest = args[a + 1];
                bool preExtracted = install && Directory.Exists(setupExe) && File.Exists(Path.Combine(setupExe, "manifest.csv"));
                if (!preExtracted && !File.Exists(setupExe)) { Console.Error.WriteLine("not found: " + setupExe); return 2; }

                if (!install)
                {
                    // plain unpack: extract the raw XDK\... tree to <outDir>
                    Directory.CreateDirectory(dest);
                    ExtractAll(setupExe, dest);
                    return 0;
                }

                // install: extract to a staging area (unless already extracted), then
                // replay manifest.csv relocated for RXDK-360. Stage under the DEST drive
                // root with a short name (e.g. C:\rxs1a2b3c4d): the XDK carries very deep
                // relative paths and %TEMP%\rxdk_stage_<32-char-GUID>\... can push a file
                // past the 260-char MAX_PATH, aborting the extract mid-cab. A short prefix
                // keeps them in range. (Installer runs elevated, so the drive root is
                // writable; fall back to %TEMP% if the root can't be determined.)
                string stageRoot = Path.GetPathRoot(Path.GetFullPath(dest));
                if (string.IsNullOrEmpty(stageRoot)) stageRoot = Path.GetTempPath();
                string staging = preExtracted ? setupExe
                    : Path.Combine(stageRoot, "rxs" + Guid.NewGuid().ToString("N").Substring(0, 8));
                try
                {
                    if (!preExtracted) { Directory.CreateDirectory(staging); ExtractAll(setupExe, staging); }
                    Directory.CreateDirectory(dest);
                    Report.Line("applying manifest -> {0}{1}", dest, dryRun ? "  (DRY RUN)" : "");
                    new ManifestInstaller(staging, dest) { DryRun = dryRun }.Run(Path.Combine(dest, UndoLog));
                    Report.Line("done.");
                }
                finally { if (!preExtracted) { try { Directory.Delete(staging, true); } catch { } } }
                return 0;
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine("error: " + ex.Message);
                return 1;
            }
        }

        private static int Usage()
        {
            Console.Error.WriteLine("usage:");
            Console.Error.WriteLine("  RxdkXdkUnpacker <XDKSetup.exe> <outDir>            (extract the XDK\\ tree)");
            Console.Error.WriteLine("  RxdkXdkUnpacker install <XDKSetup.exe> <installDir> (manifest-driven install)");
            Console.Error.WriteLine("  RxdkXdkUnpacker uninstall <installDir>              (reverse an install)");
            Console.Error.WriteLine("  RxdkXdkUnpacker stageclang <appRoot> <clangRoot>   (patch headers + build archives in place)");
            Console.Error.WriteLine("  RxdkXdkUnpacker unpacksamples <samplesRoot>        (materialise RXDK360-Samples assets in place)");
            Console.Error.WriteLine("  RxdkXdkUnpacker vsinstall   <vs20xx|vsintegrationDir> [--skip-vsix] [--skip-platform]");
            Console.Error.WriteLine("  RxdkXdkUnpacker vsuninstall <vs20xx|vsintegrationDir>");
            return 2;
        }

        // Extract every cabinet in the setup EXE to destDir. Returns file count.
        private static int ExtractAll(string setupExe, string destDir)
        {
            var cabs = FindCabinets(setupExe);
            Report.Line("found {0} cabinet(s) in {1}", cabs.Count, Path.GetFileName(setupExe));
            if (cabs.Count == 0)
                throw new InvalidDataException("no MSCF cabinet found - is this an Xbox 360 XDK setup EXE?");

            int total = 0;
            string tmp = Path.Combine(Path.GetTempPath(), "rxdk_cab_" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(tmp);
            try
            {
                using (var fs = File.OpenRead(setupExe))
                {
                    int i = 0;
                    foreach (var c in cabs)
                    {
                        string part = Path.Combine(tmp, "part.cab");
                        CarveTo(fs, c.Offset, c.Size, part);
                        using (var fdi = new FdiExtractor())
                            total += fdi.Extract(part, destDir);
                        Report.Line("  cab {0}/{1}: {2} files ({3:n0} bytes)", ++i, cabs.Count, c.Files, c.Size);
                        File.Delete(part);
                    }
                }
            }
            finally { try { Directory.Delete(tmp, true); } catch { } }
            Report.Line("extracted {0} files", total);
            return total;
        }

        private struct CabRef { public long Offset; public long Size; public int Files; }

        // Walk the concatenated cabinet chain starting at the PE overlay.
        private static List<CabRef> FindCabinets(string path)
        {
            var list = new List<CabRef>();
            using (var fs = File.OpenRead(path))
            using (var br = new BinaryReader(fs))
            {
                long overlay = OverlayStart(fs, br);
                long off = overlay;
                long len = fs.Length;
                var hdr = new byte[36];
                while (off + 36 <= len)
                {
                    fs.Position = off;
                    if (fs.Read(hdr, 0, 36) != 36) break;
                    if (!(hdr[0] == 'M' && hdr[1] == 'S' && hdr[2] == 'C' && hdr[3] == 'F')) break;
                    long cbCab = BitConverter.ToUInt32(hdr, 8);
                    ushort cFiles = BitConverter.ToUInt16(hdr, 28);
                    if (cbCab < 36 || off + cbCab > len) break;
                    list.Add(new CabRef { Offset = off, Size = cbCab, Files = cFiles });
                    off += cbCab;
                }
            }
            return list;
        }

        // End of the last PE section = start of the appended cabinet payload.
        private static long OverlayStart(FileStream fs, BinaryReader br)
        {
            fs.Position = 0x3c;
            uint pe = br.ReadUInt32();
            fs.Position = pe + 6;
            ushort nsec = br.ReadUInt16();
            fs.Position = pe + 20;
            ushort optSize = br.ReadUInt16();
            long secTab = pe + 24 + optSize;
            long end = 0;
            for (int i = 0; i < nsec; i++)
            {
                fs.Position = secTab + i * 40 + 16;
                uint rawSize = br.ReadUInt32();
                uint rawPtr = br.ReadUInt32();
                end = Math.Max(end, (long)rawPtr + rawSize);
            }
            return end;
        }

        private static void CarveTo(FileStream src, long offset, long size, string dest)
        {
            src.Position = offset;
            using (var dst = File.Create(dest))
            {
                var buf = new byte[1 << 20];
                long remaining = size;
                while (remaining > 0)
                {
                    int want = (int)Math.Min(buf.Length, remaining);
                    int got = src.Read(buf, 0, want);
                    if (got <= 0) throw new IOException("unexpected EOF carving cabinet");
                    dst.Write(buf, 0, got);
                    remaining -= got;
                }
            }
        }
    }
}
