/*
 * 2026 - Team Resurgent
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Part of RXDK - see LICENSE.md for the full GNU GPL v3.
 *
 * Coverage for the MS PowerPC/VMX128 intrinsic shim (runtime/xbox/vmx_intrinsics.c)
 * that the modern clang toolchain resolves for XDK/ATG titles. clang does not
 * provide these under the MS spellings, so a title links our out-of-line bodies;
 * this asserts they are byte/element exact against hand-computed results on the
 * big-endian PPC target (element 0 = most significant lane/byte).
 *
 * The XMVECTOR/XMMATRIX layer that titles actually build camera matrices with is
 * exercised separately (t_xnamath.cpp); this pins the primitives underneath it.
 */
#include <math.h>
#include "rxdk_test.h"

/* __vector4 laid out exactly as runtime/xbox/vmx_intrinsics.c defines it (a
 * 16-byte, 16-aligned union). We declare the intrinsic prototypes here rather
 * than pull the XDK's VectorIntrinsics.h/PPCIntrinsics.h, which need the full
 * Win32/Xbox header environment (crtdefs, __declspec(intrin_type), ...) and MS
 * compat flags the pure-libc stdlib harness deliberately does not enable. The
 * type is layout- and ABI-identical to the shim's, so these calls bind to and
 * exercise the real compiled bodies. The XDK header declares the same set atop
 * an intrin_type __vector4; that path (FastXXX/libcompat) links the same .a and
 * is proven ABI-compatible, so nothing is lost by declaring them plainly here. */
typedef struct __attribute__((aligned(16))) {
    union { float vector4_f32[4]; unsigned int vector4_u32[4]; };
} __vector4;

/* loads / partial stores / logical */
extern __vector4 __lvlx(const void*, int);
extern __vector4 __lvrx(const void*, int);
extern void      __stvewx(__vector4, void*, int);
extern __vector4 __vslo(__vector4, __vector4);
/* float arithmetic */
extern __vector4 __vaddfp(__vector4, __vector4);
extern __vector4 __vsubfp(__vector4, __vector4);
extern __vector4 __vmulfp(__vector4, __vector4);
extern __vector4 __vmaddfp(__vector4, __vector4, __vector4);
extern __vector4 __vnmsubfp(__vector4, __vector4, __vector4);
extern __vector4 __vmaxfp(__vector4, __vector4);
extern __vector4 __vminfp(__vector4, __vector4);
extern __vector4 __vmsum3fp(__vector4, __vector4);
extern __vector4 __vmsum4fp(__vector4, __vector4);
extern __vector4 __vrefp(__vector4);
extern __vector4 __vrsqrtefp(__vector4);
/* splat / permute */
extern __vector4 __vspltisw(int);
extern __vector4 __vspltisb(int);
extern __vector4 __vspltish(int);
extern __vector4 __vspltw(__vector4, unsigned);
extern __vector4 __vperm(__vector4, __vector4, __vector4);
extern __vector4 __vpermwi(__vector4, unsigned);
extern __vector4 __vrlimi(__vector4, __vector4, unsigned, unsigned);
extern __vector4 __vsldoi(__vector4, __vector4, unsigned);
/* convert */
extern __vector4 __vcfsx(__vector4, unsigned short);
extern __vector4 __vctsxs(__vector4, unsigned);
extern __vector4 __vcfux(__vector4, unsigned);
extern __vector4 __vctuxs(__vector4, unsigned);
/* float compares */
extern __vector4 __vcmpgtfp(__vector4, __vector4);
extern __vector4 __vcmpeqfp(__vector4, __vector4);
extern __vector4 __vcmpbfp(__vector4, __vector4);
/* integer logical / add-sub / average */
extern __vector4 __vand(__vector4, __vector4);
extern __vector4 __vor(__vector4, __vector4);
extern __vector4 __vxor(__vector4, __vector4);
extern __vector4 __vnor(__vector4, __vector4);
extern __vector4 __vandc(__vector4, __vector4);
extern __vector4 __vadduwm(__vector4, __vector4);
extern __vector4 __vsubuwm(__vector4, __vector4);
extern __vector4 __vavgub(__vector4, __vector4);
extern __vector4 __vadduws(__vector4, __vector4);
extern __vector4 __vsubuws(__vector4, __vector4);
/* shifts / rotate / merge */
extern __vector4 __vslw(__vector4, __vector4);
extern __vector4 __vsrw(__vector4, __vector4);
extern __vector4 __vsraw(__vector4, __vector4);
extern __vector4 __vrlw(__vector4, __vector4);
extern __vector4 __vmrghw(__vector4, __vector4);
extern __vector4 __vmrglw(__vector4, __vector4);
extern __vector4 __vmrghb(__vector4, __vector4);
/* D3D pack/unpack + round-to-integer */
extern __vector4 __vpkd3d(__vector4, __vector4, unsigned, unsigned, unsigned);
extern __vector4 __vupkd3d(__vector4, unsigned);
extern __vector4 __vrfin(__vector4);
extern __vector4 __vrfim(__vector4);
extern __vector4 __vrfip(__vector4);
extern __vector4 __vrfiz(__vector4);
/* scalar PPC */
extern double __fsel(double, double, double);
extern float  __fself(float, float, float);
extern unsigned int _CountLeadingZeros(long);
extern unsigned int _CountLeadingZeros64(long long);
extern unsigned long      __loadwordbytereverse(int, const void*);
extern unsigned short     __loadshortbytereverse(int, const void*);
extern unsigned long long __loaddoublewordbytereverse(int, const void*);
extern void               __storewordbytereverse(unsigned long, int, void*);
extern double __fsqrt(double);
extern float  __fsqrts(float);
extern double __frsqrte(double);
extern float  __fres(double);

