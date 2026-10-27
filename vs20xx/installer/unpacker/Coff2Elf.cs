// SPDX-License-Identifier: GPL-3.0-or-later
// Part of RXDK-360 - see LICENSE.md for the full GNU GPL v3.
//
// Translate the XDK's PPC COFF import/static libraries to big-endian ELF
// archives at install time, with no Python. This is the C# twin of the
// `archive` mode of tools/coff2elf.py: it reads a Microsoft archive (.lib) of
// big-endian PowerPC COFF objects (machine 0x01F2, a format lld cannot read),
// rewrites each object as a big-endian ELF, and repacks the archive as a
// System V `.a` that lld links natively. Translate's elf64 flag (default true)
// selects ELFCLASS64/EM_PPC64 ELFv2 (powerpc64-unknown-xbox360, e_flags=0x2) to
// match the modern clang toolchain; elf64=false is the legacy ELFCLASS32/EM_PPC.
//
// The output is validated byte-identical against tools/coff2elf.py (its --ppc64
// mode for elf64), so every subtlety of that translator is reproduced here:
// COMDAT section groups (SHT_GROUP), weak-external default-alias resolution,
// COMMON -> SHN_COMMON, STT_SECTION folding, the ADDR32/REL24/REFHI/REFLO+PAIR
// relocation math, the code-signature strong/weak decision,
// locals-before-globals ordering, string-table dedup order, and the ELF-BE /
// System-V-`.a` byte layout (Elf32/Elf64 widths, Elf64_Sym field reorder).
//
// Only the `archive` orchestration is ported (dump/survey/emit/translate-all
// are developer-only reporting modes with no install-time use). net472, no
// NuGet dependencies -- System.* only, mirroring KernelImportLib.cs.

