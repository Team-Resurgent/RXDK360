/* Which CPU memory window does physical allocation land in?
 *
 * AdvancedLighting's Debug D3D runtime (d3d9d) asserts "AssertPhysical" on any
 * GPU resource pointer < 0xA0000000. On the kit, XPhysicalAlloc memory came back
 * at 0x40001600. This probes XPhysicalAlloc directly for several Protect/cache
 * types and prints the window, so we can see (a) whether xenia reproduces the
 * 0x40000000 window and (b) which Protect, if any, reaches >= 0xA0000000. */
#include "rxdk_test.h"

#define PAGE_READWRITE    0x04u
#define PAGE_NOCACHE      0x200u
#define PAGE_WRITECOMBINE 0x400u
#define MEM_LARGE_PAGES   0x20000000u

/* XPhysicalAlloc(dwSize, ulPhysicalAddress, dwAlignment, flProtect) */
extern void* XPhysicalAlloc(unsigned long dwSize, unsigned long ulPhysicalAddress,
                            unsigned long dwAlignment, unsigned long flProtect);
extern void  XPhysicalFree(void* p);

static void probe(const char* label, unsigned long protect) {
    void* p = XPhysicalAlloc(0x10000, 0xFFFFFFFFul, 4096, protect);
    unsigned long a = (unsigned long)(unsigned long long)p;   /* 32-bit ptr */
    DbgPrint("[PHYS] %-28s protect=0x%08lX -> 0x%08lX (window 0x%08lX)\n",
             label, protect, a, a & 0xF0000000ul);
    if (p) XPhysicalFree(p);
}

int main() {
    /* PAGE_WRITECOMBINE/PAGE_NOCACHE are cache MODIFIERS: they must be OR'd with a
       base protection (PAGE_READWRITE). Passed alone they are an invalid protect and
       the kernel rejects them (NULL). D3D allocates GPU resources as
       PAGE_READWRITE|PAGE_WRITECOMBINE, so that combined form is the one that matters. */
    probe("READWRITE",              PAGE_READWRITE);
    probe("RW|WRITECOMBINE",        PAGE_READWRITE | PAGE_WRITECOMBINE);
    probe("RW|NOCACHE",             PAGE_READWRITE | PAGE_NOCACHE);
    probe("READWRITE|LARGE",        PAGE_READWRITE | MEM_LARGE_PAGES);
    probe("RW|WRITECOMBINE|LARGE",  PAGE_READWRITE | PAGE_WRITECOMBINE | MEM_LARGE_PAGES);
    probe("RW|NOCACHE|LARGE",       PAGE_READWRITE | PAGE_NOCACHE | MEM_LARGE_PAGES);

    /* Also record whether the write-combined form D3D uses reaches a physical window. */
    void* wc = XPhysicalAlloc(0x10000, 0xFFFFFFFFul, 4096, PAGE_READWRITE | PAGE_WRITECOMBINE);
    unsigned long wca = (unsigned long)(unsigned long long)wc;
    CHECK(wca >= 0xA0000000ul, "WRITECOMBINE reaches >=0xA0000000 window");
    if (wc) XPhysicalFree(wc);

    CHECK_DONE("physwindow");
    return 0;
}
