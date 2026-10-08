#include "rt.h"
extern void HalReturnToFirmware(unsigned int routine);

/* Pre-main: run the C++ static constructors (.init_array) the way a real title's
   xapilib startup does before it reaches the title. The linker script provides
   the bounds. This is the equivalent of the XDK's XapiThreadStartup running the
   CRT init before main; the real xapilib startup will replace this shim later. */
typedef void (*init_fn)(void);
extern init_fn __init_array_start[];
extern init_fn __init_array_end[];

/* The MS CRT initializer table (.CRT$XC*): how a prebuilt MS library registers a
   pre-main constructor. Entries can be null (the XCA/XCZ boundary markers), so
   skip those -- this is what the XDK CRT's _initterm does. */
extern init_fn __xc_a[];
extern init_fn __xc_z[];

static void run_init_array(void) {
    for (init_fn *f = __init_array_start; f != __init_array_end; ++f)
        (*f)();
    for (init_fn *f = __xc_a; f != __xc_z; ++f)
        if (*f)
            (*f)();
}

/* The process heap. malloc/new/RtlAllocateHeap all live on XapiProcessHeap, which
   the real crt_start (and the console's XapiInitProcess) creates before anything
   allocates. This shim bypasses crt_start, so without this a title's very first
   malloc returns NULL -- which silently breaks any allocation, including
   thrd_create spawning a worker. Mirror crt_start: create a growable heap up
   front. */
extern void *XapiProcessHeap;
extern void *RtlCreateHeap(unsigned flags, void *base, unsigned long reserve,
                           unsigned long commit, void *lock, void *params);

/* Shared entry: run static ctors, then the program's title_main, print a
   completion sentinel, then reboot the console (HalReturnToFirmware routine 2;
   xenia exits on any routine, and routine 0 powers a devkit off, so the next
   hardware run cannot reach it). The sentinel is the last line the
   harness expects, so a dropped trailing line (xenia's async logger can lose the
   last line on a fast exit) costs the sentinel, not the program's real output;
   the harness also retries a run whose sentinel never arrived. */
void _start(void) {
    if (!XapiProcessHeap)                 /* HEAP_GROWABLE, /HEAP default 1MB/4KB */
        XapiProcessHeap = RtlCreateHeap(0x2, 0, 0x100000, 0x1000, 0, 0);
    run_init_array();
    title_main();
    DbgPrint("##RXDK-CORPUS-END##\n");
    HalReturnToFirmware(2);
    for (;;) {}
}
