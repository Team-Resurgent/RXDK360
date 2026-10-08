#include "rt.h"
/* VMX halfword multiplies. clang lowers a 4 x 32-bit vector multiply (which its
   loop vectorizer produces from ordinary integer code) to vmulouh + vmsumuhm,
   the AltiVec idiom for targets without vmuluwm. xenia does not implement
   either (break_on_unimplemented_instructions stops the title), so this
   checks what the console does with them. Each line runs one instruction; a
   missing line means the instruction before it faulted. expected.txt holds
   the AltiVec-defined results. */

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

    __asm__ __volatile__("lvx 0, 0, %1\n\t"
                         "lvx 1, 0, %2\n\t"
                         "vmulouh 2, 0, 1\n\t"
                         "stvx 2, 0, %0"
                         :
                         : "b"(&r), "b"(&a), "b"(&b)
                         : "v0", "v1", "v2", "memory");
    show("vmulouh", r);

    __asm__ __volatile__("lvx 0, 0, %1\n\t"
                         "lvx 1, 0, %2\n\t"
                         "vmuleuh 2, 0, 1\n\t"
                         "stvx 2, 0, %0"
                         :
                         : "b"(&r), "b"(&a), "b"(&b)
                         : "v0", "v1", "v2", "memory");
    show("vmuleuh", r);

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
    show("v4i32 mul", va * vb);
}
