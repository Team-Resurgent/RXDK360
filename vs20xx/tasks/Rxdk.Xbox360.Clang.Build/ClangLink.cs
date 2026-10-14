// SPDX-License-Identifier: GPL-3.0-or-later
// Part of RXDK-360 - see LICENSE.md for the full GNU GPL v3.

using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;
using Microsoft.Build.Framework;
using Microsoft.Build.Utilities;

namespace Rxdk.Xbox360.Clang.Build
{
    /// <summary>
    /// Link compiled objects into a bootable XEX with the modern (LLVM) toolchain,
    /// resolving the kernel imports a title calls. Writes the layout script and
    /// links once against the global kernel import library (kernel_import.a), whose
    /// per-import COMDAT groups let --gc-sections keep exactly the thunks the title
    /// reaches; binds those kept imports (BindKernelImports: patch the per-title
    /// module index, emit the manifest), then packs and debug-signs with XexTool.
    /// </summary>
    public sealed class ClangLink : ClangTool
    {
        [Required] public string ClangPath { get; set; }
        [Required] public string LldPath { get; set; }
        [Required] public string XexToolPath { get; set; }

        [Required] public ITaskItem[] Objects { get; set; }

        /// <summary>Output .xex.</summary>
        [Required] public string OutputFile { get; set; }

        /// <summary>Scratch dir for the .ld / .elf / stubs (the project IntDir).</summary>
        [Required] public string IntDir { get; set; }

        /// <summary>XDK lib\xbox directory (short-import members -&gt; console ordinals).</summary>
        [Required] public string XdkLibDir { get; set; }

        /// <summary>Where bare library names resolve (coff2elf archives).</summary>
        public string CoffDir { get; set; }
        /// <summary>Where the clang runtime archives (libc.a/libcpp.a) live.</summary>
        public string LibcDir { get; set; }

        public string MsTriple { get; set; } = "powerpc64-unknown-xbox360";
        public string BaseAddress { get; set; } = "0x82000000";

        /// <summary>Extra libraries: bare name -&gt; CoffDir\name.a, or a full path/.a.</summary>
        public string[] Libraries { get; set; }
        /// <summary>The project's standard Linker -&gt; Input -&gt; Additional Dependencies
        /// (e.g. "xboxkrnl.lib;xapilib.lib;d3d9.lib;..."), mirroring an official title.
        /// Each ".lib" is mapped to the clang toolchain's equivalent - see
        /// ResolveLibraries.</summary>
        public string[] AdditionalDependencies { get; set; }
        /// <summary>Do not auto-link the clang runtime (libc.a, libcpp.a, xapilib.a).</summary>
        public bool NoDefaultLibs { get; set; }

        public string AdditionalOptions { get; set; }
        public bool KeepElf { get; set; }

        // Debug/instrumented XDK libs whose needed symbols (PIX*, RtlDebug*, DmDebug*)
        // are weak COMDAT code in members (pix.o, rtldbg.o, debugheap.o) that a normal
        // archive scan won't extract for a strong reference. Collected by ResolveLibraries
        // and linked with --whole-archive so those members are force-included; --gc-sections
        // then drops whatever the title doesn't use, and --allow-multiple-definition lets a
        // strong kernel_import.a record override any weak import stub these also carry.
        private readonly List<string> _wholeArchive = new List<string>();

        // Xbox 360 Image Conversion page (same metadata as legacy imagexex).
        public string TitleID { get; set; }
        public string LanKey { get; set; }
        public string HeapSize { get; set; }
        public string WorkspaceSize { get; set; }
        public bool ExportByName { get; set; }
        public bool OpticalDiscDriveMapping { get; set; }
        public bool Pal50Incompatible { get; set; }
        public bool MultiDiscTitle { get; set; }
        public bool PreferBigButtonInput { get; set; }
        public bool CrossPlatformSystemLink { get; set; }
        public bool AllowAvatarGetMetadata { get; set; }
        public bool AllowControllerSwapping { get; set; }
        public bool RequireFullExperience { get; set; }
        public bool GameVoiceRequiredUI { get; set; }
        public bool KinectElevationControl { get; set; }
        public string KinectSupportLevel { get; set; }
        public string ConfigurationFile { get; set; }
        public string[] AdditionalSections { get; set; }

        private static readonly Regex UndefinedRe = new Regex(@"undefined symbol: (\S+)", RegexOptions.Compiled);
        private static readonly Regex UnresolvedRe = new Regex(@"unresolved \(not kernel imports\): (.+)", RegexOptions.Compiled);

