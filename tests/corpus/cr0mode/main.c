#include "rt.h"
/* Which PowerPC computation mode do titles run in? A devkit run (2026-10-09)
   answers: the 32-bit mode (MSR[SF]=0) -- registers and 64-bit instructions
   are full width, but
     - record forms (Rc=1) set CR0 from the low 32 bits of the result;
     - bdnz tests the low 32 bits of CTR;
     - XER[OV] (and XER[CA], see carry64) report 32-bit overflow and carry;
     - effective addresses are the low 32 bits of the computed address.
   sradi's CA follows the full 64-bit operand (its sign and the bits shifted
   out), so sradi + addze stays correct. Explicit 64-bit compares (cmpd/cmpld,
   L=1) compare all 64 bits and are the control. clang must not rely on the
   64-bit-mode behaviour of the others for i64 values on the Xbox 360. The
   comments below give both modes' answers; expected.txt holds the console's.
   The address case could fault in 64-bit mode, so the debugger is asked to
   step past faults (hw-skip-faults). */

static const unsigned long long hi_only = 0x0000000100000000ULL;  /* low word 0 */
static const unsigned long long sign_lo = 0x0000000080000000ULL;  /* >0, low word <0 */
static const unsigned long long pos_lo_neg = 0x0000000080000001ULL;
static const unsigned long long neg_lo_pos = 0xFFFFFFFF00000001ULL;
static const unsigned long long ctr_init = 0x0000000100000001ULL;
static const unsigned long long int_max32 = 0x000000007FFFFFFFULL;
static const unsigned long long one = 1ULL;

static const char *cr0(unsigned cr)
{
    switch (cr >> 28 & 0xE) {
    case 8: return "LT";
    case 4: return "GT";
    case 2: return "EQ";
    default: return "??";
    }
}

/* or. x,x,x: CR0 from the result, which is x. */
static const char *or_dot(const unsigned long long *p)
{
    unsigned cr, t;
    __asm__ __volatile__("ld %1, 0(%2)\n\t"
                         "or. %1, %1, %1\n\t"
                         "mfcr %0"
                         : "=r"(cr), "=&r"(t)
                         : "b"(p)
                         : "cr0");
    return cr0(cr);
}

static const char *cmpdi0(const unsigned long long *p)
{
    unsigned cr, t;
    __asm__ __volatile__("ld %1, 0(%2)\n\t"
                         "cmpdi 0, %1, 0\n\t"
                         "mfcr %0"
                         : "=r"(cr), "=&r"(t)
                         : "b"(p)
                         : "cr0");
    return cr0(cr);
}

static unsigned sradi_ca(const unsigned long long *p)
{
    unsigned xer, t;
    __asm__ __volatile__("ld %1, 0(%2)\n\t"
                         "sradi %1, %1, 1\n\t"
                         "mfxer %0"
                         : "=r"(xer), "=&r"(t)
                         : "b"(p)
                         : "xer");
    return xer >> 29 & 1;
}

/* CTR = p - 1 after the bdnz decrement; 1 if the branch was taken. */
static unsigned bdnz_taken(const unsigned long long *p)
{
    unsigned taken, t;
    __asm__ __volatile__("ld %1, 0(%2)\n\t"
                         "mtctr %1\n\t"
                         "li %0, 1\n\t"
                         "bdnz 1f\n\t"
                         "li %0, 0\n"
                         "1:"
                         : "=&r"(taken), "=&r"(t)
                         : "b"(p)
                         : "ctr");
    return taken;
}

static unsigned addo_ov(const unsigned long long *a, const unsigned long long *b)
{
    unsigned xer, t0, t1;
    __asm__ __volatile__("li %1, 0\n\t"
                         "mtxer %1\n\t"
                         "ld %1, 0(%3)\n\t"
                         "ld %2, 0(%4)\n\t"
                         "addo %1, %1, %2\n\t"
                         "mfxer %0"
                         : "=r"(xer), "=&r"(t0), "=&r"(t1)
                         : "b"(a), "b"(b)
                         : "xer");
    return xer >> 30 & 1;
}

static volatile unsigned target_word = 0x1234ABCDu;

/* lwz through a register holding 0x00000001_xxxxxxxx, where xxxxxxxx is the
   address of target_word: 32-bit mode loads target_word, 64-bit mode faults
   (the debugger then reports the code in r0). */
static void high_address_load(void)
{
    static unsigned long long addr;
    unsigned value = 0, code;
    addr = 0x0000000100000000ULL | (unsigned)(unsigned long)&target_word;
    __asm__ __volatile__("li 0, 0\n\t"
                         "ld %1, 0(%2)\n\t"
                         "lwz %1, 0(%1)\n\t"
                         "mr %0, 0"
                         : "=&b"(code), "=&b"(value)
                         : "b"(&addr)
                         : "r0", "memory");
    if (code)
        DbgPrint("lwz via high-bit address fault 0x%08x\n", code);
    else
        DbgPrint("lwz via high-bit address loaded 0x%08x\n", value);
}

void t_cr0mode(void)
{
    /* 64-bit mode: GT. 32-bit mode: EQ (low word is 0). */
    DbgPrint("or. 0x1_00000000 CR0=%s\n", or_dot(&hi_only));
    /* 64-bit mode: GT. 32-bit mode: LT (bit 32 is the low word's sign). */
    DbgPrint("or. 0x0_80000000 CR0=%s\n", or_dot(&sign_lo));
    /* Control: a 64-bit compare is GT in both modes. */
    DbgPrint("cmpdi 0x1_00000000,0 CR0=%s\n", cmpdi0(&hi_only));
    DbgPrint("cmpdi 0x0_80000000,0 CR0=%s\n", cmpdi0(&sign_lo));
    /* sradi by 1: 64-bit view: positive -> CA=0; negative with a 1 shifted
       out -> CA=1. The low-word view gives the opposite in both cases. */
    DbgPrint("sradi 0x0_80000001,1 CA=%u\n", sradi_ca(&pos_lo_neg));
    DbgPrint("sradi 0xffffffff_00000001,1 CA=%u\n", sradi_ca(&neg_lo_pos));
    /* bdnz from 0x1_00000001: CTR becomes 0x1_00000000. 64-bit mode: nonzero,
       taken (1). 32-bit mode: low word 0, not taken (0). */
    DbgPrint("bdnz ctr=0x1_00000001 taken=%u\n", bdnz_taken(&ctr_init));
    /* 0x7fffffff + 1: 64-bit mode OV=0; 32-bit mode OV=1. */
    DbgPrint("addo 0x7fffffff+1 OV=%u\n", addo_ov(&int_max32, &one));
    high_address_load();
}
