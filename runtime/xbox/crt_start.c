/*
 * 2026 - Team Resurgent
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Part of RXDK - see LICENSE.md for the full GNU GPL v3.
 */

/*
 * Standard title entry: the CRT startup a title gets for free when it defines
 * main() and links libc, mirroring the XDK's XapiTitleStartup. It runs the C++
 * static constructors (.init_array) and the MS CRT initializer table (.CRT$XC*)
 * before main -- so both first-party and prebuilt-MS pre-main init run the way a
 * console title expects -- calls main, then runs the destructors (.fini_array,
 * e.g. the stdio flush), traces main's return value the way the console's xapi
 * startup does, and reboots the console.
 *
 * This lives in libc.a as its own object, so it is pulled in only when the title
 * has not supplied its own _start (e.g. the corpus driver, which uses its own
 * entry + title_main). ENTRY(_start) in the linker script points here.
 */
extern int  DbgPrint(const char *, ...);
extern void HalReturnToFirmware(unsigned int routine);

/* The process heap. The XDK's RtlAllocateHeap-based allocators -- GetProcessHeap,
   and the many shipped libraries that call HeapAlloc(GetProcessHeap(), ...), vcomp
   among them for its OpenMP lock objects -- read the heap handle from the data
   symbol XapiProcessHeap. On console the real XapiInitProcess creates it early with
   RtlCreateHeap; our minimal startup mirrors that here. RtlCreateHeap and
   XapiProcessHeap come from the XDK's own xapilib (translated), so the heap layout
   matches exactly what RtlAllocateHeap/RtlFreeHeap expect. Our own malloc/free/
   new/delete run on THIS heap too (see rt_support.c) -- one heap for the whole
   image, so a pointer from any allocator is valid to any free, as libcMT assumes.
   It must therefore exist before any allocation, i.e. before ctors. */
extern void *XapiProcessHeap;
extern void *RtlCreateHeap(unsigned flags, void *base,
                           unsigned long reserve, unsigned long commit,
                           void *lock, void *parameters);
extern unsigned XexGetModuleHandle(const char *name, void **out_handle);

static unsigned rd_le32(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) |
           ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

/* Heap reserve/commit are a *build* setting: the linker's /HEAP option, which
   the Xbox 360 Image Conversion "Heap Size" field feeds and which lands in the
   image's PE optional header (SizeOfHeapReserve/Commit). The console's real
   XapiInitProcess sizes the process heap from there rather than a constant, so
   we do the same -- read this module's PE header and honour the build value,
   falling back to the MSVC /HEAP default (1MB/4KB) only if it can't be read. */
static void init_process_heap(void) {
    if (XapiProcessHeap)
        return;
    unsigned long reserve = 0x100000, commit = 0x1000;   /* /HEAP default */
    void *hmod = 0;
    if (XexGetModuleHandle(0, &hmod) == 0 && hmod) {
        /* the handle is an LDR_DATA_TABLE_ENTRY; DllBase is at +0x18 (a native
           big-endian guest pointer -- an ordinary load reads it correctly). */
        const unsigned char *base =
            (const unsigned char *)(unsigned long)
                *(const volatile unsigned *)((const unsigned char *)hmod + 0x18);
        /* PE headers are little-endian even on this big-endian console, so pull
           the dwords out byte-wise. e_lfanew @ DOS+0x3C; optional header follows
           the 4-byte PE signature and 20-byte COFF header; SizeOfHeapReserve @
           optional+0x50, SizeOfHeapCommit @ +0x54. */
        if (base && base[0] == 'M' && base[1] == 'Z') {
            const unsigned char *opt = base + rd_le32(base + 0x3C) + 4 + 20;
            unsigned r = rd_le32(opt + 0x50), c = rd_le32(opt + 0x54);
            if (r) { reserve = r; if (c) commit = c; }
        }
    }
    /* HEAP_GROWABLE (0x2): grows on demand from the reserve. */
    XapiProcessHeap = RtlCreateHeap(0x2, 0, reserve, commit, 0, 0);
}

typedef void (*init_fn)(void);
extern init_fn __init_array_start[];
extern init_fn __init_array_end[];
extern init_fn __xc_a[];             /* .CRT$XCA .. .CRT$XCZ (MS ctor table) */
extern init_fn __xc_z[];
extern init_fn __fini_array_start[];
extern init_fn __fini_array_end[];

/* The title's entry. Declared with (int, char**) so int main(void),
   void main(void) and int main(int, char**) all link and call correctly. */
extern int main(int argc, char **argv);
extern void __rxdk_run_atexit(void);  /* C++ static dtors + atexit handlers */

/* The loader's command line: xboxkrnl exports ExLoadedCommandLine (ordinal
   0x1AE) as a *data* variable -- a char* to the launch command line (empty on a
   normal disc/xex launch, non-empty when a debugger / Remote Reboot / a title
   relaunch passes one). It is imported as a variable (tools/gen_import_stubs.py
   emits it as a .kvars slot the loader patches with the pointer), so reading it
   yields the string, with no xapi initialisation required. NULL if unresolved --
   the tokeniser handles that. */
extern char *ExLoadedCommandLine;

#define RXDK_ARG_MAX     32
#define RXDK_CMDLINE_MAX 256
static char  g_cmdline[RXDK_CMDLINE_MAX];
static char *g_argv[RXDK_ARG_MAX + 1];