        public override bool Execute()
        {
            Directory.CreateDirectory(IntDir);
            string outBase = Path.Combine(IntDir, Path.GetFileNameWithoutExtension(OutputFile));
            string layout = outBase + ".ld";
            string elf = outBase + ".elf";

            uint baseAddr = ParseBase(BaseAddress);
            int page = baseAddr < 0x90000000u ? 0x10000 : 0x1000;
            File.WriteAllText(layout, Layout(baseAddr, page));

            var objs = new List<string>();
            foreach (var o in Objects) objs.Add(o.GetMetadata("FullPath"));

            var libs = ResolveLibraries();
            foreach (var lib in libs)
                if (!File.Exists(lib)) { Log.LogError("library not found: {0}", lib); return false; }

            var ldflags = ExtraLinkerArgs();

            // Single link against the global kernel import library (kernel_import.a,
            // in `libs` via ResolveLibraries). Every kernel/xam/xbdm export is its own
            // gc-droppable COMDAT group, so --gc-sections keeps exactly the thunks the
            // title reaches -- no trial link, no per-title genstubs. A symbol that is
            // neither defined nor a kernel import is a genuine unresolved-symbol error,
            // which this link reports directly.
            var final = LinkElf(objs, libs, null, layout, elf, ldflags);
            if (final.ExitCode != 0)
            {
                LogDiagnostics(final.Combined);
                Log.LogError("link failed");
                return false;
            }

            // Bind the kernel imports the link kept from the global kernel_import.a
            // (which leaves module_index a placeholder): patch the real per-title
            // module index into each record word and emit the import manifest.
            string manifest = BindKernelImports(elf, outBase);
            if (Log.HasLoggedErrors) return false;

            // Pack the ELF into a XEX2, then debug-sign it. A zero RSA signature
            // is classified Retail; this kit then returns LDRX C000007B. `-m d`
            // fills hashes and the debug signature (observed required for load).
            string packed = OutputFile + ".unsigned.xex";
            var packArgs = new List<string> { "pack", elf, "-o", packed };
            if (manifest != null) { packArgs.Add("--import-manifest"); packArgs.Add(manifest); }
            var pack = Run(XexToolPath, packArgs);
            Log.LogMessage(MessageImportance.Normal, pack.StdOut);
            if (pack.ExitCode != 0)
            {
                LogDiagnostics(pack.Combined);
                Log.LogError("pack failed");
                return false;
            }

            string xml = Path.Combine(IntDir, Path.GetFileNameWithoutExtension(OutputFile) + ".xex.xml");
            if (!WriteImageXexXml(xml))
                return false;
            if (File.Exists(xml))
            {
                var apply = Run(XexToolPath, new[] { "applyxml", packed, "--xml", xml, "-o", packed });
                Log.LogMessage(MessageImportance.Normal, apply.StdOut);
                if (apply.ExitCode != 0)
                {
                    LogDiagnostics(apply.Combined);
                    Log.LogError("applyxml failed (Image Conversion settings)");
                    return false;
                }
            }

            var sign = Run(XexToolPath, new[] { "-m", "d", "-o", OutputFile, packed });
            Log.LogMessage(MessageImportance.Normal, sign.StdOut);
            try { File.Delete(packed); } catch { }
            if (sign.ExitCode != 0)
            {
                LogDiagnostics(sign.Combined);
                Log.LogError("XexTool -m d failed");
                return false;
            }

            if (KeepElf)
            {
                // Keep the DWARF-carrying ELF beside the XEX as the symbol file for
                // source-level debugging (maps onto the XEX load base).
                try
                {
                    string sym = Path.ChangeExtension(OutputFile, ".elf");
                    if (!string.Equals(Path.GetFullPath(sym), Path.GetFullPath(elf), StringComparison.OrdinalIgnoreCase))
                        File.Copy(elf, sym, true);
                }
                catch { /* symbol-file copy is best-effort */ }
            }
            else if (File.Exists(elf)) File.Delete(elf);
            Log.LogMessage(MessageImportance.High, "  -> " + OutputFile);
            return !Log.HasLoggedErrors;
        }

