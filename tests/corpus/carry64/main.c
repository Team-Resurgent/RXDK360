#include "rt.h"
/* XER[CA] width for 64-bit operands. A devkit run (2026-10-08) shows titles
   get the carry out of the low 32 bits, as in the PowerPC 32-bit computation
   mode (MSR[SF]=0), even though the registers and 64-bit instructions are
   full width; xenia does the same (ppc_emit_alu.cc AddDidCarry/SubDidCarry).
   Code that needs the carry out of a 64-bit add or subtract -- clang's i64
   addc/adde/subfc/subfe/addze sequences, e.g. 128-bit arithmetic in picolibc's
   Ryu, or the runtime's FILETIME -> Unix time conversion -- therefore gets the
   wrong answer on the console, and the compiler must not emit it for the
   Xbox 360. The first two cases differ between 32- and 64-bit carry; the
   last two agree. expected.txt holds the console's answer. */

/* Each operand is loaded with ld so it occupies a full 64-bit register (this
   is an ILP32 target: a long long C operand would be a 32-bit register pair). */
static unsigned subfc_ca(const unsigned long long *rb, const unsigned long long *ra)
{
    unsigned xer, t0, t1;
    __asm__ __volatile__("ld %1, 0(%3)\n\t"
                         "ld %2, 0(%4)\n\t"
                         "subfc %1, %2, %1\n\t" /* rb - ra */
                         "mfxer %0"
                         : "=r"(xer), "=&r"(t0), "=&r"(t1)
                         : "b"(rb), "b"(ra)
                         : "xer");
    return (xer >> 29) & 1;
}

static unsigned addc_ca(const unsigned long long *a, const unsigned long long *b)
{
    unsigned xer, t0, t1;
    __asm__ __volatile__("ld %1, 0(%3)\n\t"
                         "ld %2, 0(%4)\n\t"
                         "addc %1, %1, %2\n\t"
                         "mfxer %0"
                         : "=r"(xer), "=&r"(t0), "=&r"(t1)
                         : "b"(a), "b"(b)
                         : "xer");
    return (xer >> 29) & 1;
}

void t_carry64(void)
{
    static const unsigned long long hi_bit = 0x0000000100000000ULL;
    static const unsigned long long one = 1ULL;
    static const unsigned long long two = 2ULL;
    static const unsigned long long low_ones = 0x00000000FFFFFFFFULL;
    static const unsigned long long all_ones = 0xFFFFFFFFFFFFFFFFULL;

    /* 64-bit: no borrow (CA=1). Low 32 bits alone: 0 - 1 borrows (CA=0). */
    DbgPrint("subfc 0x100000000-1 CA=%u\n", subfc_ca(&hi_bit, &one));
    /* 64-bit: no carry out of bit 63 (CA=0). Low 32 bits alone: carry (CA=1). */
    DbgPrint("addc 0xffffffff+1 CA=%u\n", addc_ca(&low_ones, &one));
    /* Controls: both widths agree. */
    DbgPrint("subfc 2-1 CA=%u\n", subfc_ca(&two, &one));
    DbgPrint("addc ~0+1 CA=%u\n", addc_ca(&all_ones, &one));
}
