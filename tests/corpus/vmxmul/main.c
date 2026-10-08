#include "rt.h"
/* VMX halfword multiplies. clang lowers a 4 x 32-bit vector multiply (which its
   loop vectorizer produces from ordinary integer loops) to vmulouh + vmsumuhm,
   the AltiVec idiom for targets without vmuluwm. A devkit run (2026-10-08)
   printed "try vmulouh" and then raised STATUS_ILLEGAL_INSTRUCTION
   (0xC000001D) on it: Xenon's VMX does not implement it, so the compiler must
   not emit these for the Xbox 360 (xenia does not implement them either and
   stops the title). Each instruction is announced before it runs, so the last
   "try" line shows which one faulted. expected.txt holds the AltiVec-defined
   results. */

typedef unsigned int v4u __attribute__((vector_size(16)));

static const v4u a = { 0x00020003u, 0x00040005u, 0xFFFF0001u, 0x12345678u };
static const v4u b = { 0x00060007u, 0x00080009u, 0x0002FFFFu, 0x9ABCDEF0u };
static const v4u c = { 1u, 2u, 3u, 4u };

static void show(const char *name, v4u r)
{
    DbgPrint("%s %08x %08x %08x %08x\n", name, r[0], r[1], r[2], r[3]);
}

void t_vmxmul(void)
{
    v4u r;
    volatile v4u va = a, vb = b;

    DbgPrint("try vmulouh\n");
    __asm__ __volatile__("lvx 0, 0, %1\n\t"
                         "lvx 1, 0, %2\n\t"
                         "vmulouh 2, 0, 1\n\t"
                         "stvx 2, 0, %0"
                         :
                         : "b"(&r), "b"(&a), "b"(&b)
                         : "v0", "v1", "v2", "memory");
    show("vmulouh", r);

    DbgPrint("try vmuleuh\n");
    __asm__ __volatile__("lvx 0, 0, %1\n\t"
                         "lvx 1, 0, %2\n\t"
                         "vmuleuh 2, 0, 1\n\t"
                         "stvx 2, 0, %0"
                         :
                         : "b"(&r), "b"(&a), "b"(&b)
                         : "v0", "v1", "v2", "memory");
    show("vmuleuh", r);

    DbgPrint("try vmsumuhm\n");
    __asm__ __volatile__("lvx 0, 0, %1\n\t"
                         "lvx 1, 0, %2\n\t"
                         "lvx 3, 0, %3\n\t"
                         "vmsumuhm 2, 0, 1, 3\n\t"
                         "stvx 2, 0, %0"
                         :
                         : "b"(&r), "b"(&a), "b"(&b), "b"(&c)
                         : "v0", "v1", "v2", "v3", "memory");
    show("vmsumuhm", r);

    /* What the compiler emits for a plain vector multiply. */
    DbgPrint("try v4i32 mul\n");
    show("v4i32 mul", va * vb);
}