        private ProcResult LinkElf(List<string> objs, List<string> libs, string stubs,
                                   string layout, string elf, List<string> ldflags)
        {
            var args = new List<string> { "-T", layout, "-e", "_start", "--error-limit=0",
                                          "--no-demangle", "--gc-sections",
                                          // Ignore #pragma comment(lib, "x.lib") directives from XDK
                                          // headers - the modern link names its ELF libraries explicitly.
                                          "--no-dependent-libraries",
                                          // Our modern CRT (libc.a, scanned first) shadows the XDK's
                                          // MSVC CRT, but XDK import archives bundle CRT-level symbols
                                          // (ceilf, wsprintf, ...) into members pulled for other code,
                                          // so the same symbol can be defined in both. Take the first
                                          // definition (our runtime) instead of erroring; symbols only
                                          // the XDK provides are unaffected.
                                          "--allow-multiple-definition" };
            args.AddRange(ldflags);
            args.AddRange(objs);
            if (stubs != null) args.Add(stubs);
            // Force-include the debug XDK libs' weak COMDAT code (PIX*/RtlDebug*/DmDebug*)
            // that a lazy archive scan skips; --gc-sections trims the unused remainder.
            if (_wholeArchive.Count > 0)
            {
                args.Add("--whole-archive");
                foreach (var wa in _wholeArchive)
                    if (File.Exists(wa)) args.Add(wa);
                args.Add("--no-whole-archive");
            }
            if (libs.Count > 0)
            {
                args.Add("--start-group");
                args.AddRange(libs);
                args.Add("--end-group");
            }
            args.Add("-o");
            args.Add(elf);
            return Run(LldPath, args);
        }

        // XEX-import modules: on the 360 a title imports these from separate modules
        // (the kernel, xam.xex, and - dev only - xbdm.xex) via its XEX import table,
        // NOT by linking their code. Their symbols are left undefined so the trial
        // link discovers them and XexTool genstubs synthesises the imports (recorded
        // in the import manifest that 'pack --import-manifest' writes into the XEX).
        // Their .a's in the lib dir are tiny import stubs; linking one directly would
        // both duplicate the genstubs and leave the XEX with no declared import, so a
        // listed xboxkrnl.lib/xam.lib/xbdm.lib is skipped here on purpose.
        private static readonly HashSet<string> XexImportModules =
            new HashSet<string>(StringComparer.OrdinalIgnoreCase) { "xboxkrnl", "xam", "xbdm" };