using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace Rxdk.Xdk.Unpacker
{
    internal static class Coff2Elf
    {
        // ---- constants ------------------------------------------------------

        private const ushort ImageFileMachinePowerPcBe = 0x01F2;

        // ELF
        private const ushort ET_REL = 1;
        private const ushort EM_PPC = 20;
        private const ushort EM_PPC64 = 21;
        // PPC64 e_flags ABI-version nibble: 2 selects ELFv2 (matches coff2elf.py's
        // EF_PPC64_ABI_V2). clang stamps 0x2 on powerpc64-unknown-xbox360 objects.
        private const uint EF_PPC64_ABI_V2 = 0x2;
        private const uint SHT_PROGBITS = 1, SHT_SYMTAB = 2, SHT_STRTAB = 3, SHT_RELA = 4, SHT_NOBITS = 8;
        private const uint SHT_GROUP = 17;
        private const uint SHF_WRITE = 0x1, SHF_ALLOC = 0x2, SHF_EXECINSTR = 0x4, SHF_GROUP = 0x200;
        private const uint GRP_COMDAT = 0x1;
        private const int IMAGE_COMDAT_SELECT_ASSOCIATIVE = 5;
        private const byte STB_LOCAL = 0, STB_GLOBAL = 1, STB_WEAK = 2;
        private const byte STT_NOTYPE = 0, STT_OBJECT = 1, STT_FUNC = 2, STT_SECTION = 3;
        private const ushort SHN_UNDEF = 0, SHN_ABS = 0xFFF1, SHN_COMMON = 0xFFF2;

        // PPC ELF relocation types
        private const uint R_PPC_ADDR32 = 1, R_PPC_ADDR16_LO = 4, R_PPC_ADDR16_HA = 6, R_PPC_REL24 = 10;

        // COFF section characteristics
        private const uint IMAGE_SCN_CNT_CODE = 0x00000020;
        private const uint IMAGE_SCN_CNT_INITIALIZED_DATA = 0x00000040;
        private const uint IMAGE_SCN_CNT_UNINITIALIZED_DATA = 0x00000080;
        private const uint IMAGE_SCN_LNK_INFO = 0x00000200;
        private const uint IMAGE_SCN_LNK_REMOVE = 0x00000800;
        private const uint IMAGE_SCN_LNK_COMDAT = 0x00001000;
        private const uint IMAGE_SCN_MEM_WRITE = 0x80000000;
        private const uint IMAGE_SCN_MEM_EXECUTE = 0x20000000;

        // COFF storage classes
        private const byte IMAGE_SYM_CLASS_EXTERNAL = 2;
        private const byte IMAGE_SYM_CLASS_STATIC = 3;
        private const byte IMAGE_SYM_CLASS_WEAK_EXTERNAL = 105;

        private static readonly byte[] ArchiveMagic = Encoding.ASCII.GetBytes("!<arch>\n");

        // ---- little/big-endian byte helpers ---------------------------------

        private static ushort U16LE(byte[] b, int o) { return (ushort)(b[o] | (b[o + 1] << 8)); }
        private static uint U32LE(byte[] b, int o)
        { return (uint)(b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)); }
        private static int I32LE(byte[] b, int o) { return unchecked((int)U32LE(b, o)); }
        private static uint U32BE(byte[] b, int o)
        { return (uint)((b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]); }

        private static void PutU16BE(List<byte> o, ushort v) { o.Add((byte)(v >> 8)); o.Add((byte)v); }
        private static void PutU32BE(List<byte> o, uint v)
        { o.Add((byte)(v >> 24)); o.Add((byte)(v >> 16)); o.Add((byte)(v >> 8)); o.Add((byte)v); }
        private static void PutU64BE(List<byte> o, ulong v)
        { for (int s = 56; s >= 0; s -= 8) o.Add((byte)(v >> s)); }

        private static int IndexOfZero(byte[] b, int start)
        {
            for (int i = start; i < b.Length; i++) if (b[i] == 0) return i;
            return b.Length;
        }

        // ---- Microsoft archive (.lib) ---------------------------------------

        private sealed class Member { public string Name; public byte[] Data; public int Offset; }

        // Yield every archive member in file order (both "/" linker members
        // included; callers skip by name). The "//" longnames member at index 2
        // is captured into <paramref name="longnames"/> and not yielded.
        private static IEnumerable<Member> ReadArchive(byte[] blob, byte[][] longnamesBox)
        {
            longnamesBox[0] = new byte[0];
            for (int i = 0; i < ArchiveMagic.Length; i++)
                if (i >= blob.Length || blob[i] != ArchiveMagic[i])
                    throw new InvalidDataException("not a Microsoft archive: bad !<arch> magic");

            int pos = 8;
            var raw = new List<Member>();
            while (pos + 60 <= blob.Length)
            {
                if (blob[pos + 58] != (byte)'`' || blob[pos + 59] != (byte)'\n')
                    throw new InvalidDataException("malformed archive header at 0x" + pos.ToString("X"));
                string name = Encoding.ASCII.GetString(blob, pos, 16).TrimEnd();
                int size = int.Parse(Encoding.ASCII.GetString(blob, pos + 48, 10).Trim());
                int dataOff = pos + 60;
                var data = new byte[size];
                Array.Copy(blob, dataOff, data, 0, size);
                raw.Add(new Member { Name = name, Data = data, Offset = dataOff });
                pos = dataOff + size + (size & 1);              // members are 2-byte aligned
            }

            for (int i = 0; i < raw.Count; i++)
            {
                if (i == 2 && raw[i].Name == "//") { longnamesBox[0] = raw[i].Data; continue; }
                yield return raw[i];
            }
        }

        // Resolve a member's real name: "/123" indexes the longnames member.
        private static string MemberName(string name, byte[] longnames)
        {
            if (name.Length > 1 && name[0] == '/' && AllDigits(name, 1))
            {
                int start = int.Parse(name.Substring(1));
                int end = IndexOfZero(longnames, start);
                return Encoding.ASCII.GetString(longnames, start, end - start);
            }
            return name.TrimEnd('/');
        }

        private static bool AllDigits(string s, int from)
        {
            if (from >= s.Length) return false;
            for (int i = from; i < s.Length; i++) if (s[i] < '0' || s[i] > '9') return false;
            return true;
        }

        // ---- PPC COFF object ------------------------------------------------

        private struct Reloc { public uint Va; public uint Sym; public ushort Typ; }

        private sealed class Section
        {
            public string Name;
            public uint Vsize, Vaddr, Rawsize, Rawptr, Relptr, Flags;
            public ushort Nreloc;
            public byte[] Data;
            public Reloc[] Relocs;
        }

        private sealed class Symbol
        {
            public string Name;
            public int Value;
            public int Secnum;
            public ushort Type;
            public byte Cls;
            public byte Naux;
            public byte[][] Aux;
        }

        private sealed class CoffObject
        {
            public byte[] Blob;
            public ushort Machine, Nsections, Optsize, Characteristics;
            public uint Symptr, Nsymbols;
            public List<Section> Sections = new List<Section>();
            public List<Symbol> Symbols = new List<Symbol>();
            // (COFF index, Symbol), skipping the aux records that follow each.
            public List<KeyValuePair<int, Symbol>> EnumSyms = new List<KeyValuePair<int, Symbol>>();

            public CoffObject(byte[] blob)
            {
                Blob = blob;
                Machine = U16LE(blob, 0);
                Nsections = U16LE(blob, 2);
                // timestamp @4
                Symptr = U32LE(blob, 8);
                Nsymbols = U32LE(blob, 12);
                Optsize = U16LE(blob, 16);
                Characteristics = U16LE(blob, 18);

                int off = 20 + Optsize;
                for (int n = 0; n < Nsections; n++)
                {
                    var s = new Section();
                    s.Name = SectionName(blob, off);
                    s.Vsize = U32LE(blob, off + 8);
                    s.Vaddr = U32LE(blob, off + 12);
                    s.Rawsize = U32LE(blob, off + 16);
                    s.Rawptr = U32LE(blob, off + 20);
                    s.Relptr = U32LE(blob, off + 24);
                    // lineptr @28
                    s.Nreloc = U16LE(blob, off + 32);
                    // nline @34
                    s.Flags = U32LE(blob, off + 36);
                    if (s.Rawptr != 0)
                    {
                        s.Data = new byte[s.Rawsize];
                        Array.Copy(blob, (int)s.Rawptr, s.Data, 0, (int)s.Rawsize);
                    }
                    else s.Data = new byte[0];
                    s.Relocs = ReadRelocs(s);
                    Sections.Add(s);
                    off += 40;
                }

                ReadSymbols();
                int idx = 0;
                foreach (var sym in Symbols)
                {
                    EnumSyms.Add(new KeyValuePair<int, Symbol>(idx, sym));
                    idx += 1 + sym.Naux;
                }
            }

            // string table follows the symbol table; name offsets measure from it
            private int StrTabBase() { return (int)(Symptr + Nsymbols * 18); }

            private string SectionName(byte[] blob, int off)
            {
                if (blob[off] == (byte)'/')                    // "/123" -> strtab offset
                {
                    int end = off + 1;
                    while (end < off + 8 && blob[end] != 0) end++;
                    int offset = int.Parse(Encoding.ASCII.GetString(blob, off + 1, end - (off + 1)));
                    int b = StrTabBase();
                    int z = IndexOfZero(blob, b + offset);
                    return Encoding.ASCII.GetString(blob, b + offset, z - (b + offset));
                }
                int e = off;
                while (e < off + 8 && blob[e] != 0) e++;
                return Encoding.ASCII.GetString(blob, off, e - off);
            }

            private Reloc[] ReadRelocs(Section s)
            {
                var outr = new Reloc[s.Nreloc];
                for (int i = 0; i < s.Nreloc; i++)
                {
                    int o = (int)s.Relptr + i * 10;
                    outr[i].Va = U32LE(Blob, o);
                    outr[i].Sym = U32LE(Blob, o + 4);
                    outr[i].Typ = U16LE(Blob, o + 8);
                }
                return outr;
            }

            private string SymName(int o)
            {
                if (Blob[o] == 0 && Blob[o + 1] == 0 && Blob[o + 2] == 0 && Blob[o + 3] == 0)
                {
                    int offset = (int)U32LE(Blob, o + 4);
                    int b = StrTabBase();
                    int z = IndexOfZero(Blob, b + offset);
                    return Encoding.ASCII.GetString(Blob, b + offset, z - (b + offset));
                }
                int e = o;
                while (e < o + 8 && Blob[e] != 0) e++;
                return Encoding.ASCII.GetString(Blob, o, e - o);
            }

            private void ReadSymbols()
            {
                int i = 0;
                while (i < Nsymbols)
                {
                    int o = (int)Symptr + i * 18;
                    var sym = new Symbol();
                    sym.Name = SymName(o);
                    sym.Value = I32LE(Blob, o + 8);
                    sym.Secnum = U16LE(Blob, o + 12);
                    sym.Type = U16LE(Blob, o + 14);
                    sym.Cls = Blob[o + 16];
                    sym.Naux = Blob[o + 17];
                    sym.Aux = new byte[sym.Naux][];
                    for (int k = 0; k < sym.Naux; k++)
                    {
                        var a = new byte[18];
                        Array.Copy(Blob, o + 18 + k * 18, a, 0, 18);
                        sym.Aux[k] = a;
                    }
                    Symbols.Add(sym);
                    i += 1 + sym.Naux;
                }
            }
        }

        // ---- section keep rule ----------------------------------------------

        // The ELF section a COFF section is carried into, or null to drop it.
        private static string KeepSection(string name, uint flags)
        {
            if (name == ".rdata" || name.StartsWith(".rdata$")) return ".rodata";
            if (name.StartsWith(".CRT$")) return name;
            string bas = name.Split('$')[0];
            if (bas == ".debug" || bas == ".drectve" || bas == ".idata" || bas == ".didat"
                || bas == ".edata" || bas == ".rsrc" || bas == ".sxdata" || bas == ".gfids"
                || bas == ".giats" || bas == ".gljmp")
                return null;
            if (name == ".XBLD$W") return null;
            if ((flags & (IMAGE_SCN_LNK_INFO | IMAGE_SCN_LNK_REMOVE)) != 0) return null;
            if (bas == ".text" || bas == ".data" || bas == ".bss" || bas == ".pdata" || bas == ".xdata")
                return bas;
            if ((flags & IMAGE_SCN_CNT_CODE) != 0) return ".text";
            if ((flags & IMAGE_SCN_CNT_UNINITIALIZED_DATA) != 0) return ".bss";
            if ((flags & IMAGE_SCN_CNT_INITIALIZED_DATA) != 0)
                return (flags & IMAGE_SCN_MEM_WRITE) != 0 ? ".data" : ".rodata";
            return null;
        }

        // ---- string table ---------------------------------------------------

        private sealed class StrTab
        {
            public List<byte> Buf = new List<byte> { 0 };
            public Dictionary<string, int> Offsets = new Dictionary<string, int>(StringComparer.Ordinal) { { "", 0 } };

            public int Add(string s)
            {
                int existing;
                if (Offsets.TryGetValue(s, out existing)) return existing;
                int off = Buf.Count;
                Offsets[s] = off;
                Buf.AddRange(Encoding.UTF8.GetBytes(s));
                Buf.Add(0);
                return off;
            }
        }

        // ---- per-archive strong sets ----------------------------------------

        // Names this object defines STRONG in a non-COMDAT section.
        private static void StrongNoncomdatGlobals(CoffObject obj, HashSet<string> outSet)
        {
            foreach (var kv in obj.EnumSyms)
            {
                var sym = kv.Value;
                if (sym.Cls == IMAGE_SYM_CLASS_EXTERNAL && sym.Secnum > 0 && sym.Secnum <= obj.Sections.Count
                    && (obj.Sections[sym.Secnum - 1].Flags & IMAGE_SCN_LNK_COMDAT) == 0)
                    outSet.Add(sym.Name);
            }
        }

        private static bool KeepCodeSignatureStrong(string signame, HashSet<string> externalStrong)
        {
            return externalStrong.Contains(signame)
                || signame.StartsWith("??_E") || signame.StartsWith("??_G");
        }

        // ---- ELF symbol / section models ------------------------------------

        private struct ElfSym
        {
            public uint NameOff, Value, Size;
            public byte Info, Other;
            public ushort Shndx;
        }

        private struct RelaEntry { public uint Off; public uint Sym; public uint Type; public uint Addend; }

        private sealed class Group { public int Sig; public List<int> Members = new List<int>(); public string Signame; }

        private sealed class ElfSec
        {
            public string Name;
            public uint Typ, Flags, Link, Info, Align, Entsize;
            public byte[] Data;
            public int Size;
            public int Offset;
            public int NameOff;

            public ElfSec(string name, uint typ, uint flags = 0, byte[] data = null, int size = -1,
                          uint link = 0, uint info = 0, uint align = 1, uint entsize = 0)
            {
                Name = name;
                Typ = typ;
                Flags = flags;
                Data = data ?? new byte[0];
                Size = size < 0 ? Data.Length : size;
                Link = link;
                Info = info;
                Align = align;
                Entsize = entsize;
            }
        }

        // ---- coff_to_elf ----------------------------------------------------

        private static byte[] CoffToElf(CoffObject obj, HashSet<string> noncomdatStrong, HashSet<string> externalStrong, bool elf64)
        {
            // 1. decide which COFF sections survive, in order, ELF indices assigned.
            var kept = new List<Section>();                    // kept sections, in order
            var keptElfName = new List<string>();
            var keptCoffIdx = new List<int>();                 // 1-based COFF secnum of each kept
            var coffToElfShndx = new Dictionary<int, int>();   // 1-based COFF secnum -> ELF section index
            for (int ci = 1; ci <= obj.Sections.Count; ci++)
            {
                var s = obj.Sections[ci - 1];
                string elfName = KeepSection(s.Name, s.Flags);
                if (elfName == null) continue;
                coffToElfShndx[ci] = 1 + kept.Count;
                kept.Add(s);
                keptElfName.Add(elfName);
                keptCoffIdx.Add(ci);
            }

            // 1b. COMDAT selection per kept COMDAT section (Auxiliary Format 5).
            var secSelection = new Dictionary<int, KeyValuePair<byte, int>>();  // coff secnum -> (selection, assoc number)
            var secSelOrder = new List<int>();                 // preserve insertion order
            foreach (var kv in obj.EnumSyms)
            {
                var sym = kv.Value;
                if (sym.Cls == IMAGE_SYM_CLASS_STATIC && sym.Naux >= 1
                    && coffToElfShndx.ContainsKey(sym.Secnum)
                    && (obj.Sections[sym.Secnum - 1].Flags & IMAGE_SCN_LNK_COMDAT) != 0)
                {
                    byte[] aux = sym.Aux[0];
                    if (aux.Length >= 15 && !secSelection.ContainsKey(sym.Secnum))
                    {
                        int number = U16LE(aux, 12);
                        secSelection[sym.Secnum] = new KeyValuePair<byte, int>(aux[14], number);
                        secSelOrder.Add(sym.Secnum);
                    }
                }
            }

            // 2. symbols. locals first (a STT_SECTION per kept section, then local
            //    COFF symbols), then globals.
            var strtab = new StrTab();
            var elfSyms = new List<ElfSym>();
            elfSyms.Add(new ElfSym { Shndx = SHN_UNDEF });     // index 0 null symbol

            var sectionSymOf = new Dictionary<int, int>();     // ELF section index -> its STT_SECTION sym
            for (int elfIdx = 1; elfIdx <= kept.Count; elfIdx++)
            {
                sectionSymOf[elfIdx] = elfSyms.Count;
                elfSyms.Add(new ElfSym { Info = (byte)((STB_LOCAL << 4) | STT_SECTION), Shndx = (ushort)elfIdx });
            }

            // COFF index -> Symbol (for weak-external TagIndex), and names defined
            // in a kept section of this object.
            var coffByIndex = new Dictionary<int, Symbol>();
            foreach (var kv in obj.EnumSyms) coffByIndex[kv.Key] = kv.Value;
            var definedHere = new Dictionary<string, Symbol>(StringComparer.Ordinal);
            foreach (var kv in obj.EnumSyms)
                if (coffToElfShndx.ContainsKey(kv.Value.Secnum) && !definedHere.ContainsKey(kv.Value.Name))
                    definedHere[kv.Value.Name] = kv.Value;

            var coffToElfSym = new Dictionary<int, int>();     // COFF sym index -> ELF sym index
            for (int pass = 0; pass < 2; pass++)
            {
                bool passGlobals = pass == 1;
                foreach (var kv in obj.EnumSyms)
                {
                    int csi = kv.Key;
                    Symbol sym = kv.Value;
                    bool isWeakExt = sym.Cls == IMAGE_SYM_CLASS_WEAK_EXTERNAL;
                    bool isGlobal = sym.Cls == IMAGE_SYM_CLASS_EXTERNAL || isWeakExt;
                    if (isGlobal != passGlobals) continue;

                    // a STATIC symbol naming a kept section -> fold onto section symbol
                    if (sym.Cls == IMAGE_SYM_CLASS_STATIC && coffToElfShndx.ContainsKey(sym.Secnum)
                        && KeepSection(sym.Name, obj.Sections[sym.Secnum - 1].Flags) != null
                        && Split1(sym.Name) == Split1(obj.Sections[sym.Secnum - 1].Name))
                    {
                        coffToElfSym[csi] = sectionSymOf[coffToElfShndx[sym.Secnum]];
                        continue;
                    }
                    // a section symbol for a dropped section -> leave out
                    if (sym.Cls == IMAGE_SYM_CLASS_STATIC && sym.Name.StartsWith(".")
                        && !coffToElfShndx.ContainsKey(sym.Secnum))
                        continue;

                    byte bind = isGlobal ? STB_GLOBAL : STB_LOCAL;
                    if (isWeakExt) bind = STB_WEAK;
                    uint size = 0;
                    ushort shndx;
                    uint value;
                    byte styp;
                    if (sym.Secnum == 0)                        // undefined / common / weak
                    {
                        shndx = SHN_UNDEF; value = 0; styp = STT_NOTYPE;
                        if (sym.Cls == IMAGE_SYM_CLASS_EXTERNAL && sym.Value > 0)
                        {
                            size = (uint)sym.Value;
                            uint align = 1;
                            while (align * 2 <= size && align < 16) align *= 2;
                            shndx = SHN_COMMON; value = align; styp = STT_OBJECT;
                        }
                        if (isWeakExt && sym.Naux >= 1 && sym.Aux[0].Length >= 4)
                        {
                            int tagidx = (int)U32LE(sym.Aux[0], 0);
                            Symbol dflt;
                            coffByIndex.TryGetValue(tagidx, out dflt);
                            Symbol target = null;
                            if (dflt != null) definedHere.TryGetValue(dflt.Name, out target);
                            if (target != null)
                            {
                                uint tflags = obj.Sections[target.Secnum - 1].Flags;
                                shndx = (ushort)coffToElfShndx[target.Secnum];
                                value = (uint)target.Value;
                                styp = (tflags & IMAGE_SCN_MEM_EXECUTE) != 0 ? STT_FUNC : STT_OBJECT;
                            }
                        }
                    }
                    else if (sym.Secnum == 0xFFFF)             // 0xFFFFFFFF cannot occur for ushort
                    {
                        shndx = SHN_ABS; value = (uint)sym.Value; styp = STT_NOTYPE;
                    }
                    else if (coffToElfShndx.ContainsKey(sym.Secnum))
                    {
                        shndx = (ushort)coffToElfShndx[sym.Secnum];
                        value = (uint)sym.Value;
                        uint sflags = obj.Sections[sym.Secnum - 1].Flags;
                        styp = (sflags & IMAGE_SCN_MEM_EXECUTE) != 0 ? STT_FUNC : STT_OBJECT;
                        if (isGlobal && (sflags & IMAGE_SCN_LNK_COMDAT) != 0
                            && !secSelection.ContainsKey(sym.Secnum))
                            bind = STB_WEAK;
                    }
                    else
                    {
                        shndx = SHN_UNDEF; value = 0; styp = STT_NOTYPE;
                    }

                    coffToElfSym[csi] = elfSyms.Count;
                    elfSyms.Add(new ElfSym
                    {
                        NameOff = (uint)strtab.Add(sym.Name),
                        Value = value,
                        Size = size,
                        Info = (byte)((bind << 4) | styp),
                        Other = 0,
                        Shndx = shndx,
                    });
                }
            }

            // .symtab sh_info = index of first non-local symbol (weak counts as non-local).
            int firstGlobal = elfSyms.Count;
            for (int i = 0; i < elfSyms.Count; i++)
                if ((elfSyms[i].Info >> 4) != STB_LOCAL) { firstGlobal = i; break; }

            // 3. relocations per kept section
            var relaList = new List<KeyValuePair<int, List<RelaEntry>>>();  // elf section index -> entries (insertion order)
            for (int elfIdx = 1; elfIdx <= kept.Count; elfIdx++)
            {
                Section s = kept[elfIdx - 1];
                var entries = new List<RelaEntry>();
                int i = 0;
                while (i < s.Relocs.Length)
                {
                    uint va = s.Relocs[i].Va;
                    uint symidx = s.Relocs[i].Sym;
                    ushort typ = s.Relocs[i].Typ;
                    int tt = typ & 0xFF;
                    if (tt == 0x0B || tt == 0x0C) { i++; continue; }   // SECREL/SECTION -> drop
                    if (tt == 0x0A) { i++; continue; }                 // ADDR32NB -> import manifest
                    int elfsym;
                    if (!coffToElfSym.TryGetValue((int)symidx, out elfsym)) { i++; continue; }
                    uint inplace = (va + 4 <= (uint)s.Data.Length) ? U32BE(s.Data, (int)va) : 0;
                    if (tt == 0x02)                                    // ADDR32
                    {
                        entries.Add(new RelaEntry { Off = va, Sym = (uint)elfsym, Type = R_PPC_ADDR32, Addend = inplace });
                    }
                    else if (tt == 0x06)                               // REL24
                    {
                        long li = inplace & 0x03FFFFFC;
                        if ((li & 0x02000000) != 0) li -= 0x04000000;
                        long addend = li + va;
                        entries.Add(new RelaEntry { Off = va, Sym = (uint)elfsym, Type = R_PPC_REL24, Addend = (uint)addend });
                    }
                    else if (tt == 0x10 || tt == 0x11)                 // REFHI / REFLO (+ PAIR)
                    {
                        long pairLow = 0;
                        if (i + 1 < s.Relocs.Length && (s.Relocs[i + 1].Typ & 0xFF) == 0x12)
                        {
                            pairLow = s.Relocs[i + 1].Sym;
                            i++;
                        }
                        long imm = inplace & 0xFFFF;
                        long addend = (tt == 0x10)
                            ? ((imm << 16) | (pairLow & 0xFFFF))
                            : ((pairLow << 16) | imm);
                        if ((addend & 0x80000000L) != 0) addend -= 0x100000000L;
                        uint rtype = (tt == 0x10) ? R_PPC_ADDR16_HA : R_PPC_ADDR16_LO;
                        // PPC ELF 16-bit relocs point two bytes into the big-endian insn.
                        entries.Add(new RelaEntry { Off = va + 2, Sym = (uint)elfsym, Type = rtype, Addend = (uint)addend });
                    }
                    i++;
                }
                if (entries.Count > 0)
                    relaList.Add(new KeyValuePair<int, List<RelaEntry>>(elfIdx, entries));
            }

            // 4. COMDAT section groups.
            var secSig = new Dictionary<int, int>();           // coff secnum -> elf sym index (signature)
            var secSigName = new Dictionary<int, string>();
            foreach (var kv in obj.EnumSyms)
            {
                Symbol sym = kv.Value;
                if (sym.Cls == IMAGE_SYM_CLASS_EXTERNAL && secSelection.ContainsKey(sym.Secnum)
                    && coffToElfSym.ContainsKey(kv.Key))
                {
                    if (!secSig.ContainsKey(sym.Secnum))
                    {
                        secSig[sym.Secnum] = coffToElfSym[kv.Key];
                        secSigName[sym.Secnum] = sym.Name;
                    }
                }
            }

            var groupsOrder = new List<int>();                 // root elf_shndx, insertion order
            var groupsByRoot = new Dictionary<int, Group>();
            foreach (int sn in secSelOrder)
            {
                int root = ComdatRoot(sn, secSelection, new HashSet<int>());
                int sig;
                if (!secSig.TryGetValue(root, out sig)) continue;   // no external signature -> weak above
                int rElf = coffToElfShndx[root];
                Group g;
                if (!groupsByRoot.TryGetValue(rElf, out g))
                {
                    string signame;
                    secSigName.TryGetValue(root, out signame);
                    g = new Group { Sig = sig, Signame = signame ?? "" };
                    groupsByRoot[rElf] = g;
                    groupsOrder.Add(rElf);
                }
                g.Members.Add(coffToElfShndx[sn]);
            }

            // COMDAT binding: keep one STRONG signature per CODE group (see the
            // Python for the full rationale); weaken every other COMDAT global.
            var elfFlags = new Dictionary<int, uint>();        // elf shndx -> coff flags
            for (int k = 0; k < kept.Count; k++) elfFlags[coffToElfShndx[keptCoffIdx[k]]] = kept[k].Flags;
            var signatures = new HashSet<int>();
            foreach (int rElf in groupsOrder)
            {
                Group g = groupsByRoot[rElf];
                uint fl; elfFlags.TryGetValue(rElf, out fl);
                if (!noncomdatStrong.Contains(g.Signame) && (fl & IMAGE_SCN_MEM_EXECUTE) != 0
                    && KeepCodeSignatureStrong(g.Signame, externalStrong))
                    signatures.Add(g.Sig);
            }
            var comdatKept = new HashSet<int>();
            for (int k = 0; k < kept.Count; k++)
                if ((kept[k].Flags & IMAGE_SCN_LNK_COMDAT) != 0)
                    comdatKept.Add(coffToElfShndx[keptCoffIdx[k]]);
            for (int k = 0; k < elfSyms.Count; k++)
            {
                ElfSym e = elfSyms[k];
                if ((e.Info >> 4) == STB_GLOBAL && comdatKept.Contains(e.Shndx) && !signatures.Contains(k))
                {
                    e.Info = (byte)((STB_WEAK << 4) | (e.Info & 0xF));
                    elfSyms[k] = e;
                }
            }

            return WriteElf(kept, keptElfName, keptCoffIdx, coffToElfShndx, elfSyms, strtab,
                            firstGlobal, relaList, groupsOrder, groupsByRoot, elf64);
        }

        private static string Split1(string s)
        {
            int i = s.IndexOf('$');
            return i < 0 ? s : s.Substring(0, i);
        }

        private static int ComdatRoot(int sn, Dictionary<int, KeyValuePair<byte, int>> secSelection, HashSet<int> seen)
        {
            KeyValuePair<byte, int> v;
            if (!secSelection.TryGetValue(sn, out v)) return sn;
            byte sel = v.Key; int num = v.Value;
            if (sel == IMAGE_COMDAT_SELECT_ASSOCIATIVE && num != 0 && num != sn
                && !seen.Contains(num) && secSelection.ContainsKey(num))
            {
                seen.Add(sn);
                return ComdatRoot(num, secSelection, seen);
            }
            return sn;
        }

        // ---- defined globals (archive index) --------------------------------

        private static List<string> DefinedGlobals(CoffObject obj)
        {
            var kept = new HashSet<int>();
            for (int ci = 1; ci <= obj.Sections.Count; ci++)
                if (KeepSection(obj.Sections[ci - 1].Name, obj.Sections[ci - 1].Flags) != null)
                    kept.Add(ci);
            var outl = new List<string>();
            foreach (var kv in obj.EnumSyms)
            {
                Symbol sym = kv.Value;
                if (sym.Cls == IMAGE_SYM_CLASS_EXTERNAL && kept.Contains(sym.Secnum))
                    outl.Add(sym.Name);
                else if (sym.Cls == IMAGE_SYM_CLASS_EXTERNAL && sym.Secnum == 0 && sym.Value > 0)
                    outl.Add(sym.Name);
            }
            return outl;
        }

        // ---- ELF32-BE writer ------------------------------------------------

        private static byte[] WriteElf(List<Section> kept, List<string> keptElfName, List<int> keptCoffIdx,
            Dictionary<int, int> coffToElfShndx, List<ElfSym> elfSyms, StrTab strtab, int firstGlobal,
            List<KeyValuePair<int, List<RelaEntry>>> relaList, List<int> groupsOrder,
            Dictionary<int, Group> groupsByRoot, bool elf64)
        {
            // kept elf_idx (1-based) -> group root elf_idx (SHF_GROUP membership)
            var memberRoot = new Dictionary<int, int>();
            var memberRootOrder = new List<int>();
            foreach (int root in groupsOrder)
                foreach (int m in groupsByRoot[root].Members)
                {
                    memberRoot[m] = root;
                    memberRootOrder.Add(m);
                }

            var secs = new List<ElfSec>();
            secs.Add(new ElfSec("", 0));                       // null section

            var keptElfIndex = new Dictionary<int, int>();     // kept elf_idx (1-based) -> secs[] index
            for (int elfIdx = 1; elfIdx <= kept.Count; elfIdx++)
            {
                Section s = kept[elfIdx - 1];
                uint flags = SHF_ALLOC;
                if ((s.Flags & IMAGE_SCN_MEM_WRITE) != 0) flags |= SHF_WRITE;
                if ((s.Flags & IMAGE_SCN_MEM_EXECUTE) != 0) flags |= SHF_EXECINSTR;
                if (memberRoot.ContainsKey(elfIdx)) flags |= SHF_GROUP;
                uint nalign = (s.Flags >> 20) & 0xF;
                uint align = nalign != 0 ? (uint)(1 << (int)(nalign - 1)) : 4;
                keptElfIndex[elfIdx] = secs.Count;
                if ((s.Flags & IMAGE_SCN_CNT_UNINITIALIZED_DATA) != 0)
                    secs.Add(new ElfSec(keptElfName[elfIdx - 1], SHT_NOBITS, flags, new byte[0],
                        size: (int)(s.Vsize != 0 ? s.Vsize : s.Rawsize), align: align));
                else
                    secs.Add(new ElfSec(keptElfName[elfIdx - 1], SHT_PROGBITS, flags, s.Data, align: align));
            }

            // .symtab -- Elf32_Sym is name,value,size,info,other,shndx (16 bytes);
            // Elf64_Sym reorders to name,info,other,shndx,value,size (24 bytes).
            uint shAlign = elf64 ? 8u : 4u;
            var symBytes = new List<byte>();
            foreach (ElfSym e in elfSyms)
            {
                if (elf64)
                {
                    PutU32BE(symBytes, e.NameOff);
                    symBytes.Add(e.Info);
                    symBytes.Add(e.Other);
                    PutU16BE(symBytes, e.Shndx);
                    PutU64BE(symBytes, e.Value);
                    PutU64BE(symBytes, e.Size);
                }
                else
                {
                    PutU32BE(symBytes, e.NameOff);
                    PutU32BE(symBytes, e.Value);
                    PutU32BE(symBytes, e.Size);
                    symBytes.Add(e.Info);
                    symBytes.Add(e.Other);
                    PutU16BE(symBytes, e.Shndx);
                }
            }
            int symtabIdx = secs.Count;
            secs.Add(new ElfSec(".symtab", SHT_SYMTAB, data: symBytes.ToArray(), link: (uint)(symtabIdx + 1),
                info: (uint)firstGlobal, align: shAlign, entsize: elf64 ? 24u : 16u));
            secs.Add(new ElfSec(".strtab", SHT_STRTAB, data: strtab.Buf.ToArray(), align: 1));

            // group root elf_idx -> secs[] indices that are its members
            var groupSecidx = new Dictionary<int, List<int>>();
            foreach (int root in groupsOrder) groupSecidx[root] = new List<int>();
            foreach (int m in memberRootOrder) groupSecidx[memberRoot[m]].Add(keptElfIndex[m]);

            foreach (var kv in relaList)
            {
                int elfIdx = kv.Key;
                int target = keptElfIndex[elfIdx];
                uint rflags = memberRoot.ContainsKey(elfIdx) ? SHF_GROUP : 0;
                var blob = new List<byte>();
                foreach (RelaEntry r in kv.Value)
                {
                    if (elf64)
                    {
                        PutU64BE(blob, r.Off);
                        PutU64BE(blob, ((ulong)r.Sym << 32) | r.Type);
                        PutU64BE(blob, unchecked((ulong)(long)(int)r.Addend));   // sign-extend
                    }
                    else
                    {
                        PutU32BE(blob, r.Off);
                        PutU32BE(blob, (r.Sym << 8) | r.Type);
                        PutU32BE(blob, r.Addend);
                    }
                }
                secs.Add(new ElfSec(".rela" + secs[target].Name, SHT_RELA, data: blob.ToArray(),
                    link: (uint)symtabIdx, info: (uint)target, align: shAlign, entsize: elf64 ? 24u : 12u, flags: rflags));
                if (memberRoot.ContainsKey(elfIdx))
                    groupSecidx[memberRoot[elfIdx]].Add(secs.Count - 1);
            }

            // one SHT_GROUP section per COMDAT group
            foreach (int root in groupsOrder)
            {
                Group g = groupsByRoot[root];
                var blob = new List<byte>();
                PutU32BE(blob, GRP_COMDAT);
                foreach (int m in groupSecidx[root]) PutU32BE(blob, (uint)m);
                secs.Add(new ElfSec(".group", SHT_GROUP, data: blob.ToArray(),
                    link: (uint)symtabIdx, info: (uint)g.Sig, align: 4, entsize: 4));
            }

            var shstr = new StrTab();
            foreach (ElfSec sec in secs) sec.NameOff = shstr.Add(sec.Name);
            int shstrtabIdx = secs.Count;
            var shstrSec = new ElfSec(".shstrtab", SHT_STRTAB, align: 1);
            shstrSec.NameOff = shstr.Add(".shstrtab");
            shstrSec.Data = shstr.Buf.ToArray();
            shstrSec.Size = shstrSec.Data.Length;
            secs.Add(shstrSec);

            // layout: ELF header (52/64), each non-empty section (aligned), section headers.
            int ehsize = elf64 ? 64 : 52;
            int shentsize = elf64 ? 64 : 40;
            int alignEnd = elf64 ? 8 : 4;
            int offset = ehsize;
            foreach (ElfSec sec in secs)
            {
                if (sec.Typ == 0 || sec.Typ == SHT_NOBITS) { sec.Offset = 0; continue; }
                if (sec.Align > 1 && offset % sec.Align != 0)
                    offset += (int)(sec.Align - (offset % sec.Align));
                sec.Offset = offset;
                offset += sec.Data.Length;
            }
            if (offset % alignEnd != 0) offset += alignEnd - (offset % alignEnd);
            int shoff = offset;

            var outb = new List<byte>();
            outb.AddRange(new byte[] { 0x7F, (byte)'E', (byte)'L', (byte)'F', (byte)(elf64 ? 2 : 1), 2, 1, 0 });
            outb.AddRange(new byte[8]);                        // e_ident padding
            PutU16BE(outb, ET_REL);
            PutU16BE(outb, elf64 ? EM_PPC64 : EM_PPC);
            PutU32BE(outb, 1);                                 // e_version
            if (elf64)
            {
                PutU64BE(outb, 0);                             // e_entry
                PutU64BE(outb, 0);                             // e_phoff
                PutU64BE(outb, (ulong)shoff);                  // e_shoff
                PutU32BE(outb, EF_PPC64_ABI_V2);               // e_flags
            }
            else
            {
                PutU32BE(outb, 0);                             // e_entry
                PutU32BE(outb, 0);                             // e_phoff
                PutU32BE(outb, (uint)shoff);                   // e_shoff
                PutU32BE(outb, 0);                             // e_flags
            }
            PutU16BE(outb, (ushort)ehsize);                    // e_ehsize
            PutU16BE(outb, 0);                                 // e_phentsize
            PutU16BE(outb, 0);                                 // e_phnum
            PutU16BE(outb, (ushort)shentsize);                 // e_shentsize
            PutU16BE(outb, (ushort)secs.Count);                // e_shnum
            PutU16BE(outb, (ushort)shstrtabIdx);               // e_shstrndx
            while (outb.Count < ehsize) outb.Add(0);

            foreach (ElfSec sec in secs)
            {
                if (sec.Typ == 0 || sec.Typ == SHT_NOBITS || sec.Data.Length == 0) continue;
                while (outb.Count < sec.Offset) outb.Add(0);
                outb.AddRange(sec.Data);
            }
            while (outb.Count < shoff) outb.Add(0);

            foreach (ElfSec sec in secs)
            {
                if (elf64)
                {
                    // Elf64_Shdr: name,type, flags(8),addr(8),offset(8),size(8), link,info, align(8),entsize(8).
                    PutU32BE(outb, (uint)sec.NameOff);
                    PutU32BE(outb, sec.Typ);
                    PutU64BE(outb, sec.Flags);
                    PutU64BE(outb, 0);                         // sh_addr
                    PutU64BE(outb, (ulong)sec.Offset);
                    PutU64BE(outb, (ulong)sec.Size);
                    PutU32BE(outb, sec.Link);
                    PutU32BE(outb, sec.Info);
                    PutU64BE(outb, sec.Align);
                    PutU64BE(outb, sec.Entsize);
                }
                else
                {
                    PutU32BE(outb, (uint)sec.NameOff);
                    PutU32BE(outb, sec.Typ);
                    PutU32BE(outb, sec.Flags);
                    PutU32BE(outb, 0);                         // sh_addr
                    PutU32BE(outb, (uint)sec.Offset);
                    PutU32BE(outb, (uint)sec.Size);
                    PutU32BE(outb, sec.Link);
                    PutU32BE(outb, sec.Info);
                    PutU32BE(outb, sec.Align);
                    PutU32BE(outb, sec.Entsize);
                }
            }
            return outb.ToArray();
        }

        // ---- System V archive (.a) ------------------------------------------

        private struct ArMember { public string Name; public byte[] Data; public List<string> Defs; }

        private static byte[] ArHeader(string name, int size)
        {
            string h = Pad(name, 16) + Pad("0", 12) + Pad("0", 6) + Pad("0", 6) + Pad("0", 8) + Pad(size.ToString(), 10) + "`\n";
            return Encoding.ASCII.GetBytes(h);
        }

        private static string Pad(string s, int width) { return s.Length >= width ? s : s + new string(' ', width - s.Length); }

        private static int Padded(int n) { return n + (n & 1); }

        private static int WriteArchive(List<ArMember> members, string path)
        {
            // long member names -> "//" table; short names stored inline as "name/"
            var longnames = new List<byte>();
            var storedNames = new List<string>();
            foreach (ArMember m in members)
            {
                string shortName = m.Name + "/";
                if (shortName.Length <= 16)
                    storedNames.Add(shortName);
                else
                {
                    storedNames.Add("/" + longnames.Count);
                    longnames.AddRange(Encoding.ASCII.GetBytes(m.Name + "/\n"));
                }
            }

            // symbol index: 4-byte BE count, count*4 BE member offsets, then names.
            var symbols = new List<KeyValuePair<string, int>>();   // (symbol, member index)
            for (int mi = 0; mi < members.Count; mi++)
                foreach (string sym in members[mi].Defs)
                    symbols.Add(new KeyValuePair<string, int>(sym, mi));
            var indexNames = new List<byte>();
            foreach (var s in symbols) { indexNames.AddRange(Encoding.ASCII.GetBytes(s.Key)); indexNames.Add(0); }
            int indexSize = 4 + 4 * symbols.Count + indexNames.Count;

            int off = ArchiveMagic.Length;
            off += 60 + Padded(indexSize);                     // "/" symbol table member
            int longnamesSize = longnames.Count;
            if (longnamesSize != 0) off += 60 + Padded(longnamesSize);   // "//" longnames member

            var memberOffsets = new int[members.Count];
            int cur = off;
            for (int mi = 0; mi < members.Count; mi++)
            {
                memberOffsets[mi] = cur;
                cur += 60 + Padded(members[mi].Data.Length);
            }

            var outb = new List<byte>();
            outb.AddRange(ArchiveMagic);
            // symbol table member
            outb.AddRange(ArHeader("/", indexSize));
            PutU32BE(outb, (uint)symbols.Count);
            foreach (var s in symbols) PutU32BE(outb, (uint)memberOffsets[s.Value]);
            outb.AddRange(indexNames);
            if ((indexSize & 1) != 0) outb.Add((byte)'\n');
            // longnames member
            if (longnamesSize != 0)
            {
                outb.AddRange(ArHeader("//", longnamesSize));
                outb.AddRange(longnames);
                if ((longnamesSize & 1) != 0) outb.Add((byte)'\n');
            }
            // object members
            for (int mi = 0; mi < members.Count; mi++)
            {
                outb.AddRange(ArHeader(storedNames[mi], members[mi].Data.Length));
                outb.AddRange(members[mi].Data);
                if ((members[mi].Data.Length & 1) != 0) outb.Add((byte)'\n');
            }

            File.WriteAllBytes(path, outb.ToArray());
            return symbols.Count;
        }

        // ---- short-import parse (skip only) + runtime armap -----------------

        private static void ParseShortImport(byte[] data, out string sym, out string dll,
            out int ordhint, out int nameType, out int importType)
        {
            ordhint = U16LE(data, 16);
            int typebits = U16LE(data, 18);
            importType = typebits & 0x3;
            nameType = (typebits >> 2) & 0x7;
            int p = 20, e = IndexOfZero(data, p);
            sym = Encoding.ASCII.GetString(data, p, e - p);
            p = e + 1; e = IndexOfZero(data, Math.Min(p, data.Length));
            dll = Encoding.ASCII.GetString(data, Math.Min(p, data.Length), Math.Max(0, e - p));
        }

        // Defined external symbol names of a GNU/llvm-ar archive, from its armap.
        private static HashSet<string> RuntimeDefinedSymbols(string path)
        {
            var result = new HashSet<string>(StringComparer.Ordinal);
            byte[] blob;
            try { blob = File.ReadAllBytes(path); } catch { return result; }
            if (blob.Length < 8) return result;
            for (int i = 0; i < 8; i++) if (blob[i] != ArchiveMagic[i]) return result;
            int pos = 8;
            while (pos + 60 <= blob.Length)
            {
                string name = Encoding.ASCII.GetString(blob, pos, 16).TrimEnd();
                int size = int.Parse(Encoding.ASCII.GetString(blob, pos + 48, 10).Trim());
                int dataOff = pos + 60;
                pos = dataOff + size + (size & 1);
                if (name == "/")                               // GNU armap: BE count, offsets, names
                {
                    if (size < 4) return result;
                    uint n = U32BE(blob, dataOff);
                    int namesStart = dataOff + 4 + 4 * (int)n;
                    int p = namesStart;
                    int end = dataOff + size;
                    while (p < end)
                    {
                        int z = IndexOfZero(blob, p);
                        if (z > end) z = end;
                        if (z > p) result.Add(Encoding.GetEncoding("iso-8859-1").GetString(blob, p, z - p));
                        p = z + 1;
                    }
                    return result;
                }
                if (name.Length != 0 && name != "//") break;   // first real member, no armap
            }
            return result;
        }

        // ---- orchestration --------------------------------------------------

        /// <summary>
        /// Translate a whole PPC-COFF .lib to a PPC-BE ELF `.a` that lld links.
        /// elf64=true emits ELFCLASS64/EM_PPC64 ELFv2 (the powerpc64-unknown-xbox360
        /// toolchain, e_flags=0x2); elf64=false emits the legacy ELFCLASS32/EM_PPC.
        /// The runtime archives (libcpp.a, libc.a) supply the external-strong set.
        /// Returns the number of indexed symbols written to the archive index.
        /// </summary>
        public static int Translate(string libPath, string outA, string[] runtimeLibs, bool elf64 = true)
        {
            byte[] blob = File.ReadAllBytes(libPath);

            // First pass: every COFF object, plus the union of names each defines
            // strong out-of-line (non-COMDAT), and the runtime external-strong set.
            var externalStrong = new HashSet<string>(StringComparer.Ordinal);
            if (runtimeLibs != null)
                foreach (string rl in runtimeLibs)
                    externalStrong.UnionWith(RuntimeDefinedSymbols(rl));

            var coffMembers = new List<KeyValuePair<string, CoffObject>>();   // (raw member name, object)
            var noncomdatStrong = new HashSet<string>(StringComparer.Ordinal);
            var imports = new List<string[]>();                // manifest rows (best effort)

            var lnBox = new byte[1][];
            foreach (Member member in ReadArchive(blob, lnBox))
            {
                if (member.Name == "/" || member.Name == "//") continue;
                if (!(member.Data.Length >= 2 && member.Data[0] == (ImageFileMachinePowerPcBe & 0xFF)
                      && member.Data[1] == (ImageFileMachinePowerPcBe >> 8)))
                {
                    string sym, dll; int oh, nt, it;
                    ParseShortImport(member.Data, out sym, out dll, out oh, out nt, out it);
                    imports.Add(new[] { sym, dll, oh.ToString(), nt.ToString(), it.ToString() });
                    continue;
                }
                var obj = new CoffObject(member.Data);
                StrongNoncomdatGlobals(obj, noncomdatStrong);
                coffMembers.Add(new KeyValuePair<string, CoffObject>(MemberName(member.Name, lnBox[0]), obj));
            }

            // Second pass: translate each object with both strong sets, uniquify
            // member base names, and repack.
            var members = new List<ArMember>();
            var seen = new Dictionary<string, int>(StringComparer.Ordinal);
            foreach (var kv in coffMembers)
            {
                byte[] elf = CoffToElf(kv.Value, noncomdatStrong, externalStrong, elf64);
                string bas = kv.Key.Replace('\\', '/');
                int slash = bas.LastIndexOf('/');
                if (slash >= 0) bas = bas.Substring(slash + 1);
                if (bas.EndsWith(".obj")) bas = bas.Substring(0, bas.Length - 4);
                int n; seen.TryGetValue(bas, out n);
                seen[bas] = n + 1;
                string uniq = n != 0 ? (bas + "." + n + ".o") : (bas + ".o");
                members.Add(new ArMember { Name = uniq, Data = elf, Defs = DefinedGlobals(kv.Value) });
            }

            int nsyms = WriteArchive(members, outA);
            WriteManifest(libPath, outA, members.Count, nsyms, imports);
            return nsyms;
        }

        // The imports.json manifest (not needed for validation; produced when trivial).
        private static void WriteManifest(string libPath, string outA, int nobjects, int nsyms, List<string[]> imports)
        {
            try
            {
                string manifest = Path.ChangeExtension(outA, null);
                int dot = outA.LastIndexOf('.');
                manifest = (dot >= 0 ? outA.Substring(0, dot) : outA) + ".imports.json";
                var sb = new StringBuilder();
                sb.Append("{\n");
                sb.Append("  \"library\": ").Append(JsonStr(libPath)).Append(",\n");
                sb.Append("  \"objects\": ").Append(nobjects).Append(",\n");
                sb.Append("  \"index_symbols\": ").Append(nsyms).Append(",\n");
                sb.Append("  \"imports\": [");
                for (int i = 0; i < imports.Count; i++)
                {
                    string[] im = imports[i];
                    sb.Append(i == 0 ? "\n" : ",\n");
                    sb.Append("    {\n");
                    sb.Append("      \"symbol\": ").Append(JsonStr(im[0])).Append(",\n");
                    sb.Append("      \"dll\": ").Append(JsonStr(im[1])).Append(",\n");
                    sb.Append("      \"ordinal_or_hint\": ").Append(im[2]).Append(",\n");
                    sb.Append("      \"name_type\": ").Append(im[3]).Append(",\n");
                    sb.Append("      \"import_type\": ").Append(im[4]).Append("\n");
                    sb.Append("    }");
                }
                sb.Append(imports.Count > 0 ? "\n  ]\n" : "]\n");
                sb.Append("}");
                File.WriteAllText(manifest, sb.ToString(), new UTF8Encoding(false));
            }
            catch { /* manifest is best-effort */ }
        }

        private static string JsonStr(string s)
        {
            var sb = new StringBuilder("\"");
            foreach (char c in s)
            {
                if (c == '"' || c == '\\') sb.Append('\\').Append(c);
                else if (c == '\n') sb.Append("\\n");
                else if (c == '\r') sb.Append("\\r");
                else if (c == '\t') sb.Append("\\t");
                else sb.Append(c);
            }
            return sb.Append('"').ToString();
        }
    }
}