/* ---- vector builders / checkers ---------------------------------------- */
static __vector4 vf(float a, float b, float c, float d)
{ __vector4 v; v.vector4_f32[0]=a; v.vector4_f32[1]=b; v.vector4_f32[2]=c; v.vector4_f32[3]=d; return v; }
static __vector4 vu(unsigned a, unsigned b, unsigned c, unsigned d)
{ __vector4 v; v.vector4_u32[0]=a; v.vector4_u32[1]=b; v.vector4_u32[2]=c; v.vector4_u32[3]=d; return v; }

static int equ(__vector4 v, unsigned a, unsigned b, unsigned c, unsigned d)
{ return v.vector4_u32[0]==a && v.vector4_u32[1]==b && v.vector4_u32[2]==c && v.vector4_u32[3]==d; }
static int eqf(__vector4 v, float a, float b, float c, float d, float eps)
{ return fabsf(v.vector4_f32[0]-a)<=eps && fabsf(v.vector4_f32[1]-b)<=eps
      && fabsf(v.vector4_f32[2]-c)<=eps && fabsf(v.vector4_f32[3]-d)<=eps; }

int main(void)
{
    /* ---- unaligned 16-byte load idiom: vor(lvlx(p,0), lvrx(p,16)) ------- */
    {
        static unsigned char __attribute__((aligned(16))) buf[32];
        for (int i = 0; i < 32; ++i) buf[i] = (unsigned char)i;
        /* aligned base + offset 3 -> bytes 3..18 land in the register */
        __vector4 r = __vor(__lvlx(buf, 3), __lvrx(buf, 19));
        CHECK(equ(r, 0x03040506u, 0x0708090Au, 0x0B0C0D0Eu, 0x0F101112u),
              "lvlx|lvrx unaligned load @3");
        /* __vslo: shift-left-by-octet, sh from (VRB[15]>>3)&0xF; sh=2 words? */
        __vector4 sh = __vslo(vu(0x00010203u,0x04050607u,0x08090A0Bu,0x0C0D0E0Fu),
                              vu(0,0,0,0x20u));            /* (0x20>>3)&0xF = 4 bytes */
        CHECK(equ(sh, 0x04050607u,0x08090A0Bu,0x0C0D0E0Fu,0x00000000u), "vslo 4 bytes");
        __stvewx(vu(0xDEADBEEFu,0,0,0), buf, 0);           /* word 0 -> buf[0..3] */
        CHECK(buf[0]==0xDE && buf[1]==0xAD && buf[2]==0xBE && buf[3]==0xEF, "stvewx word0");
    }

    /* ---- floating-point vector arithmetic ------------------------------ */
    CHECK(eqf(__vaddfp(vf(1,2,3,4), vf(10,20,30,40)), 11,22,33,44, 0), "vaddfp");
    CHECK(eqf(__vsubfp(vf(10,20,30,40), vf(1,2,3,4)), 9,18,27,36, 0), "vsubfp");
    CHECK(eqf(__vmulfp(vf(1,2,3,4), vf(5,6,7,8)), 5,12,21,32, 0), "vmulfp");
    CHECK(eqf(__vmaddfp(vf(1,2,3,4), vf(5,6,7,8), vf(100,100,100,100)), 105,112,121,132, 0), "vmaddfp a*b+c");
    CHECK(eqf(__vnmsubfp(vf(1,2,3,4), vf(5,6,7,8), vf(100,100,100,100)), 95,88,79,68, 0), "vnmsubfp c-a*b");
    CHECK(eqf(__vmaxfp(vf(1,9,3,7), vf(5,2,8,4)), 5,9,8,7, 0), "vmaxfp");
    CHECK(eqf(__vminfp(vf(1,9,3,7), vf(5,2,8,4)), 1,2,3,4, 0), "vminfp");
    CHECK(eqf(__vmsum3fp(vf(1,2,3,999), vf(4,5,6,999)), 32,32,32,32, 0), "vmsum3fp dot3 broadcast");
    CHECK(eqf(__vmsum4fp(vf(1,2,3,4), vf(1,1,1,1)), 10,10,10,10, 0), "vmsum4fp dot4 broadcast");
    /* reciprocal / rsqrt estimates: loose tolerance (hw is an estimate) */
    CHECK(eqf(__vrefp(vf(2,4,0.5f,1)), 0.5f,0.25f,2,1, 1e-3f), "vrefp 1/x");
    CHECK(eqf(__vrsqrtefp(vf(4,16,0.25f,1)), 0.5f,0.25f,2,1, 1e-3f), "vrsqrtefp 1/sqrt(x)");

    /* ---- splat / permute / rotate-insert ------------------------------- */
    CHECK(equ(__vspltisw(-1), 0xFFFFFFFFu,0xFFFFFFFFu,0xFFFFFFFFu,0xFFFFFFFFu), "vspltisw -1");
    CHECK(equ(__vspltisw(5), 5,5,5,5), "vspltisw 5");
    CHECK(equ(__vspltw(vu(0xAAu,0xBBu,0xCCu,0xDDu), 2), 0xCCu,0xCCu,0xCCu,0xCCu), "vspltw[2]");
    CHECK(equ(__vspltisb(0x7F), 0x7F7F7F7Fu,0x7F7F7F7Fu,0x7F7F7F7Fu,0x7F7F7F7Fu), "vspltisb");
    CHECK(equ(__vspltish(0x1234), 0x12341234u,0x12341234u,0x12341234u,0x12341234u), "vspltish");
    {
        __vector4 a = vu(0x00010203u,0x04050607u,0x08090A0Bu,0x0C0D0E0Fu);
        __vector4 b = vu(0x10111213u,0x14151617u,0x18191A1Bu,0x1C1D1E1Fu);
        __vector4 ctl = vu(0x0F0E0D0Cu,0x0B0A0908u,0x07060504u,0x03020100u); /* reverse a */
        CHECK(equ(__vperm(a,b,ctl), 0x0F0E0D0Cu,0x0B0A0908u,0x07060504u,0x03020100u), "vperm reverse");
        CHECK(equ(__vpermwi(vu(0x11u,0x22u,0x33u,0x44u), 0xE4u), 0x44u,0x33u,0x22u,0x11u), "vpermwi reverse words");
        CHECK(equ(__vrlimi(vu(0,0,0,0), vu(0x11u,0x22u,0x33u,0x44u), 0xFu, 1), 0x22u,0x33u,0x44u,0x11u), "vrlimi rot1 all");
        CHECK(equ(__vsldoi(a,b,4), 0x04050607u,0x08090A0Bu,0x0C0D0E0Fu,0x10111213u), "vsldoi shb=4");
    }

    /* ---- fixed<->float convert ----------------------------------------- */
    CHECK(eqf(__vcfsx(vu(4,8,16,2), 1), 2,4,8,1, 0), "vcfsx /2");
    CHECK(equ(__vctsxs(vf(2.5f,4.0f,-3.0f,1.0f), 1), 5,8,(unsigned)-6,2), "vctsxs *2 sat");
    CHECK(eqf(__vcfux(vu(10,20,30,40), 1), 5,10,15,20, 0), "vcfux /2");
    CHECK(equ(__vctuxs(vf(2.5f,3.0f,4.0f,1.0f), 1), 5,6,8,2), "vctuxs *2");

    /* ---- float compares (all-ones per element where true) -------------- */
    CHECK(equ(__vcmpgtfp(vf(2,1,5,3), vf(1,2,5,0)), 0xFFFFFFFFu,0,0,0xFFFFFFFFu), "vcmpgtfp");
    CHECK(equ(__vcmpeqfp(vf(1,2,3,4), vf(1,9,3,9)), 0xFFFFFFFFu,0,0xFFFFFFFFu,0), "vcmpeqfp");
    CHECK(equ(__vcmpbfp(vf(5,-5,3,-3), vf(4,4,4,4)), 0x80000000u,0x40000000u,0,0), "vcmpbfp bounds");

    /* ---- integer logical / modulo add-sub / averages ------------------- */
    CHECK(equ(__vand (vu(0xFFu,0xFFu,0xFFu,0xFFu), vu(0x0Fu,0xF0u,0x00u,0xFFu)), 0x0Fu,0xF0u,0x00u,0xFFu), "vand");
    CHECK(equ(__vor  (vu(0xF0u,0x0Fu,0x00u,0x00u), vu(0x0Fu,0xF0u,0xAAu,0x00u)), 0xFFu,0xFFu,0xAAu,0x00u), "vor");
    CHECK(equ(__vxor (vu(0xFFu,0xAAu,0x00u,0xF0u), vu(0x0Fu,0xAAu,0x00u,0x0Fu)), 0xF0u,0x00u,0x00u,0xFFu), "vxor");
    CHECK(equ(__vnor (vu(0,0,0xFFFFFFFFu,0), vu(0,0xFFFFFFFFu,0,0)), 0xFFFFFFFFu,0,0,0xFFFFFFFFu), "vnor");
    CHECK(equ(__vandc(vu(0xFFu,0xFFu,0xFFu,0xFFu), vu(0x0Fu,0xF0u,0x00u,0xFFu)), 0xF0u,0x0Fu,0xFFu,0x00u), "vandc a&~b");
    CHECK(equ(__vadduwm(vu(1,2,3,0xFFFFFFFFu), vu(10,20,30,1)), 11,22,33,0), "vadduwm wrap");
    CHECK(equ(__vsubuwm(vu(10,20,30,0), vu(1,2,3,1)), 9,18,27,0xFFFFFFFFu), "vsubuwm wrap");
    CHECK(equ(__vavgub(vu(0x0A0A0A0Au,0x0A0A0A0Au,0x0A0A0A0Au,0x0A0A0A0Au),
                       vu(0x14141414u,0x14141414u,0x14141414u,0x14141414u)),
              0x0F0F0F0Fu,0x0F0F0F0Fu,0x0F0F0F0Fu,0x0F0F0F0Fu), "vavgub round");
    CHECK(equ(__vadduws(vu(0xFFFFFFFFu,1,0,0), vu(1,2,3,4)), 0xFFFFFFFFu,3,3,4), "vadduws sat");
    CHECK(equ(__vsubuws(vu(5,0,10,0), vu(3,1,2,0)), 2,0,8,0), "vsubuws sat");

    /* ---- per-element shifts / rotate ----------------------------------- */
    CHECK(equ(__vslw(vu(1,2,4,0x80000000u), vu(1,2,3,1)), 2,8,32,0), "vslw");
    CHECK(equ(__vsrw(vu(2,8,32,1), vu(1,2,3,1)), 1,2,4,0), "vsrw");
    CHECK(equ(__vsraw(vu((unsigned)-8,16,(unsigned)-1,4), vu(1,2,1,1)), 0xFFFFFFFCu,4,0xFFFFFFFFu,2), "vsraw arith");
    CHECK(equ(__vrlw(vu(0x80000001u,1,0,0xF0000000u), vu(1,4,5,8)), 0x00000003u,0x10u,0,0xF0u), "vrlw rotate");

    /* ---- merges -------------------------------------------------------- */
    CHECK(equ(__vmrghw(vu(0x11u,0x22u,0x33u,0x44u), vu(0xAAu,0xBBu,0xCCu,0xDDu)), 0x11u,0xAAu,0x22u,0xBBu), "vmrghw");
    CHECK(equ(__vmrglw(vu(0x11u,0x22u,0x33u,0x44u), vu(0xAAu,0xBBu,0xCCu,0xDDu)), 0x33u,0xCCu,0x44u,0xDDu), "vmrglw");
    CHECK(equ(__vmrghb(vu(0x00010203u,0x04050607u,0x08090A0Bu,0x0C0D0E0Fu),
                       vu(0x10111213u,0x14151617u,0x18191A1Bu,0x1C1D1E1Fu)),
              0x00100111u,0x02120313u,0x04140515u,0x06160717u), "vmrghb");

    /* ---- D3D vertex pack / unpack -------------------------------------- */
    CHECK(equ(__vpkd3d(vu(0,0,0,0), vf(255,128,64,32), 0 /*D3DCOLOR*/, 1 /*32*/, 0), 0x20FF8040u,0,0,0), "vpkd3d D3DCOLOR ARGB");
    CHECK(eqf(__vupkd3d(vu(0x20FF8040u,0,0,0), 0 /*D3DCOLOR*/), 255,128,64,32, 0), "vupkd3d D3DCOLOR");
    /* FLOAT16_4 pack lands two words; unpack recovers the four floats */
    CHECK(equ(__vpkd3d(vu(0,0,0,0), vf(1,2,0.5f,-3), 5 /*FLOAT16_4*/, 2 /*64lo*/, 0), 0x3C004000u,0x3800C200u,0,0), "vpkd3d FLOAT16_4");
    CHECK(eqf(__vupkd3d(vu(0x3C004000u,0x3800C200u,0,0), 5 /*FLOAT16_4*/), 1,2,0.5f,-3, 1e-3f), "vupkd3d FLOAT16_4");

    /* ---- round-to-integer (result stays float) ------------------------- */
    CHECK(eqf(__vrfin(vf(1.4f,1.6f,-1.4f,-1.6f)), 1,2,-1,-2, 0), "vrfin nearest");
    CHECK(eqf(__vrfim(vf(1.9f,-1.1f,2.0f,-2.5f)), 1,-2,2,-3, 0), "vrfim floor");
    CHECK(eqf(__vrfip(vf(1.1f,-1.9f,2.0f,-2.5f)), 2,-1,2,-2, 0), "vrfip ceil");
    CHECK(eqf(__vrfiz(vf(1.9f,-1.9f,2.5f,-2.5f)), 1,-1,2,-2, 0), "vrfiz trunc");

    /* ---- scalar PPC intrinsics ----------------------------------------- */
    CHECK(__fsel(1.0, 10.0, 20.0) == 10.0, "fsel >=0");
    CHECK(__fsel(-1.0, 10.0, 20.0) == 20.0, "fsel <0");
    CHECK(__fsel(0.0, 10.0, 20.0) == 10.0, "fsel 0 -> GE");
    CHECK(__fself(-0.5f, 3.0f, 7.0f) == 7.0f, "fself <0");
    CHECK_EQI(_CountLeadingZeros(1), 31, "clz 1");
    CHECK_EQI(_CountLeadingZeros(0), 32, "clz 0");
    CHECK_EQI(_CountLeadingZeros(0x80000000u), 0, "clz msb");
    CHECK_EQI(_CountLeadingZeros64(1), 63, "clz64 1");
    CHECK_EQI(_CountLeadingZeros64(0), 64, "clz64 0");
    {
        unsigned char br[8] = {0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88};
        CHECK_EQI(__loadwordbytereverse(0, br), 0x44332211u, "loadwordbytereverse");
        CHECK_EQI(__loadshortbytereverse(0, br), 0x2211u, "loadshortbytereverse");
        CHECK(__loaddoublewordbytereverse(0, br) == 0x8877665544332211ull, "loaddoublewordbytereverse");
        unsigned char wb[4] = {0,0,0,0};
        __storewordbytereverse(0x11223344u, 0, wb);
        CHECK(wb[0]==0x44 && wb[1]==0x33 && wb[2]==0x22 && wb[3]==0x11, "storewordbytereverse");
    }
    CHECK(fabs(__fsqrt(16.0) - 4.0) <= 1e-9, "fsqrt 16");
    CHECK(fabsf(__fsqrts(25.0f) - 5.0f) <= 1e-4f, "fsqrts 25");
    /* frsqrte/fres are the PowerPC reciprocal-(sqrt) ESTIMATE instructions:
     * the architecture only guarantees ~1/32 (frsqrte) / ~1/256 (fres) relative
     * accuracy, so a correct result can sit several percent off exact. The band
     * here confirms the wrapper emits the right instruction and returns a usable
     * seed (catches a zero/NaN/garbage result) without being brittle on the
     * estimate's low precision. */
    CHECK(fabs(__frsqrte(4.0) - 0.5) <= 0.5 * 0.0625, "frsqrte 4 (est ~6%)");
    CHECK(fabsf((float)__fres(4.0) - 0.25f) <= 0.25f * 0.0625f, "fres 4 (est ~6%)");

    CHECK_DONE("vmx");
    return 0;
}