        /// <summary>
        /// Resolve the archives to link. Two sources, treated the same: Libraries
        /// (the RxdkClangLibs bare-name list) and AdditionalDependencies (the
        /// project's standard Linker Input field, mirroring an official title). Each
        /// entry maps like this:
        ///   * a full path or a *.a           -> taken as-is
        ///   * the CRT (libc/libcmt/libcpp..) -> the clang runtime archive
        ///   * an XEX-import module (xboxkrnl/xam/xbdm) -> skipped (genstubs handles it)
        ///   * anything else                  -> CoffDir\name.a (our translated code;
        ///                                       --gc-sections drops what the title
        ///                                       does not use, so over-listing is safe)
        /// The C/C++ runtime is linked implicitly, like libcmt in an official project.
        /// </summary>
        private List<string> ResolveLibraries()
        {
            var user = new List<string>();

            void AddSpec(string spec)
            {
                foreach (var n in spec.Split(',', ';'))
                {
                    var name = n.Trim();
                    if (name.Length == 0) continue;
                    // A full path or an explicit .a is taken verbatim.
                    if (name.IndexOf(Path.DirectorySeparatorChar) >= 0 || Path.IsPathRooted(name) ||
                        name.EndsWith(".a", StringComparison.OrdinalIgnoreCase))
                    { user.Add(name); continue; }
                    // Strip a trailing .lib (the Additional Dependencies form).
                    if (name.EndsWith(".lib", StringComparison.OrdinalIgnoreCase))
                        name = name.Substring(0, name.Length - 4);
                    if (name.Length == 0) continue;
                    var lower = name.ToLowerInvariant();
                    // CRT aliases (retail + debug spellings) -> the clang runtime.
                    if (lower == "libc" || lower == "libcmt" || lower == "libcmtd" || lower == "msvcrt")
                    { if (!string.IsNullOrEmpty(LibcDir)) user.Add(Path.Combine(LibcDir, "libc.a")); continue; }
                    if (lower == "libcpp" || lower == "libc++" || lower == "libcpmt" || lower == "libcpmtd")
                    { if (!string.IsNullOrEmpty(LibcDir)) user.Add(Path.Combine(LibcDir, "libcpp.a")); continue; }
                    // Retail xapilib is the canonical clang-runtime companion; the debug
                    // variant (xapilibd) carries extra debug-only code (PIX*, RtlDebug*)
                    // the retail one lacks, so a Debug title must link xapilibd.a itself.
                    if (lower == "xapilib")
                    { if (!string.IsNullOrEmpty(CoffDir)) user.Add(Path.Combine(CoffDir, "xapilib.a")); continue; }
                    if (lower == "xapilibd")
                    { if (!string.IsNullOrEmpty(CoffDir)) _wholeArchive.Add(Path.Combine(CoffDir, "xapilibd.a")); continue; }
                    // xbdm is an XEX-import module for its console exports (genstubs handles
                    // those), but its debug lib also ships real code (debugheap.o ->
                    // DmDebugAlloc/Free) a Debug title calls -- pull that via whole-archive.
                    if (lower == "xbdm")
                    { if (!string.IsNullOrEmpty(CoffDir)) _wholeArchive.Add(Path.Combine(CoffDir, "xbdm.a")); continue; }
                    // Other XEX-import modules are resolved by genstubs, not linked.
                    if (XexImportModules.Contains(lower)) continue;
                    // Everything else is a translated static library.
                    user.Add(Path.Combine(CoffDir ?? "", name + ".a"));
                }
            }

            if (Libraries != null) foreach (var spec in Libraries) AddSpec(spec);
            if (AdditionalDependencies != null) foreach (var spec in AdditionalDependencies) AddSpec(spec);

            // The modern C/C++ runtime (libc.a, libcpp.a) is implicit, like libcmt.
            // It is placed FIRST so it is scanned ahead of the XDK import archives:
            // both --start-group members can define the same CRT-level symbol (e.g.
            // xapilib.a ships MSVC-built wsprintf/wvsprintf, which read a clang
            // va_list as garbage), and within a group lld takes the definition from
            // the first archive in command-line order. Runtime-first makes our
            // clang-built CRT win those collisions; symbols only the XDK provides
            // still resolve because the group is rescanned until stable. xapilib.a
            // is appended (after title libs) so only its non-CRT members are pulled.
            var pre = new List<string>();
            if (!NoDefaultLibs)
            {
                if (!string.IsNullOrEmpty(LibcDir))
                {
                    pre.Add(Path.Combine(LibcDir, "libcpp.a"));
                    pre.Add(Path.Combine(LibcDir, "libc.a"));
                    // The global kernel import library (like an SDK's xboxkrnl.lib):
                    // one COMDAT group per kernel/xam/xbdm export, so --gc-sections
                    // keeps exactly the thunks the title reaches. This replaces the
                    // old per-title trial-link + XexTool genstubs discovery.
                    var kimp = Path.Combine(LibcDir, "kernel_import.a");
                    if (File.Exists(kimp)) pre.Add(kimp);
                    // Compat library: our clang reimplementations of XDK C++ helper
                    // classes a title subclasses (CXAPOBase, ...), so the subclass
                    // links a modern-ABI base instead of the MSVC-ABI XDK .lib. On
                    // demand -- pulled only by a title that references a member.
                    var compat = Path.Combine(LibcDir, "libcompat.a");
                    if (File.Exists(compat)) pre.Add(compat);
                }
                if (!string.IsNullOrEmpty(CoffDir))
                    user.Add(Path.Combine(CoffDir, "xapilib.a"));
            }

            // De-dupe by normalised full path, keeping first occurrence (so the
            // runtime copies in `pre` take precedence over any listed by the title).
            var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            var result = new List<string>();
            foreach (var l in pre)
            {
                string key;
                try { key = Path.GetFullPath(l); } catch { key = l; }
                if (seen.Add(key)) result.Add(l);
            }
            foreach (var l in user)
            {
                string key;
                try { key = Path.GetFullPath(l); } catch { key = l; }
                if (seen.Add(key)) result.Add(l);
            }
            return result;
        }

        // Unwrap a caller-supplied -Wl,a,b into raw linker arguments (the old zig
        // driver form), else pass the flag through verbatim.
        private List<string> ExtraLinkerArgs()
        {
            var outv = new List<string>();
            if (string.IsNullOrWhiteSpace(AdditionalOptions)) return outv;
            foreach (var f in AdditionalOptions.Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries))
            {
                // Drop MSVC-style (/...) linker options: this toolset drives ld.lld,
                // and the stock platform props inject link.exe defaults (e.g. /XEX:NO)
                // that ld.lld does not understand.
                if (f.StartsWith("/"))
                    Log.LogMessage(MessageImportance.Low, "  (ignoring MSVC-style linker option " + f + ")");
                else if (f.StartsWith("-Wl,"))
                    outv.AddRange(f.Substring(4).Split(','));
                else
                    outv.Add(f);
            }
            return outv;
        }

