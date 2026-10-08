#include "rt.h"
/* XER[CA] width. Titles run in 64-bit mode (MSR[SF]=1), where the PowerPC
   architecture sets CA from the carry out of the full 64-bit operation. xenia
   computes it from the low 32 bits (ppc_emit_alu.cc AddDidCarry/SubDidCarry),
   which breaks compiled 64-bit compare-and-select sequences -- e.g. the
   runtime's FILETIME -> Unix time conversion (subfc/addze) returned 0. The
   first two cases differ between 32- and 64-bit carry; the last two agree.
   expected.txt holds the 64-bit-mode answer. */

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
