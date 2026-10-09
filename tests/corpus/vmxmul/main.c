#include "rt.h"
/* A 4 x 32-bit vector multiply written in C. clang lowered it to vmulouh +
   vmsumuhm (the AltiVec idiom for targets without vmuluwm), and Xenon does not
   implement any VMX integer multiply (see vmxops): on the console the title
   raised STATUS_ILLEGAL_INSTRUCTION, and xenia stops it as an unimplemented
   instruction. The loop vectorizer produces the same multiply from ordinary
   integer loops. This is the regression test for the compiler emitting only
   instructions Xenon has; expected.txt holds the products. */

typedef unsigned int v4u __attribute__((vector_size(16)));

static const v4u a = { 0x00020003u, 0x00040005u, 0xFFFF0001u, 0x12345678u };
static const v4u b = { 0x00060007u, 0x00080009u, 0x0002FFFFu, 0x9ABCDEF0u };

/* An integer loop of the shape the loop vectorizer turns into vector multiplies. */
static unsigned dot(const unsigned *x, const unsigned *y, int n)
{
    unsigned s = 0;
    for (int i = 0; i < n; i++)
        s += x[i] * y[i];
    return s;
}

void t_vmxmul(void)
{
    volatile v4u va = a, vb = b;
    v4u r = va * vb;
    static unsigned x[64], y[64];

    DbgPrint("v4i32 mul %08x %08x %08x %08x\n", r[0], r[1], r[2], r[3]);

    for (int i = 0; i < 64; i++) {
        x[i] = 0x01000193u * (unsigned)i + 7u;
        y[i] = 0x9E3779B9u ^ (unsigned)(i * 31);
    }
    DbgPrint("vectorized dot %08x\n", dot(x, y, 64));
}