        private static List<string> UndefinedFrom(string text)
        {
            var seen = new List<string>();
            foreach (Match m in UndefinedRe.Matches(text))
            {
                string name = m.Groups[1].Value;
                if (!seen.Contains(name)) seen.Add(name);
            }
            return seen;
        }

        private static uint Be32(byte[] b, int o) =>
            ((uint)b[o] << 24) | ((uint)b[o + 1] << 16) | ((uint)b[o + 2] << 8) | b[o + 3];
        private static ushort Be16(byte[] b, int o) => (ushort)((b[o] << 8) | b[o + 1]);
        private static void PutBe32(byte[] b, int o, uint v)
        { b[o] = (byte)(v >> 24); b[o + 1] = (byte)(v >> 16); b[o + 2] = (byte)(v >> 8); b[o + 3] = (byte)v; }

        private static readonly Regex ImportLibRe = new Regex(
            "\\{\\s*\"module\"\\s*:\\s*\"(?<m>[^\"]+)\"\\s*,\\s*\"records\"\\s*:\\s*\\[(?<r>[^\\]]*)\\]",
            RegexOptions.Singleline | RegexOptions.Compiled);
        private static readonly Regex ImportRecRe = new Regex("\"([^\"]+)\"", RegexOptions.Compiled);

        /// <summary>
        /// Bind the kernel imports the link kept. A title links exactly one archive
        /// for the console kernel -- the global kernel_import.a (build_import_lib.py)
        /// -- and --gc-sections keeps only the thunks its code reaches. Those thunks
        /// carry their ordinal but a placeholder module_index of 0, because the real
        /// index is the module's slot in THIS title's XEX import name table, which is
        /// not known until we see which modules the title ended up importing. Here we
        /// read the kept __imp_ records out of the linked ELF, ask XexTool genstubs
        /// which module each belongs to (the same ordinal index the library was built
        /// from), patch the real module_index into every record word in place, and
        /// return the manifest for `pack --import-manifest`. No per-title stub archive
        /// is linked; this is just the per-title import binding imagexex would do.
        /// Returns the manifest path, or null when the title imports nothing.
        /// </summary>
        private string BindKernelImports(string elf, string outBase)
        {
            byte[] blob = File.ReadAllBytes(elf);
            if (blob.Length < 0x34 || blob[0] != 0x7F || blob[1] != (byte)'E' ||
                blob[2] != (byte)'L' || blob[3] != (byte)'F')
            { Log.LogError("import binding: not an ELF: {0}", elf); return null; }

            // The ppc64 (ILP32-on-ppc64) toolchain emits ELFCLASS64 objects, so the
            // linked image is an ELF64 executable; the pre-cutover ppc32 path produced
            // ELF32. Read the structural fields (header, section table, symbol table)
            // at the width the file's EI_CLASS dictates. Every address/offset in a
            // 0x82000000 image still fits in 32 bits, so narrowing each (8-byte on
            // ELF64) field to uint is safe; the import records patched below are 32-bit
            // words of section content and are unaffected by the ELF class.
            bool is64 = blob[4] == 2;   // EI_CLASS: 1 = ELFCLASS32, 2 = ELFCLASS64
            uint BeN(int o, int n) { ulong v = 0; for (int k = 0; k < n; k++) v = (v << 8) | blob[o + k]; return (uint)v; }

            int shoff = (int)BeN(is64 ? 0x28 : 0x20, is64 ? 8 : 4);
            int shent = Be16(blob, is64 ? 0x3A : 0x2E);
            int shnum = Be16(blob, is64 ? 0x3C : 0x30);
            int shstrndx = Be16(blob, is64 ? 0x3E : 0x32);
            // Section-header field byte offsets (ELF32 Shdr is 40 bytes, ELF64 64).
            int oAddr = is64 ? 16 : 12, oOff = is64 ? 24 : 16, oSize = is64 ? 32 : 20;
            int oLink = is64 ? 40 : 24, oEnt = is64 ? 56 : 36, wWide = is64 ? 8 : 4;
            int Sh(int i) => shoff + i * shent;
            var secType = new uint[shnum]; var secAddr = new uint[shnum];
            var secOff = new uint[shnum]; var secSize = new uint[shnum]; var secName = new uint[shnum];
            for (int i = 0; i < shnum; i++)
            {
                secName[i] = BeN(Sh(i) + 0, 4); secType[i] = BeN(Sh(i) + 4, 4);
                secAddr[i] = BeN(Sh(i) + oAddr, wWide); secOff[i] = BeN(Sh(i) + oOff, wWide);
                secSize[i] = BeN(Sh(i) + oSize, wWide);
            }
            int shstrOff = (int)secOff[shstrndx];
            string CStr(int at) { int e = at; while (blob[e] != 0) e++; return Encoding.ASCII.GetString(blob, at, e - at); }
            // The IAT records all land in .kvars; only __imp_ symbols there are kernel
            // imports (so a stray dllimport-style __imp_ from elsewhere is ignored).
            uint kvLo = 0, kvHi = 0;
            for (int i = 0; i < shnum; i++)
                if (CStr(shstrOff + (int)secName[i]) == ".kvars") { kvLo = secAddr[i]; kvHi = secAddr[i] + secSize[i]; }

            int symIdx = -1;
            for (int i = 0; i < shnum; i++) if (secType[i] == 2) symIdx = i;   // SHT_SYMTAB
            if (symIdx < 0) return null;
            int symOff = (int)secOff[symIdx], symSize = (int)secSize[symIdx];
            int symEnt = (int)BeN(Sh(symIdx) + oEnt, wWide);
            int strOff = (int)secOff[(int)BeN(Sh(symIdx) + oLink, 4)];   // sh_link -> strtab
            // ELF32 Sym: name(4) value(4) size(4) info(1) other(1) shndx(2); the value
            // sits right after the name. ELF64 Sym: name(4) info(1) other(1) shndx(2)
            // value(8) size(8); the value is an 8-byte field at offset 8.
            int oStValue = is64 ? 8 : 4, wStValue = is64 ? 8 : 4;
            var addr = new Dictionary<string, uint>(StringComparer.Ordinal);
            for (int o = symOff; o + symEnt <= symOff + symSize; o += symEnt)
            {
                uint stName = BeN(o, 4), stValue = BeN(o + oStValue, wStValue);
                if (stName != 0 && stValue != 0) addr[CStr(strOff + (int)stName)] = stValue;
            }

            // Kept imports = the base name of every __imp_ record placed in .kvars.
            var names = new List<string>();
            foreach (var kv in addr)
                if (kv.Key.StartsWith("__imp_", StringComparison.Ordinal) && kv.Value >= kvLo && kv.Value < kvHi)
                    names.Add(kv.Key.Substring(6));
            if (names.Count == 0) return null;
            names.Sort(StringComparer.Ordinal);

            // genstubs is the module oracle: it maps each name to its module and
            // writes the manifest whose library order defines the module indices.
            string manifest = outBase + "_imports.json";
            var gs = Run(XexToolPath, new[] { "genstubs", "--xdk", XdkLibDir, "--names",
                string.Join(",", names), "-o", outBase + "_imports.s", "--manifest", manifest });
            if (gs.ExitCode != 0) { LogDiagnostics(gs.Combined); Log.LogError("import binding (genstubs) failed"); return null; }
            var un = UnresolvedRe.Match(gs.StdOut);
            if (un.Success) { Log.LogError("unresolved symbols (not kernel imports): {0}", un.Groups[1].Value.Trim()); return null; }

            int VaToOff(uint va)
            {
                for (int i = 0; i < shnum; i++)
                    if (secType[i] != 8 && secSize[i] != 0 && va >= secAddr[i] && va < secAddr[i] + secSize[i])
                        return (int)(secOff[i] + (va - secAddr[i]));
                throw new InvalidOperationException($"import record VA 0x{va:X8} is in no section");
            }
            void SetIndex(uint va, int mi)
            {
                int off = VaToOff(va);
                PutBe32(blob, off, (Be32(blob, off) & ~0x00FF0000u) | ((uint)mi << 16));
            }

            // Patch the module index into every kept record, per the manifest's
            // library order. A function lists "__imp_x" (its IAT slot) and "x" (its
            // two-word thunk); a data export lists only "__imp_x".
            string json = File.ReadAllText(manifest);
            int index = 0;
            foreach (Match lib in ImportLibRe.Matches(json))
            {
                foreach (Match rec in ImportRecRe.Matches(lib.Groups["r"].Value))
                {
                    string r = rec.Groups[1].Value;
                    if (!addr.TryGetValue(r, out uint va)) continue;
                    SetIndex(va, index);
                    if (!r.StartsWith("__imp_", StringComparison.Ordinal)) SetIndex(va + 4, index);
                }
                index++;
            }
            File.WriteAllBytes(elf, blob);
            return manifest;
        }

