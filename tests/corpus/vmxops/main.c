#include "rt.h"
/* Which standard AltiVec (VMX) instructions does Xenon implement? This runs
   every standard AltiVec instruction once, each between "li r0,0" and
   "mr rX,r0". The hardware runner launches it with --skip-faults: when an
   instruction faults, the debugger steps past it and puts the exception code
   in r0, so the line prints the code (0xc000001d = illegal instruction)
   instead of "ok". The operand values are arbitrary; only whether the
   instruction executes matters.

   A devkit run (2026-10-09), recorded in expected.txt: 153 execute, and the
   22 integer multiply instructions are illegal -- the even/odd multiplies
   (vmule*, vmulo*), multiply-high/low-add (vmhaddshs, vmhraddshs, vmladduhm),
   multiply-sum (vmsum*) and sum-across (vsum4*, vsum2sws, vsumsws). The
   compiler must not emit any of them for the Xbox 360. */

static const unsigned int va[4] __attribute__((aligned(16))) = {
    0x3FC00000u, 0xC0100000u, 0x40400000u, 0x3F000000u};   /* 1.5 -2.25 3 0.5 */
static const unsigned int vb[4] __attribute__((aligned(16))) = {
    0x40000000u, 0x3F400000u, 0xBF800000u, 0x41000000u};   /* 2 0.75 -1 8 */
static const unsigned int vc[4] __attribute__((aligned(16))) = {
    0x00010203u, 0x14151617u, 0x08090A0Bu, 0x1C1D1E1Fu};   /* permute control */
static unsigned int out[4] __attribute__((aligned(16)));

static void report(const char *name, unsigned code)
{
    if (code)
        DbgPrint("%s fault 0x%08x\n", name, code);
    else
        DbgPrint("%s ok\n", name);
}

/* v0 = va, v1 = vb, v2 = vc, result in v3; %1/%2/%3 address va/vb/vc and %4
   the scratch store buffer. */
#define PROBE(name, text)                                                      \
    do {                                                                       \
        unsigned code_;                                                        \
        __asm__ __volatile__("lvx 0, 0, %1\n\t"                                \
                             "lvx 1, 0, %2\n\t"                                \
                             "lvx 2, 0, %3\n\t"                                \
                             "li 0, 0\n\t" text "\n\t"                         \
                             "mr %0, 0"                                        \
                             : "=&b"(code_)                                    \
                             : "b"(va), "b"(vb), "b"(vc), "b"(out)             \
                             : "r0", "v0", "v1", "v2", "v3", "cr6", "memory"); \
        report(name, code_);                                                   \
    } while (0)

#define VX3(op)     PROBE(#op, #op " 3, 0, 1")       /* vD, vA, vB */
#define VX2(op)     PROBE(#op, #op " 3, 1")          /* vD, vB */
#define VXU(op, u)  PROBE(#op, #op " 3, 1, " #u)     /* vD, vB, UIMM */
#define VXS(op, s)  PROBE(#op, #op " 3, " #s)        /* vD, SIMM */
#define VA4(op)     PROBE(#op, #op " 3, 0, 1, 2")    /* vD, vA, vB, vC */
#define LDX(op)     PROBE(#op, #op " 3, 0, %1")      /* vD, 0, rB */
#define STX(op)     PROBE(#op, #op " 0, 0, %4")      /* vS, 0, rB */