/*
 * Split a command line into argv, in place, returning argc. A simple whitespace
 * tokeniser with "quoted arg" support; `cl` is copied into `buf` first (the
 * kernel string may be read-only) and tokenised there, so `argv` points into
 * `buf`. argv[0] is the first token, argv[argc] is NULL. Exposed (non-static) so
 * it can be unit-tested with known inputs, independent of a live command line.
 */
int __rxdk_parse_cmdline(const char *cl, char *buf, int bufsz,
                         char **argv, int argmax) {
    int argc = 0, n = 0;
    char *p;
    if (!cl) { if (argmax > 0) argv[0] = (char *)0; return 0; }
    while (cl[n] && n < bufsz - 1) { buf[n] = cl[n]; ++n; }
    buf[n] = '\0';
    p = buf;
    while (*p && argc < argmax - 1) {
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p) break;
        if (*p == '"') {
            argv[argc++] = ++p;
            while (*p && *p != '"') ++p;
        } else {
            argv[argc++] = p;
            while (*p && *p != ' ' && *p != '\t') ++p;
        }
        if (*p) *p++ = '\0';
    }
    argv[argc] = (char *)0;
    return argc;
}

/* Build argv from the kernel's loaded command line for main(). Empty on a normal
   launch -> argc 0, argv[0] == NULL. __rxdk_parse_cmdline is also exercised
   directly (with known inputs) by tests/stdlib/t_args.c. */
static int build_args(void) {
    return __rxdk_parse_cmdline(ExLoadedCommandLine, g_cmdline, RXDK_CMDLINE_MAX,
                                g_argv, RXDK_ARG_MAX + 1);
}

static void run_init(void) {
    for (init_fn *f = __init_array_start; f != __init_array_end; ++f)
        (*f)();
    for (init_fn *f = __xc_a; f != __xc_z; ++f)
        if (*f)
            (*f)();
}

static void run_fini(void) {
    /* .fini_array runs in reverse of registration order. */
    for (init_fn *f = __fini_array_end; f != __fini_array_start;)
        (*--f)();
}

/* C standard exit(): run the atexit/static-dtor + .fini_array teardown, trace the
   status the way the console's xapi startup does, and reboot the console
   (HalReturnToFirmware routine 2; routine 0 powers a devkit off, leaving it
   unreachable until someone presses the power button). Callable from any
   depth (unlike returning from main). _start routes through it so the normal-return
   and explicit-exit paths are identical. picolibc's exit.c is excluded from the
   build in favour of this one (it drives our __rxdk_run_atexit, not picolibc's
   exitprocs). _Exit()/_exit() (no teardown) remain picolibc's. */
_Noreturn void exit(int code) {
    __rxdk_run_atexit();  /* C++ static-object dtors + atexit, reverse order */
    run_fini();           /* .fini_array (e.g. the stdio flush), runs last */
    DbgPrint("[XAPI RETURN VALUE] %d\n", code);
    HalReturnToFirmware(2);
    for (;;) {}
}

/* tzinit.c: install the console's time zone as the default TZ. */
extern void xbox_init_timezone(void);

/* cache:\ is the title's writable utility drive. On the console it does not
   exist until the title mounts it (the "\??\cache:" link is missing and opens
   fail with STATUS_OBJECT_PATH_NOT_FOUND); the original Xbox's startup mounts
   its utility drive the same way. XMountUtilityDrive comes from the XDK's
   xapilib (translated): keep existing contents, 16KB clusters, a 64KB file
   cache. xenia maps cache:\ itself and fails this call (no STFS devices), so a
   failure is reported, not fatal. */
extern unsigned long XMountUtilityDrive(int fFormatClean, unsigned long dwBytesPerCluster,
                                        unsigned long dwFileCacheSize);

static void mount_utility_drive(void) {
    unsigned long err = XMountUtilityDrive(0, 0x4000, 0x10000);
    if (err != 0)
        DbgPrint("RXDK: XMountUtilityDrive failed (%lu); cache:\\ may be unavailable\n", err);
}

/* The console's XapiInitProcess enables xapilib's small-pages override so
   XPhysicalAllocEx does not add MEM_LARGE_PAGES. Without it, large-page physical
   allocations land in the 0x40000000 window, and d3d9d's D3DPOOL_DEFAULT texture
   memory ends up at an alias the GPU cannot reach -- MmGetPhysicalAddress fails on
   the texture base and d3d9d's AssertPhysical trips (the AdvancedLighting shadow-map
   depth/variance texture). Our minimal startup must set it the same way, before any
   allocation. (xapilib: physical.o -> g_fSmallPagesOverride.) */
extern void XEnableSmallPagesOverride(int fEnable);

void _start(void) {
    XEnableSmallPagesOverride(1);     /* small (GPU-mappable) physical pages, before any alloc */
    init_process_heap();              /* before init: C++/CRT ctors may allocate */
    mount_utility_drive();            /* after the heap (it allocates), before ctors that may open files */
    xbox_init_timezone();             /* after the heap (setenv), before ctors that may call localtime */
    run_init();
    int argc = build_args();
    exit(main(argc, g_argv));         /* g_argv is always NULL-terminated */
}