        private static uint ParseBase(string s)
        {
            s = s.Trim();
            if (s.StartsWith("0x") || s.StartsWith("0X"))
                return Convert.ToUInt32(s.Substring(2), 16);
            return Convert.ToUInt32(s, 10);
        }

        /// <summary>
        /// Write the imagexex-style XML that applyxml consumes. Returns false on
        /// a hard error (missing Additional Sections file, etc.). Returns true
        /// and omits the file when there is nothing to apply.
        /// </summary>
        private bool WriteImageXexXml(string path)
        {
            var body = new StringBuilder();
            if (!string.IsNullOrEmpty(ConfigurationFile) && File.Exists(ConfigurationFile))
            {
                // Pull child elements out of the user's imagexex config so one
                // applyxml pass sees both the file and the property-page flags.
                string raw = File.ReadAllText(ConfigurationFile);
                int open = raw.IndexOf("<xex", StringComparison.OrdinalIgnoreCase);
                int inner = open >= 0 ? raw.IndexOf('>', open) : -1;
                int close = raw.LastIndexOf("</xex>", StringComparison.OrdinalIgnoreCase);
                if (inner > 0 && close > inner)
                    body.Append(raw.Substring(inner + 1, close - inner - 1).Trim());
                else
                    Log.LogWarning("Image Conversion Configuration File is not an imagexex <xex> XML; ignoring {0}", ConfigurationFile);
            }
            else if (!string.IsNullOrEmpty(ConfigurationFile))
            {
                Log.LogError("Image Conversion Configuration File not found: {0}", ConfigurationFile);
                return false;
            }

            void Tag(string line)
            {
                if (body.Length > 0 && body[body.Length - 1] != '\n') body.AppendLine();
                body.AppendLine(line);
            }

            if (!string.IsNullOrWhiteSpace(TitleID))
                Tag("  <titleid id=\"" + TitleID.Trim() + "\"/>");
            if (!string.IsNullOrWhiteSpace(LanKey))
                Tag("  <lankey id=\"" + LanKey.Trim() + "\"/>");
            if (!string.IsNullOrWhiteSpace(HeapSize))
                Tag("  <xapiheap size=\"" + HeapSize.Trim() + "\"/>");
            if (!string.IsNullOrWhiteSpace(WorkspaceSize))
                Tag("  <workspace size=\"" + WorkspaceSize.Trim() + "\"/>");
            if (ExportByName) Tag("  <exportnames/>");
            // Privilege 2 = No ODD Mapping. applyxml drops DVD/CD (and XGD2)
            // so the XEX matches a pack imagexex accepts; /privilege:2 alone is IM1069.
            if (OpticalDiscDriveMapping) Tag("  <privilege id=\"2\"/>");
            if (Pal50Incompatible) Tag("  <privilege id=\"10\"/>");
            if (MultiDiscTitle)
            {
                Tag("  <privilege id=\"15\"/>");
                Tag("  <privilege id=\"16\"/>");
            }
            if (PreferBigButtonInput) Tag("  <privilege id=\"25\"/>");
            if (CrossPlatformSystemLink) Tag("  <privilege id=\"14\"/>");
            if (AllowAvatarGetMetadata) Tag("  <privilege id=\"29\"/>");
            if (AllowControllerSwapping) Tag("  <privilege id=\"30\"/>");
            if (RequireFullExperience) Tag("  <privilege id=\"34\"/>");
            if (GameVoiceRequiredUI) Tag("  <privilege id=\"35\"/>");
            if (KinectElevationControl) Tag("  <privilege id=\"37\"/>");
            if (string.Equals(KinectSupportLevel, "RequiresTracking", StringComparison.OrdinalIgnoreCase))
                Tag("  <privilege id=\"38\"/>");
            else if (string.Equals(KinectSupportLevel, "SupportsTracking", StringComparison.OrdinalIgnoreCase))
                Tag("  <privilege id=\"39\"/>");

            if (AdditionalSections != null)
            {
                foreach (var raw in AdditionalSections)
                {
                    if (string.IsNullOrWhiteSpace(raw))
                        continue;
                    string spec = raw.Trim();
                    string secName;
                    string file;
                    int eq = spec.IndexOf('=');
                    if (eq > 0)
                    {
                        secName = spec.Substring(0, eq).Trim();
                        file = spec.Substring(eq + 1).Trim();
                    }
                    else
                    {
                        file = spec;
                        secName = Path.GetFileNameWithoutExtension(file);
                    }
                    if (string.IsNullOrEmpty(secName) || string.IsNullOrEmpty(file))
                    {
                        Log.LogError("Additional Sections entry must be NAME=file (imagexex /section:), not '{0}'", spec);
                        return false;
                    }
                    if (!File.Exists(file))
                    {
                        Log.LogError("Additional Section file not found: {0}", file);
                        return false;
                    }
                    Tag("  <section name=\"" + XmlAttr(secName) + "\" file=\"" + XmlAttr(Path.GetFullPath(file)) + "\"/>");
                }
            }

            if (body.Length == 0) return true;

            Directory.CreateDirectory(Path.GetDirectoryName(path) ?? ".");
            File.WriteAllText(path, "<xex>\n" + body.ToString().TrimEnd() + "\n</xex>\n");
            Log.LogMessage(MessageImportance.Low, "  Image Conversion XML " + path);
            return true;
        }