void t_vmxops(void)
{
    /* integer add/subtract */
    VX3(vaddubm); VX3(vadduhm); VX3(vadduwm); VX3(vaddcuw);
    VX3(vaddubs); VX3(vadduhs); VX3(vadduws); VX3(vaddsbs); VX3(vaddshs); VX3(vaddsws);
    VX3(vsububm); VX3(vsubuhm); VX3(vsubuwm); VX3(vsubcuw);
    VX3(vsububs); VX3(vsubuhs); VX3(vsubuws); VX3(vsubsbs); VX3(vsubshs); VX3(vsubsws);
    /* integer max/min/average */
    VX3(vmaxub); VX3(vmaxuh); VX3(vmaxuw); VX3(vmaxsb); VX3(vmaxsh); VX3(vmaxsw);
    VX3(vminub); VX3(vminuh); VX3(vminuw); VX3(vminsb); VX3(vminsh); VX3(vminsw);
    VX3(vavgub); VX3(vavguh); VX3(vavguw); VX3(vavgsb); VX3(vavgsh); VX3(vavgsw);
    /* integer multiply */
    VX3(vmuleub); VX3(vmulesb); VX3(vmuleuh); VX3(vmulesh);
    VX3(vmuloub); VX3(vmulosb); VX3(vmulouh); VX3(vmulosh);
    /* integer multiply-add and multiply-sum */
    VA4(vmhaddshs); VA4(vmhraddshs); VA4(vmladduhm);
    VA4(vmsumubm); VA4(vmsummbm); VA4(vmsumuhm); VA4(vmsumuhs); VA4(vmsumshm); VA4(vmsumshs);
    /* integer sum across */
    VX3(vsum4ubs); VX3(vsum4sbs); VX3(vsum4shs); VX3(vsum2sws); VX3(vsumsws);
    /* logical */
    VX3(vand); VX3(vandc); VX3(vor); VX3(vnor); VX3(vxor); VA4(vsel);
    /* rotate and shift */
    VX3(vrlb); VX3(vrlh); VX3(vrlw);
    VX3(vslb); VX3(vslh); VX3(vslw); VX3(vsrb); VX3(vsrh); VX3(vsrw);
    VX3(vsrab); VX3(vsrah); VX3(vsraw);
    VX3(vsl); VX3(vsr); VX3(vslo); VX3(vsro);
    /* integer compare (and the record forms, which set CR6) */
    VX3(vcmpequb); VX3(vcmpequh); VX3(vcmpequw);
    VX3(vcmpgtub); VX3(vcmpgtuh); VX3(vcmpgtuw); VX3(vcmpgtsb); VX3(vcmpgtsh); VX3(vcmpgtsw);
    VX3(vcmpequb.); VX3(vcmpequh.); VX3(vcmpequw.);
    VX3(vcmpgtub.); VX3(vcmpgtuh.); VX3(vcmpgtuw.); VX3(vcmpgtsb.); VX3(vcmpgtsh.); VX3(vcmpgtsw.);
    /* floating point */
    VX3(vaddfp); VX3(vsubfp); VX3(vmaxfp); VX3(vminfp);
    VA4(vmaddfp); VA4(vnmsubfp);
    VX2(vrefp); VX2(vrsqrtefp); VX2(vexptefp); VX2(vlogefp);
    VX2(vrfin); VX2(vrfiz); VX2(vrfip); VX2(vrfim);
    VXU(vcfux, 1); VXU(vcfsx, 1); VXU(vctuxs, 1); VXU(vctsxs, 1);
    VX3(vcmpeqfp); VX3(vcmpgefp); VX3(vcmpgtfp); VX3(vcmpbfp);
    VX3(vcmpeqfp.); VX3(vcmpgefp.); VX3(vcmpgtfp.); VX3(vcmpbfp.);
    /* merge, pack, unpack */
    VX3(vmrghb); VX3(vmrghh); VX3(vmrghw); VX3(vmrglb); VX3(vmrglh); VX3(vmrglw);
    VX3(vpkuhum); VX3(vpkuwum); VX3(vpkuhus); VX3(vpkuwus);
    VX3(vpkshus); VX3(vpkswus); VX3(vpkshss); VX3(vpkswss); VX3(vpkpx);
    VX2(vupkhsb); VX2(vupkhsh); VX2(vupklsb); VX2(vupklsh); VX2(vupkhpx); VX2(vupklpx);
    /* splat, permute, shift by octet */
    VXU(vspltb, 3); VXU(vsplth, 3); VXU(vspltw, 3);
    VXS(vspltisb, -5); VXS(vspltish, -5); VXS(vspltisw, -5);
    VA4(vperm);
    PROBE("vsldoi", "vsldoi 3, 0, 1, 5");
    /* status and control */
    PROBE("mfvscr", "mfvscr 3");
    PROBE("mtvscr", "mfvscr 3\n\tmtvscr 3");
    /* loads and stores */
    LDX(lvx); LDX(lvxl); LDX(lvebx); LDX(lvehx); LDX(lvewx); LDX(lvsl); LDX(lvsr);
    STX(stvx); STX(stvxl); STX(stvebx); STX(stvehx); STX(stvewx);
    /* data stream touch */
    PROBE("dst", "dst %1, %2, 0");
    PROBE("dstt", "dstt %1, %2, 1");
    PROBE("dstst", "dstst %1, %2, 2");
    PROBE("dststt", "dststt %1, %2, 3");
    PROBE("dss", "dss 0");
    PROBE("dssall", "dssall");
}
