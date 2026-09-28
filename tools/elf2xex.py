#!/usr/bin/env python3
"""Pack a linked PPC32 ELF executable into an Xbox 360 XEX2.

The last stage of the toolchain: once code is compiled to ELF and linked at a
XEX load address, this wraps the loadable image in the XEX2 container the 360
loader (and xenia) expects, and by default makes it loadable on a stock devkit.
A genuine kit is strict where xenia is lenient, so a loadable devkit XEX needs
all of: a debug signature (a zero signature is classified retail -> LDRX
C000007B), a debug-encrypted basefile (an unencrypted image is also rejected),
the CHECKSUM_TIMESTAMP optional header (absent -> C000007B), and the optional-
header directory sorted by key (the loader binary-searches it). The first two
come from tools/xex_debugsign.py; the last two are emitted here. Pass --no-sign
for a raw unsigned/unencrypted image, or --no-encrypt to sign but not encrypt
(loads in xenia, not on a stock devkit).

XEX is big-endian throughout.

Usage:
    python tools/elf2xex.py <input.elf> -o <output.xex> [--base 0x82000000]
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xex_debugsign  # noqa: E402  (sibling module; debug-signing the packed XEX)


PAGE = 0x1000

# optional-header keys (XexTool's XexImageEntryTypes.h)
KEY_BASEFILE_FORMAT = 0x000003FF
KEY_ENTRY_POINT = 0x00010100
KEY_IMAGE_BASE_ADDRESS = 0x00010201
KEY_IMPORT_LIBRARIES = 0x000103FF
KEY_ORIGINAL_BASE_ADDRESS = 0x00010001
KEY_CHECKSUM_TIMESTAMP = 0x00018002   # low byte 0x02 == 2 dwords (checksum, timestamp)
KEY_TLS_INFO = 0x00020104             # low byte 0x04 == 4 dwords (XEX_TLS_INFO)
KEY_STACK_SIZE = 0x00020200

# Thread-local-storage slots the loader gives each thread. A title with no
# TLS_INFO header gets zero slots, so any KeTlsGetValue/KeTlsSetValue (which the
# MS CRT and middleware like d3dx9's shader compiler use) indexes past the empty
# array and the kernel traps -- a hard hang with no exception. Real titles carry
# 64 (TLS_MINIMUM_AVAILABLE); we do the same. No static TLS data, just the slots.
DEFAULT_TLS_SLOTS = 64
KEY_EXECUTION_INFO = 0x00040006     # low byte 0x06 == 6 dwords (the 0x18 struct)

DEFAULT_STACK_SIZE = 0x40000        # what a real XDK title carries

# Default title id stamped into the execution-info header. A real title needs a
# non-zero id: emulators/loaders key content, caches and (in xenia) the F12
# screenshot folder off it, and a zero id makes xenia skip those paths entirely.
DEFAULT_TITLE_ID = 0x52584401       # 'RXD\x01' -- RXDK homebrew namespace

# module / image flags
MODULEFLAG_TITLE_MODULE = 0x00000001
MODULEFLAG_USER_MODE = 0x00000080

# section info types
SECTIONINFO_CODE = 1
SECTIONINFO_DATA = 2
SECTIONINFO_READONLY = 3


# ---- ELF big-endian reading (ELFCLASS32 and ELFCLASS64) ---------------------

# ELF section flags / types
SHF_ALLOC, SHF_WRITE, SHF_EXECINSTR = 0x2, 0x1, 0x4
SHT_NOBITS = 8


class ElfSection:
    __slots__ = ("name", "vaddr", "data", "memsize", "flags", "typ")


def _elf_geom(blob):
    """Parse a big-endian PPC ELF header, ELFCLASS32 or ELFCLASS64.

    The powerpc64-unknown-xbox360 toolchain links ELFCLASS64 objects (ILP32
    content in a 64-bit container), while the legacy powerpc-unknown-xbox360
    path links ELFCLASS32. Both produce the same 32-bit image; only the ELF
    header/section/symbol/program-header field widths differ. Returns a dict
    exposing e_entry/e_shnum/e_shstrndx and uniform reader closures shdr(i)
    (a 10-tuple name,type,flags,addr,off,size,link,info,align,entsize regardless
    of class), load_vaddrs() and sym(o) -> (st_name, st_value).
    """
    if blob[:4] != b"\x7fELF":
        sys.exit("not an ELF file")
    if blob[5] != 2:
        sys.exit("expected a big-endian ELF (PPC)")
    is64 = blob[4] == 2
    if is64:
        (e_entry,) = struct.unpack_from(">Q", blob, 0x18)
        (e_phoff,) = struct.unpack_from(">Q", blob, 0x20)
        (e_shoff,) = struct.unpack_from(">Q", blob, 0x28)
        e_phentsize, e_phnum = struct.unpack_from(">HH", blob, 0x36)
        e_shentsize, e_shnum, e_shstrndx = struct.unpack_from(">HHH", blob, 0x3A)

        def shdr(i):
            o = e_shoff + i * e_shentsize
            name, typ = struct.unpack_from(">II", blob, o)
            flags, addr, off, size = struct.unpack_from(">QQQQ", blob, o + 8)
            link, info = struct.unpack_from(">II", blob, o + 0x28)
            align, entsize = struct.unpack_from(">QQ", blob, o + 0x30)
            return (name, typ, flags, addr, off, size, link, info, align, entsize)

        def load_vaddrs():
            # Elf64_Phdr: type(4), flags(4), offset(8), vaddr(8), ...
            vs = []
            for i in range(e_phnum):
                o = e_phoff + i * e_phentsize
                (p_type,) = struct.unpack_from(">I", blob, o)
                (p_vaddr,) = struct.unpack_from(">Q", blob, o + 0x10)
                if p_type == 1:
                    vs.append(p_vaddr)
            return vs

        def sym(o):
            # Elf64_Sym: name(4), info(1), other(1), shndx(2), value(8), size(8)
            (st_name,) = struct.unpack_from(">I", blob, o)
            (st_value,) = struct.unpack_from(">Q", blob, o + 8)
            return st_name, st_value
    else:
        (e_entry,) = struct.unpack_from(">I", blob, 0x18)
        (e_phoff,) = struct.unpack_from(">I", blob, 0x1C)
        (e_shoff,) = struct.unpack_from(">I", blob, 0x20)
        e_phentsize, e_phnum = struct.unpack_from(">HH", blob, 0x2A)
        e_shentsize, e_shnum, e_shstrndx = struct.unpack_from(">HHH", blob, 0x2E)

        def shdr(i):
            o = e_shoff + i * e_shentsize
            return struct.unpack_from(">IIIIIIIIII", blob, o)

        def load_vaddrs():
            # Elf32_Phdr: type(4), offset(4), vaddr(4), ...
            vs = []
            for i in range(e_phnum):
                o = e_phoff + i * e_phentsize
                p_type, _off, p_vaddr = struct.unpack_from(">III", blob, o)
                if p_type == 1:
                    vs.append(p_vaddr)
            return vs

        def sym(o):
            # Elf32_Sym: name(4), value(4), size(4), info(1), other(1), shndx(2)
            return struct.unpack_from(">II", blob, o)

    return dict(is64=is64, e_entry=e_entry, e_shnum=e_shnum,
                e_shstrndx=e_shstrndx, shdr=shdr, load_vaddrs=load_vaddrs,
                sym=sym)


def read_elf_sections(blob):
    """Return (load_base, [ElfSection], entry) for the allocatable sections.

    The basefile is built from sections rather than PT_LOAD segments so the
    ELF's own headers do not end up in the image, and so each section can carry
    its address and flags into a matching PE section.
    """
    g = _elf_geom(blob)
    e_entry = g["e_entry"]
    e_shnum = g["e_shnum"]
    shdr = g["shdr"]

    # the image base is the lowest PT_LOAD vaddr (what --image-base set), not the
    # lowest section -- lld can place the first section a little above the base
    load_vaddrs = g["load_vaddrs"]()
    image_base = min(load_vaddrs) if load_vaddrs else None

    strtab_off = shdr(g["e_shstrndx"])[4]

    def name(off):
        end = blob.index(b"\0", strtab_off + off)
        return blob[strtab_off + off:end].decode("utf-8", "replace")

    # Metadata sections that are SHF_ALLOC in the ELF but not part of the
    # runtime image we want in the XEX. NOTE: .eh_frame / .eh_frame_hdr ARE now
    # carried across -- the C++ exception runtime (libunwind) reads .eh_frame at
    # runtime, and the linker script (mktitle.py) puts it on its own page well
    # past the PE headers, so the old tiny-RVA collision no longer applies.
    SKIP_PREFIXES = (".comment", ".note", ".ARM.")

    out = []
    for i in range(e_shnum):
        nm, typ, flags, addr, off, size, *_ = shdr(i)
        if not (flags & SHF_ALLOC) or size == 0:
            continue
        secname = name(nm)
        if any(secname.startswith(p) for p in SKIP_PREFIXES):
            continue
        s = ElfSection()
        s.name = secname
        s.vaddr = addr
        s.typ = typ
        s.flags = flags
        s.memsize = size
        s.data = b"" if typ == SHT_NOBITS else blob[off:off + size]
        out.append(s)
    if not out:
        sys.exit("no allocatable sections")
    out.sort(key=lambda s: s.vaddr)
    base = image_base if image_base is not None else out[0].vaddr
    return base, out, e_entry


# ---- little-endian PPC PE32 synthesis ---------------------------------------

# Xbox 360 PE headers are little-endian (verified on a real basefile) even
# though the code they describe is big-endian.
PE_MACHINE_POWERPCBE = 0x01F2
PE_FILE_EXECUTABLE_IMAGE = 0x0002
PE_FILE_32BIT_MACHINE = 0x0100
PE_OPTIONAL_MAGIC_PE32 = 0x010B
PE_SUBSYSTEM_XBOX = 14
PE_SIZEOF_OPTIONAL_HEADER = 224

PE_SCN_CODE = 0x00000020
PE_SCN_INIT_DATA = 0x00000040
PE_SCN_UNINIT_DATA = 0x00000080
PE_SCN_MEM_EXECUTE = 0x20000000
PE_SCN_MEM_READ = 0x40000000
PE_SCN_MEM_WRITE = 0x80000000

FILE_ALIGN = 0x1000


def page_size_for(base):
    """The 360 memory map pages the two title regions differently: the
    0x80000000-0x8FFFFFFF range uses 64KB pages, 0x90000000+ uses 4KB. The XEX
    page descriptors count pages in these units, so the packer must match the
    load address or the loader reserves the wrong amount and overruns it."""
    return 0x10000 if base < 0x90000000 else 0x1000


def _pe_section_flags(s):
    f = PE_SCN_MEM_READ
    if s.flags & SHF_EXECINSTR:
        f |= PE_SCN_CODE | PE_SCN_MEM_EXECUTE
    elif s.typ == SHT_NOBITS:
        f |= PE_SCN_UNINIT_DATA
    else:
        f |= PE_SCN_INIT_DATA
    if s.flags & SHF_WRITE:
        f |= PE_SCN_MEM_WRITE
    return f


def build_pe_basefile(base, sections, entry):
    """Assemble the loadable image as a little-endian PPC PE32.

    xenia maps the basefile contiguously at the load address and then reads the
    PE headers from offset 0, so file offset equals RVA: the PE headers sit at
    RVA 0 and each section's raw data sits at its own RVA.
    """
    e_lfanew = 0x40
    nsec = len(sections)
    headers_end = e_lfanew + 4 + 20 + PE_SIZEOF_OPTIONAL_HEADER + nsec * 40
    size_of_headers = (headers_end + FILE_ALIGN - 1) & ~(FILE_ALIGN - 1)

    page = page_size_for(base)
    image_end = base
    for s in sections:
        image_end = max(image_end, s.vaddr + s.memsize)
    size_of_image = ((image_end - base) + page - 1) & ~(page - 1)

    code_rva = next((s.vaddr - base for s in sections if s.flags & SHF_EXECINSTR),
                    size_of_headers)
    size_of_code = sum(len(s.data) for s in sections if s.flags & SHF_EXECINSTR)
    size_of_data = sum(len(s.data) for s in sections if not (s.flags & SHF_EXECINSTR))

    # --- DOS header ---
    # xenia's is_valid_executable checks the first dword is 0x905A4D, i.e. the
    # bytes "MZ\x90\x00" -- the standard DOS magic with e_cblp = 0x90, not a
    # bare "MZ\0\0". Getting this wrong makes the loader reject a valid PE.
    dos = bytearray(e_lfanew)
    dos[0:4] = b"MZ\x90\x00"
    struct.pack_into("<I", dos, 0x3C, e_lfanew)

    # --- file header ---
    file_hdr = struct.pack("<IHHIIIHH",
                           0x00004550,                 # "PE\0\0"
                           PE_MACHINE_POWERPCBE,
                           nsec, 0, 0, 0,
                           PE_SIZEOF_OPTIONAL_HEADER,
                           PE_FILE_EXECUTABLE_IMAGE | PE_FILE_32BIT_MACHINE)

    # --- optional header (PE32, 224 bytes incl. 16 data directories) ---
    opt = struct.pack("<HBBIIIIII",
                      PE_OPTIONAL_MAGIC_PE32, 0, 0,
                      size_of_code, size_of_data, 0,
                      entry - base, code_rva, 0)        # BaseOfCode, BaseOfData
    opt += struct.pack("<IIIHHHHHHIIIIHHIIIIII",
                       base,                            # ImageBase
                       page, FILE_ALIGN,               # SectionAlignment, FileAlignment
                       4, 0, 0, 0, 0, 0,                # os/image/subsystem versions
                       0,                               # Win32VersionValue
                       size_of_image, size_of_headers,
                       0,                               # CheckSum
                       PE_SUBSYSTEM_XBOX, 0,            # Subsystem, DllCharacteristics
                       0x40000, 0x1000, 0x100000, 0x1000,  # stack/heap reserve/commit
                       0, 16)                           # LoaderFlags, NumberOfRvaAndSizes
    # data directories (16 x {VirtualAddress, Size}). Only the exception
    # directory (index 3) is populated, and only when a .pdata section is present
    # -- MS-compiled objects contribute .pdata (RUNTIME_FUNCTION table) + .xdata,
    # and the console kernel's RtlLookupFunctionEntry finds the frame info through
    # this directory to dispatch MSVC C++ / SEH exceptions. Our own clang code is
    # DWARF/.eh_frame and emits no .pdata, so this stays zero for such titles.
    IMAGE_DIRECTORY_ENTRY_EXCEPTION = 3
    dirs = bytearray(16 * 8)
    pdata = next((s for s in sections if s.name == ".pdata"), None)
    if pdata is not None:
        struct.pack_into("<II", dirs, IMAGE_DIRECTORY_ENTRY_EXCEPTION * 8,
                         pdata.vaddr - base, pdata.memsize)
    opt += bytes(dirs)                                  # data directories
    assert len(opt) == PE_SIZEOF_OPTIONAL_HEADER, len(opt)

    # a kept section that starts inside the PE header region cannot be
    # represented -- the ELF must be linked to leave headroom below the first
    # section. Surface it rather than emit an overlapping, unloadable image.
    for s in sections:
        if (s.vaddr - base) < size_of_headers:
            sys.exit(f"section {s.name} at RVA 0x{s.vaddr - base:X} overlaps the "
                     f"PE headers (0x{size_of_headers:X}); link with headroom below "
                     f"the first section")

    # --- section table ---
    sec_hdrs = b""
    for s in sections:
        nm = s.name.encode("ascii")[:8].ljust(8, b"\0")
        rva = s.vaddr - base
        raw_size = (len(s.data) + FILE_ALIGN - 1) & ~(FILE_ALIGN - 1)
        sec_hdrs += struct.pack("<8sIIIIIIHHI",
                                nm, s.memsize, rva,
                                raw_size, rva,          # SizeOfRawData, PointerToRawData=RVA
                                0, 0, 0, 0,
                                _pe_section_flags(s))

    # --- assemble the image: headers at 0, each section at its RVA ---
    image = bytearray(size_of_image)
    image[0:len(dos)] = dos
    o = e_lfanew
    image[o:o + len(file_hdr)] = file_hdr; o += len(file_hdr)
    image[o:o + len(opt)] = opt; o += len(opt)
    image[o:o + len(sec_hdrs)] = sec_hdrs
    for s in sections:
        rva = s.vaddr - base
        image[rva:rva + len(s.data)] = s.data
    return base, bytes(image), entry, size_of_image


# ---- XEX2 structures --------------------------------------------------------

def build_basefile_format(image_size, zero_size):
    """RawBaseFileInfo: uncompressed (compType=1), unencrypted (encType=0)."""
    info = struct.pack(">iHH", 8 + 8, 0, 1)            # infoSize, encType, compType
    info += struct.pack(">ii", image_size, zero_size)  # one RawBaseFileBlock
    return info


def build_execution_info(title_id):
    """xex2_opt_execution_info (0x18 bytes): the title identity xenia reads.

    Layout: media_id, version, base_version, title_id, platform, exec_table,
    disc_number, disc_count, savegame_id. Only title_id matters here (it must be
    non-zero); disc 1/1 is the sensible default for a single-file title."""
    return struct.pack(">IIII", 0, 0, 0, title_id) + \
           struct.pack(">BBBB", 0, 0, 1, 1) + struct.pack(">I", 0)


def read_elf_symbol_addrs(blob):
    """Map global/defined symbol name -> virtual address, from the ELF symtab."""
    g = _elf_geom(blob)
    sh = g["shdr"]

    symtab = strtab = None
    for i in range(g["e_shnum"]):
        typ = sh(i)[1]
        if typ == 2:                                # SHT_SYMTAB
            symtab = sh(i)
            strtab = sh(symtab[6])                  # sh_link -> strtab
    out = {}
    if not symtab:
        return out
    off, size, entsize = symtab[4], symtab[5], symtab[9]
    stroff = strtab[4]
    for k in range(size // entsize):
        st_name, st_value = g["sym"](off + k * entsize)
        if st_name and st_value:
            end = blob.index(b"\0", stroff + st_name)
            out[blob[stroff + st_name:end].decode("utf-8", "replace")] = st_value
    return out


def derive_auto_imports(blob, xdk_lib_dir):
    """Derive the import table from the kernel-import thunks the link kept.

    With the global import library (build_import_lib.py) a title just links
    kernel_import.a and --gc-sections keeps the thunks its code reaches. Here we
    read those surviving symbols back out (<name> thunk + __imp_<name> record),
    look each up in the same ordinal index the library was built from, group them
    into the XEX import name table, and report the module_index each record must
    carry -- so the caller can patch the placeholder-0 index the library left.

    Returns (imports, patches): imports is the [(module, [record_va, ...])] list
    build_import_libraries wants; patches is [(record_va, module_index)] naming
    every 4-byte record word whose bits 16-23 must become module_index.
    """
    import gen_import_stubs as _g
    index = _g.build_ordinal_index(xdk_lib_dir)
    syms = read_elf_symbol_addrs(blob)                 # defined name -> VA
    by_module = {}
    for name, (module, ordinal, is_var) in index.items():
        imp_va = syms.get("__imp_" + name)
        if imp_va is None:
            continue                                   # thunk was gc'd; not imported
        thunk_va = None if is_var else syms.get(name)
        by_module.setdefault(module, []).append((ordinal, is_var, thunk_va, imp_va))
    imports, patches = [], []
    for module_index, module in enumerate(sorted(by_module)):
        records = []
        for _ordinal, is_var, thunk_va, imp_va in by_module[module]:
            records.append(imp_va)                     # the IAT record
            patches.append((imp_va, module_index))
            if not is_var:                             # a function also lists its thunk;
                records.append(thunk_va)               # both thunk words carry the index
                patches.append((thunk_va, module_index))
                patches.append((thunk_va + 4, module_index))
        imports.append((module, records))
    return imports, patches


def build_import_libraries(imports):
    """Build the IMPORT_LIBRARIES optional-header block.

    `imports` is a list of (library_name, [record_virtual_address, ...]). Each
    record VA points at an import slot (a 4-byte variable record) or a 16-byte
    thunk already placed in the image; the record's own bytes encode its type
    and ordinal, which the loader reads and rewrites.

    Layout (xex2_opt_import_libraries): total size, then a string table
    (size, count, padded names), then one xex2_import_library per library.
    """
    # ImageXex writes the target module's HV export-table id here (same value
    # on every title that imports that module). Zero makes HvxResolveImports
    # return C0000225 (STATUS_NOT_FOUND). Values from a 21256 title that loads
    # on this kit; retry after import records carry the name-table index.
    module_ids = {
        "xboxkrnl.exe": 0x45DC17E0,
        "xam.xex":      0xFCA15C76,
        "xbdm.xex":     0xECEB8109,
    }
    names = [name for name, _ in imports]
    name_data = b""
    name_index = {}
    for name in names:
        name_index[name] = len(name_index)
        name_data += name.encode("ascii") + b"\0"
        if len(name_data) % 4:
            name_data += b"\0" * (4 - (len(name_data) % 4))

    libs = b""
    for name, records in imports:
        count = len(records)
        lib = struct.pack(">I", 0x28 + count * 4)     # size
        lib += b"\0" * 0x14                            # next_import_digest
        lib += struct.pack(">I", module_ids.get(name, 0))  # id
        # Same packed versions ImageXex writes (v2.0.21256, min v2.0.1861).
        lib += struct.pack(">I", 0x20530800)           # version_value
        lib += struct.pack(">I", 0x20074500)           # version_min_value
        lib += struct.pack(">H", name_index[name])     # name_index
        lib += struct.pack(">H", count)                # count
        for rec in records:
            lib += struct.pack(">I", rec)              # import_table[]
        libs += lib

    total = 12 + len(name_data) + len(libs)
    out = struct.pack(">III", total, len(name_data), len(names))
    out += name_data
    out += libs
    return out


def build_page_descriptors(base, sections, image_size, page_size):
    """Partition the image's pages into (page_count, info) descriptors by the
    protection each page needs.

    xenia (with writable_code_segments=false, the default) maps CODE and
    READONLY_DATA pages read-only and DATA pages read-write, so a title that
    writes a global faults unless its writable sections land in DATA pages.
    One descriptor spanning the whole image as CODE (the first cut) made
    everything read-only; this walks the sections and marks each page CODE,
    DATA or READONLY_DATA from the sections that occupy it.

    Page granularity is coarse (64KB below 0x90000000), so a writable section
    that shares a page with code cannot be mapped writable without making the
    code page writable too. The linker script must align the writable region
    (.data/.bss) to the page size so it occupies its own page(s); if it does
    not, the shared page falls back to CODE (read-only) and a note is printed.
    """
    num_pages = image_size // page_size
    kinds = []
    shared_code_write = False
    for p in range(num_pages):
        lo, hi = p * page_size, (p + 1) * page_size
        has_exec = has_write = has_ro = False
        for s in sections:
            s_lo = s.vaddr - base
            s_hi = s_lo + s.memsize
            if s_hi <= lo or s_lo >= hi:
                continue
            if s.flags & SHF_EXECINSTR:
                has_exec = True
            elif s.flags & SHF_WRITE:
                has_write = True
            else:
                has_ro = True
        if p == 0:
            has_ro = True                              # PE headers: read-only metadata
        if has_write and has_exec:
            shared_code_write = True
        if has_write:
            info = SECTIONINFO_DATA
        elif has_exec:
            info = SECTIONINFO_CODE
        else:
            info = SECTIONINFO_READONLY
        kinds.append(info)

    # xenia's per-title code hash scans for a CODE page and reads a wild range
    # if it finds none; guarantee at least one. Prefer the header/code page.
    forced = False
    if SECTIONINFO_CODE not in kinds and kinds:
        kinds[0] = SECTIONINFO_CODE
        forced = True

    # One HV descriptor per 64KB page, like ImageXex. Coalescing adjacent
    # pages of the same kind (e.g. 6xCODE as one descriptor) produces a
    # hash chain the kit rejects with LDRX C0000221 after a debug sign.
    descriptors = [(1, info) for info in kinds]

    notes = []
    if shared_code_write:
        notes.append("a writable section shares a page with code; align the "
                     "writable region (.data/.bss) to the page size to map it "
                     "read-write")
    if forced:
        notes.append("no page held only code, so page 0 was forced CODE for the "
                     "code hash; writable data on that page stays read-only")
    return descriptors, notes


def build_security_info(image_size, load_address, sections):
    """XexSecurityInfo + section table. Hashes and importTableCount are left zero
    here; the debug-sign pass (xex_debugsign.debug_sign, on by default) fills the
    section/import/header hashes and the RSA signature over the assembled XEX."""
    ZHASH = b"\0" * 20
    image_info = b""
    image_info += b"\0" * 256                          # signature
    image_info += struct.pack(">i", 0x174)             # infoSize
    image_info += struct.pack(">I", 0)                 # imageFlags (none: unencrypted)
    image_info += struct.pack(">I", load_address)      # loadAddress
    image_info += ZHASH                                # imageHash
    image_info += struct.pack(">i", 0)                 # importTableCount
    image_info += ZHASH                                # importHash
    image_info += b"\0" * 16                           # mediaId
    image_info += b"\0" * 16                           # imageKey (zero: unencrypted)
    image_info += struct.pack(">I", 0)                 # exportTableAddress
    image_info += ZHASH                                # headerHash
    image_info += struct.pack(">I", 0xFFFFFFFF)        # gameRegion (all)
    assert len(image_info) == 0x174, hex(len(image_info))

    sec_bytes = b""
    for pages, info_type in sections:
        sec_bytes += struct.pack(">I", (pages << 4) | (info_type & 0xF)) + ZHASH

    total = 0x184 + len(sec_bytes)
    out = struct.pack(">ii", total, image_size)
    out += image_info
    out += struct.pack(">I", 0xFFFFFFFF)               # allowedMediaTypes (all)
    out += struct.pack(">i", len(sections))            # sectionCount
    out += sec_bytes
    return out


def pack(elf_path, out_path, base_override=None, imports=None, sign=True, encrypt=True,
         auto_imports_xdk=None):
    blob = open(elf_path, "rb").read()
    load_base, sections, entry = read_elf_sections(blob)
    if base_override is not None and base_override != load_base:
        sys.exit(f"ELF is linked at 0x{load_base:08X}, not 0x{base_override:08X}; "
                 f"link it at the target base instead of overriding here")

    # resolve any import records: each is a library plus ELF symbol names whose
    # addresses become the import table (the records themselves live in the image)
    import_block = None
    patches = []
    if auto_imports_xdk:
        # SDK-style: the title linked the global kernel_import.a; derive the table
        # from the thunks the link kept and patch the module_index the lib left 0.
        resolved, patches = derive_auto_imports(blob, auto_imports_xdk)
        if resolved:
            import_block = build_import_libraries(resolved)
    elif imports:
        syms = read_elf_symbol_addrs(blob)
        resolved = []
        for libname, symnames in imports:
            vas = []
            for s in symnames:
                if s not in syms:
                    sys.exit(f"import symbol {s!r} not found in {elf_path}")
                vas.append(syms[s])
            resolved.append((libname, vas))
        import_block = build_import_libraries(resolved)

    load_base, image, entry, image_size = build_pe_basefile(load_base, sections, entry)

    # Patch each import record word's module_index (bits 16-23), now that the
    # image bytes are placed. VA maps to image offset as VA - load_base.
    if patches:
        image = bytearray(image)
        for va, module_index in patches:
            off = va - load_base
            (word,) = struct.unpack_from(">I", image, off)
            struct.pack_into(">I", image, off, word | (module_index << 16))
        image = bytes(image)

    # the basefile is the PE image; nothing is zero-trimmed for the first cut.
    # the security-info section table counts pages in the load region's page
    # size (64KB below 0x90000000, 4KB above), matching what the loader reserves.
    zero_size = 0
    pages = image_size // page_size_for(load_base)

    basefile_format = build_basefile_format(len(image), zero_size)
    # per-section page descriptors: code/read-only pages stay read-only, writable
    # pages (.data/.bss) map read-write, and at least one page is CODE for
    # xenia's per-title code hash (see build_page_descriptors).
    descriptors, desc_notes = build_page_descriptors(
        load_base, sections, image_size, page_size_for(load_base))
    assert sum(c for c, _ in descriptors) == pages, (descriptors, pages)
    security = build_security_info(image_size, load_base, descriptors)

    # optional-header directory: inline-value keys (low byte 0x00/0x01) carry
    # their value directly; others carry a file offset to their data.
    inline = [
        (KEY_ENTRY_POINT, entry),
        (KEY_IMAGE_BASE_ADDRESS, load_base),
        (KEY_ORIGINAL_BASE_ADDRESS, load_base),
        # a real title carries a stack size; without it the loader sets up the
        # main thread with a zero stack
        (KEY_STACK_SIZE, DEFAULT_STACK_SIZE),
    ]
    # (key, data) blocks placed after the security info; the directory records
    # the file offset of each. CHECKSUM_TIMESTAMP (checksum, timestamp) is a
    # header every real XEX carries; a stock devkit loader rejects an image
    # without it (LDRX C000007B) even though xenia tolerates its absence. The
    # values are not verified, so zero/zero is fine.
    offset_blocks = [(KEY_BASEFILE_FORMAT, basefile_format),
                     (KEY_CHECKSUM_TIMESTAMP, struct.pack(">II", 0, 0)),
                     # XEX_TLS_INFO: slotCount, rawDataAddress, dataSize, rawDataSize
                     (KEY_TLS_INFO, struct.pack(">IIII", DEFAULT_TLS_SLOTS, 0, 0, 0)),
                     (KEY_EXECUTION_INFO, build_execution_info(DEFAULT_TITLE_ID))]
    if import_block is not None:
        offset_blocks.append((KEY_IMPORT_LIBRARIES, import_block))

    n_entries = len(inline) + len(offset_blocks)
    header_size = 0x18 + n_entries * 8

    # layout: image header, directory, then security info, then each offset
    # block, all before the page-aligned basefile.
    sec_off = header_size
    block_offsets = {}
    cur = sec_off + len(security)
    for key, data in offset_blocks:
        block_offsets[key] = cur
        cur += len(data)
    basefile_off = (cur + PAGE - 1) & ~(PAGE - 1)

    # The optional-header directory must be sorted by key ascending: the console
    # loader binary-searches it, so an unsorted table makes it miss headers and
    # reject the image (xenia linear-scans and does not care).
    entries = [(k, v) for k, v in inline] + \
              [(k, block_offsets[k]) for k, _ in offset_blocks]
    directory = b"".join(struct.pack(">II", k, v) for k, v in sorted(entries))

    image_header = struct.pack(">4sIiiiI", b"XEX2",
                               MODULEFLAG_TITLE_MODULE,
                               basefile_off,          # sizeOfHeaders
                               0,                     # sizeOfDiscardableHeaders (loader checks 0)
                               sec_off,               # securityInfoOffset
                               n_entries)

    out = bytearray()
    out += image_header
    out += directory
    assert len(out) == sec_off
    out += security
    for key, data in offset_blocks:
        assert len(out) == block_offsets[key]
        out += data
    out += b"\0" * (basefile_off - len(out))
    out += image

    # Debug-sign (and by default debug-encrypt) so a real kit loads the title:
    # an unsigned image is classified retail and rejected, and a genuine devkit
    # additionally requires the basefile to be debug-encrypted. Done on the
    # assembled bytes because the header hash covers the descriptor hashes and
    # import digests, and the signature covers the encrypted image key.
    if sign:
        out = bytearray(xex_debugsign.debug_sign(bytes(out), encrypt=encrypt))

    with open(out_path, "wb") as f:
        f.write(out)
    state = "signed+encrypted" if (sign and encrypt) else "signed" if sign else "UNSIGNED"
    print(f"wrote {out_path}: base 0x{load_base:08X} entry 0x{entry:08X} "
          f"image {len(image)} bytes ({pages} pages), file {len(out)} bytes ({state})")
    names = {SECTIONINFO_CODE: "CODE", SECTIONINFO_DATA: "RWDATA",
             SECTIONINFO_READONLY: "RODATA"}
    print("  pages: " + ", ".join(f"{c}x{names[info]}" for c, info in descriptors))
    for note in desc_notes:
        print(f"  note: {note}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("elf")
    ap.add_argument("-o", "--out", default="out.xex")
    ap.add_argument("--base", type=lambda s: int(s, 0), default=None,
                    help="assert the ELF's load base (does not relocate)")
    ap.add_argument("--import", dest="imports", action="append", default=[],
                    metavar="LIB:sym1,sym2,...",
                    help="add an import library; syms name the import records "
                         "(variable then thunk per function) as ELF symbols")
    ap.add_argument("--import-manifest", default=None,
                    help="JSON from gen_import_stubs.py listing the import records")
    ap.add_argument("--auto-imports", default=None, metavar="XDK_LIB_DIR",
                    help="SDK-style: derive the import table from the global "
                         "kernel_import.a thunks the link kept (no manifest); the "
                         "arg is the XDK lib\\xbox dir for the ordinal lookup")
    ap.add_argument("--no-sign", dest="sign", action="store_false",
                    help="leave the XEX unsigned and unencrypted (default: "
                         "debug-sign and -encrypt it so a real kit will load it)")
    ap.add_argument("--no-encrypt", dest="encrypt", action="store_false",
                    help="debug-sign but do not encrypt (loads in xenia and on a "
                         "kit set to accept unencrypted images, not a stock devkit)")
    args = ap.parse_args()
    imports = []
    for spec in args.imports:
        lib, _, symlist = spec.partition(":")
        imports.append((lib, [s for s in symlist.split(",") if s]))
    if args.import_manifest:
        import json
        m = json.load(open(args.import_manifest))
        for lib in m["libraries"]:
            imports.append((lib["module"], lib["records"]))
    pack(args.elf, args.out, args.base, imports, sign=args.sign, encrypt=args.encrypt,
         auto_imports_xdk=args.auto_imports)


if __name__ == "__main__":
    main()