        static string XmlAttr(string s)
        {
            if (string.IsNullOrEmpty(s)) return "";
            return s.Replace("&", "&amp;").Replace("\"", "&quot;").Replace("<", "&lt;").Replace(">", "&gt;");
        }

        /// <summary>The layout the packer expects (see mktitle.write_layout).</summary>
        private static string Layout(uint baseAddr, int page)
        {
            return
$@"ENTRY(_start)
SECTIONS {{
  . = 0x{baseAddr:X8};
  . += 0x1000;                       /* PE headers; first 64KB page is headers + RO */
  .rodata : {{
    *(.rodata*)
    *(.gcc_except_table .gcc_except_table.*)
  }}
  .pdata : {{                        /* RUNTIME_FUNCTION table (exact fn bounds) */
    *(.pdata)
    *(.pdata.*)
  }}
  .xdata : {{                        /* unwind info the .pdata entries point at */
    *(.xdata)
    *(.xdata.*)
  }}
  .eh_frame : {{                     /* libunwind finds FDEs only between the markers */
    HIDDEN(__eh_frame_start = .);
    KEEP(*(.eh_frame))
    KEEP(*(.eh_frame.*))
    HIDDEN(__eh_frame_end = .);
    PROVIDE_HIDDEN(__eh_frame_hdr_start = .);
    PROVIDE_HIDDEN(__eh_frame_hdr_end = .);
  }}
  ASSERT(__eh_frame_start == ADDR(.eh_frame) &&
         __eh_frame_end == ADDR(.eh_frame) + SIZEOF(.eh_frame),
         ""__eh_frame_start/__eh_frame_end do not bracket all of .eh_frame; libunwind would miss FDEs"")
  . = ALIGN(0x{page:X});             /* CODE after RO: ImageXex IM1031 if RX shares the header page */
  .text   : {{ *(.text*) }}
  . = ALIGN(0x{page:X});             /* import thunks on their own CODE page */
  .kthunks : ALIGN(16) {{ *(.kthunks .kthunks.*) }}
  . = ALIGN(0x{page:X});             /* writable region: IAT then data. HV patches .kvars in place (C0000225 if RO). */
  .kvars  : {{ *(.kvars .kvars.*) }}
  .init_array : {{                   /* C++ static constructors, run pre-main */
    PROVIDE_HIDDEN(__init_array_start = .);
    KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
    KEEP(*(.init_array))
    PROVIDE_HIDDEN(__init_array_end = .);
  }}
  .fini_array : {{                   /* destructors (kept writable, off the CODE page) */
    PROVIDE_HIDDEN(__fini_array_start = .);
    KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
    KEEP(*(.fini_array))
    PROVIDE_HIDDEN(__fini_array_end = .);
  }}
  .CRT : {{                          /* MS CRT initializer table (.CRT$XCA..XCZ) */
    PROVIDE_HIDDEN(__xc_a = .);
    KEEP(*(SORT_BY_NAME(.CRT$XC*)))
    PROVIDE_HIDDEN(__xc_z = .);
  }}
  .data : {{
    /* PPC TOC base. lld synthesizes .TOC. for a PPC64 target but not this PPC32
       one, so some objects (OpenMP outlines, XLSP) reference it unresolved.
       Define it at the conventional GOT+0x8000 anchor. */
    PROVIDE_HIDDEN(.TOC. = . + 0x8000);
    *(.data*)
  }}
  .bss  : {{ *(.bss*) *(COMMON) }}
  /DISCARD/ : {{ *(.comment) *(.note*) }}
}}
";
        }
    }
}
