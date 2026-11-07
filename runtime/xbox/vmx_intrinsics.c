/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * RXDK-360 VMX/PPC intrinsic shim for the modern clang toolchain.
 *
 * The stock XDK headers use a handful of Microsoft PowerPC/VMX128 intrinsics
 * that clang does not provide under the MS spellings (they surface as
 * unresolved externals when linking XDK/ATG code). Most are implemented here
 * with portable, spec-faithful byte semantics - correct on the big-endian PPC
 * target, at a scalar cost (matches the _XM_NO_INTRINSICS_ math path). The
 * partial vector load/stores (lvlx/lvrx/stvlx/stvrx) instead emit the real
 * Xenon instructions via inline asm, now that the assembler recognizes them, so
 * they touch exactly the bytes they own. These have C linkage, matching the XDK
 * header declarations.
 *
 * References: PowerPC VMX (AltiVec) instruction semantics for lvlx/lvrx/stvewx/
 * vor/vslo, and the Xenon dcbz128 cache op.
 */
#include <stdint.h>

/* Mirror the XDK's __vector4 (vectorintrinsics.h): a 16-byte, 16-aligned union.
 * Kept layout-compatible so the ABI matches the XDK-header prototypes. */
typedef struct __attribute__((aligned(16))) __vector4 {
    union {
        float        vector4_f32[4];
        unsigned int vector4_u32[4];
    };
} __vector4;

/* A native 16-byte vector, used only to marshal a __vector4 in/out of a vector
 * register for the real VMX load/store instructions below (lvlx/lvrx/stvlx/
 * stvrx), which clang has no builtin for. Layout-compatible with __vector4. */
typedef __attribute__((__vector_size__(16), __aligned__(16))) float __rxvf4_t;

/* Load Vector Left Indexed: EA = base+offset. Load the bytes from EA up to the
 * next 16-byte boundary, left-justified into the register; the rest are zero.
 * (Paired with __lvrx + __vor this performs an unaligned 16-byte load.)
 * Emit the real lvlx: it reads only [EA, boundary) in hardware. RA (base) must
 * be a nonzero base register -> "b"; RB (offset) -> "r"; EA = base + offset. */
__vector4 __lvlx(const void* base, int offset)
{
    union { __rxvf4_t v; __vector4 s; } u;
    __asm__ volatile("lvlx %0, %1, %2" : "=v"(u.v) : "b"(base), "r"(offset) : "memory");
    return u.s;
}

/* Load Vector Right Indexed: EA = base+offset. Load the bytes from the previous
 * 16-byte boundary up to EA, right-justified into the register; rest zero. The
 * real lvrx touches nothing when EA is 16-byte aligned. */
__vector4 __lvrx(const void* base, int offset)
{
    union { __rxvf4_t v; __vector4 s; } u;
    __asm__ volatile("lvrx %0, %1, %2" : "=v"(u.v) : "b"(base), "r"(offset) : "memory");
    return u.s;
}

/* Store Vector Element Word Indexed: store the word element selected by EA's
 * bits [2:3] to the word-aligned address at EA. */
void __stvewx(__vector4 vSrc, void* base, int offset)
{
    uintptr_t ea = (uintptr_t)base + (uintptr_t)offset;
    unsigned widx = (unsigned)((ea >> 2) & 3u);
    *(unsigned int*)(ea & ~(uintptr_t)3) = vSrc.vector4_u32[widx];
}

/* Vector logical OR. */
__vector4 __vor(__vector4 a, __vector4 b)
{
    __vector4 r;
    for (unsigned i = 0; i < 4u; ++i) r.vector4_u32[i] = a.vector4_u32[i] | b.vector4_u32[i];
    return r;
}

/* Vector Shift Left by Octet: shift VRA left by sh bytes, where sh is bits
 * [121:124] of VRB (i.e. (low byte >> 3) & 0xF); zero-fill the low bytes. */
__vector4 __vslo(__vector4 a, __vector4 b)
{
    const unsigned char* bb = (const unsigned char*)&b;
    unsigned sh = (unsigned)((bb[15] >> 3) & 0xF);
    const unsigned char* ab = (const unsigned char*)&a;
    __vector4 r;
    unsigned char* o = (unsigned char*)&r;
    for (unsigned i = 0; i < 16u; ++i) o[i] = (i + sh < 16u) ? ab[i + sh] : 0;
    return r;
}

/* Data Cache Block Zero (128-byte line): zero the 128 bytes of the cache line
 * containing base+offset. */
void __dcbz128(int offset, void* base)
{
    uintptr_t a = ((uintptr_t)base + (uintptr_t)offset) & ~(uintptr_t)127;
    unsigned char* p = (unsigned char*)a;
    for (unsigned i = 0; i < 128u; ++i) p[i] = 0;
}

/* Count leading zeros of a 64-bit value (returns 64 for zero). */
unsigned int _CountLeadingZeros64(long long val)
{
    return val ? (unsigned int)__builtin_clzll((unsigned long long)val) : 64u;
}

/* Count leading zeros of a 32-bit value (returns 32 for zero). */
unsigned int _CountLeadingZeros(long val)
{
    return val ? (unsigned int)__builtin_clz((unsigned int)val) : 32u;
}

/* ---- scalar PPC intrinsics (ppcintrinsics.h) ---------------------------- */

/* Cache-block management (ppcintrinsics.h). These name real Xenon cache
 * instructions, so emit them -- on hardware they have observable effect and
 * titles that call them (e.g. flushing a buffer before the GPU/XMA DMA reads
 * it) depend on it. Only __dcbt is advisory (a prefetch hint), but emit it too
 * for fidelity. The effective address is base+offset; the dcb* forms take
 * (RA,RB) and RA==0 means the EA is RB alone. The "memory" clobber stops the
 * compiler reordering the surrounding loads/stores across the cache op, which
 * is the entire reason the title issued it.
 *   __dcbt  dcbt  - touch: prefetch the line (hint).
 *   __dcbf  dcbf  - flush: write the line back if dirty AND invalidate it.
 *   __dcbst dcbst - store: write the line back to coherency, no invalidate.
 *   __dcbz  dcbz  - zero:  set the cache block to zero.
 * (__dcbz128 above is the 128-byte-line variant and already zeroes its block.) */
void __dcbt(int offset, const void* base)
{
    __asm__ __volatile__("dcbt 0,%0" :: "r"((const char*)base + offset) : "memory");
}
void __dcbf(int offset, const void* base)
{
    __asm__ __volatile__("dcbf 0,%0" :: "r"((const char*)base + offset) : "memory");
}
void __dcbst(int offset, const void* base)
{
    __asm__ __volatile__("dcbst 0,%0" :: "r"((const char*)base + offset) : "memory");
}
void __dcbz(int offset, void* base)
{
    __asm__ __volatile__("dcbz 0,%0" :: "r"((char*)base + offset) : "memory");
}

/* Move From Time Base: the 64-bit Xenon time base. Read hi/lo/hi with a retry so
 * a low-word wrap between the two reads is not observed. */
unsigned long long __mftb(void)
{
    unsigned int hi, lo, hi2;
    do {
        __asm__ volatile("mftbu %0" : "=r"(hi));
        __asm__ volatile("mftb  %0" : "=r"(lo));
        __asm__ volatile("mftbu %0" : "=r"(hi2));
    } while (hi != hi2);
    return ((unsigned long long)hi << 32) | lo;
}

/* Byte-reversed (little-endian) loads: read the big-endian bytes at base+offset
 * and assemble them low-byte-first. */
unsigned long __loadwordbytereverse(int offset, const void* base)
{
    const unsigned char* p = (const unsigned char*)base + offset;
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8)
         | ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}
unsigned short __loadshortbytereverse(int offset, const void* base)
{
    const unsigned char* p = (const unsigned char*)base + offset;
    return (unsigned short)(p[0] | (p[1] << 8));
}
unsigned long long __loaddoublewordbytereverse(int offset, const void* base)
{
    const unsigned char* p = (const unsigned char*)base + offset;
    unsigned long long v = 0;
    for (int i = 0; i < 8; ++i) v |= (unsigned long long)p[i] << (8 * i);
    return v;
}
void __storedoublewordbytereverse(unsigned long long v, int offset, void* base)
{
    unsigned char* p = (unsigned char*)base + offset;
    for (int i = 0; i < 8; ++i) p[i] = (unsigned char)(v >> (8 * i));
}

/* Volatile variants (vectorintrinsics.h / ppcintrinsics.h declare these extern,
   unlike the __forceinline non-volatile __lvx/__stvx). The `volatile` conveys
   "do not reorder or elide" -- the memory transfer itself is identical, so the
   byte-reverse load/store bodies are reused, and __stvx_volatile does a straight
   16-byte vector store. Marking the access through a volatile pointer keeps the
   compiler from hoisting or dropping it (write-combined framebuffer writes rely
   on this; FastUntile). */
unsigned long long __loadvolatiledoublewordbytereverse(int offset, const void* base)
{
    const volatile unsigned char* p = (const volatile unsigned char*)base + offset;
    unsigned long long v = 0;
    for (int i = 0; i < 8; ++i) v |= (unsigned long long)p[i] << (8 * i);
    return v;
}
void __storevolatiledoublewordbytereverse(unsigned long long v, int offset, void* base)
{
    volatile unsigned char* p = (volatile unsigned char*)base + offset;
    for (int i = 0; i < 8; ++i) p[i] = (unsigned char)(v >> (8 * i));
}

__vector4 __lvx_volatile(const volatile void* base, int offset)
{
    __vector4 r;
    const volatile unsigned char* p = (const volatile unsigned char*)base + offset;
    for (int i = 0; i < 16; ++i) ((unsigned char*)&r)[i] = p[i];
    return r;
}
void __stvx_volatile(__vector4 vSrc, volatile void* base, int offset)
{
    volatile unsigned char* p = (volatile unsigned char*)base + offset;
    for (int i = 0; i < 16; ++i) p[i] = ((const unsigned char*)&vSrc)[i];
}
void __storewordbytereverse(unsigned long v, int offset, void* base)
{
    unsigned char* p = (unsigned char*)base + offset;
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}
void __storeshortbytereverse(unsigned short v, int offset, void* base)
{
    unsigned char* p = (unsigned char*)base + offset;
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
}

/* Floating select: fComparand >= 0 ? fValGE : fLT (the fsel instruction). */
double __fsel(double fComparand, double fValGE, double fLT)
{
    return fComparand >= 0.0 ? fValGE : fLT;
}

/* __emit(opcode) issues a raw instruction word. The XDK headers invoke it with
 * exactly four constant opcodes -- the memory barriers __sync/__lwsync/__eieio
 * and the YieldProcessor() SMT hint (ppcintrinsics.h) -- so decode each to the
 * precise instruction its opcode encodes instead of collapsing them all to a
 * full barrier. The old blanket __sync_synchronize() over-serialized __lwsync()
 * and __eieio() (a full sync where a lighter fence sufficed) and, worse, turned
 * YieldProcessor() into a barrier rather than an SMT thread-priority yield. Any
 * opcode not in the set falls back to a full sync -- conservative, and matching
 * the previous behavior. Each emits the exact word the header specifies:
 *   0x7C0004AC sync | 0x7C2004AC lwsync | 0x7C0006AC eieio | 0x7FFFFB78 or 31,31,31 */
void __emit(unsigned int opcode)
{
    switch (opcode) {
    case 0x7C0004ACu: __asm__ __volatile__("sync"        ::: "memory"); break; /* __sync()          */
    case 0x7C2004ACu: __asm__ __volatile__("lwsync"      ::: "memory"); break; /* __lwsync()        */
    case 0x7C0006ACu: __asm__ __volatile__("eieio"       ::: "memory"); break; /* __eieio()         */
    case 0x7FFFFB78u: __asm__ __volatile__("or 31,31,31" ::: "memory"); break; /* YieldProcessor()  */
    default:          __sync_synchronize();                             break; /* unknown: full sync */
    }
}

/* ---- VMX floating-point vector ops (vectorintrinsics.h) ----------------- */

__vector4 __vaddfp(__vector4 a, __vector4 b)
{
    __vector4 r;
    for (unsigned i = 0; i < 4u; ++i) r.vector4_f32[i] = a.vector4_f32[i] + b.vector4_f32[i];
    return r;
}
__vector4 __vsubfp(__vector4 a, __vector4 b)
{
    __vector4 r;
    for (unsigned i = 0; i < 4u; ++i) r.vector4_f32[i] = a.vector4_f32[i] - b.vector4_f32[i];
    return r;
}
__vector4 __vmaddfp(__vector4 a, __vector4 b, __vector4 c)   /* a*b + c */
{
    __vector4 r;
    for (unsigned i = 0; i < 4u; ++i) r.vector4_f32[i] = a.vector4_f32[i] * b.vector4_f32[i] + c.vector4_f32[i];
    return r;
}
__vector4 __vnmsubfp(__vector4 a, __vector4 b, __vector4 c)  /* c - a*b */
{
    __vector4 r;
    for (unsigned i = 0; i < 4u; ++i) r.vector4_f32[i] = c.vector4_f32[i] - a.vector4_f32[i] * b.vector4_f32[i];
    return r;
}
__vector4 __vmsum3fp(__vector4 a, __vector4 b)               /* 3-way dot, broadcast */
{
    /* Real Xenon vmsum3fp128 (distinct VMX128 instruction, xo 0x190): a 3-lane
       dot, not a 4-lane sum with w zeroed. A scalar sum would round differently. */
    union { __rxvf4_t v; __vector4 s; } ua, ub, ur;
    ua.s = a; ub.s = b;
    __asm__("vmsum3fp128 %0, %1, %2" : "=v"(ur.v) : "v"(ua.v), "v"(ub.v));
    return ur.s;
}
__vector4 __vmsum4fp(__vector4 a, __vector4 b)               /* 4-way dot, broadcast */
{
    /* Real Xenon vmsum4fp128: one instruction, fused multiply-sum of all four
       lanes, broadcast to every lane. A scalar sum would round differently. */
    union { __rxvf4_t v; __vector4 s; } ua, ub, ur;
    ua.s = a; ub.s = b;
    __asm__("vmsum4fp128 %0, %1, %2" : "=v"(ur.v) : "v"(ua.v), "v"(ub.v));
    return ur.s;
}
__vector4 __vmulfp(__vector4 a, __vector4 b)
{ __vector4 r; for (unsigned i=0;i<4u;++i) r.vector4_f32[i]=a.vector4_f32[i]*b.vector4_f32[i]; return r; }
__vector4 __vmaxfp(__vector4 a, __vector4 b)
{ __vector4 r; for (unsigned i=0;i<4u;++i) r.vector4_f32[i]=a.vector4_f32[i]>b.vector4_f32[i]?a.vector4_f32[i]:b.vector4_f32[i]; return r; }
__vector4 __vminfp(__vector4 a, __vector4 b)
{ __vector4 r; for (unsigned i=0;i<4u;++i) r.vector4_f32[i]=a.vector4_f32[i]<b.vector4_f32[i]?a.vector4_f32[i]:b.vector4_f32[i]; return r; }
__vector4 __vand(__vector4 a, __vector4 b)
{ __vector4 r; for (unsigned i=0;i<4u;++i) r.vector4_u32[i]=a.vector4_u32[i]&b.vector4_u32[i]; return r; }
__vector4 __vxor(__vector4 a, __vector4 b)
{ __vector4 r; for (unsigned i=0;i<4u;++i) r.vector4_u32[i]=a.vector4_u32[i]^b.vector4_u32[i]; return r; }
__vector4 __vrefp(__vector4 b)                              /* reciprocal estimate */
{ __vector4 r; for (unsigned i=0;i<4u;++i) r.vector4_f32[i]=1.0f/b.vector4_f32[i]; return r; }
__vector4 __vrsqrtefp(__vector4 b)                          /* reciprocal sqrt estimate */
{ __vector4 r; for (unsigned i=0;i<4u;++i) r.vector4_f32[i]=1.0f/__builtin_sqrtf(b.vector4_f32[i]); return r; }

/* Splat immediate signed word: every word = the (already sign-extended) SIM. */
__vector4 __vspltisw(int sim)
{ __vector4 r; for (unsigned i=0;i<4u;++i) r.vector4_u32[i]=(unsigned)sim; return r; }
/* Splat word: every word = VRB[uim & 3]. */
__vector4 __vspltw(__vector4 b, unsigned uim)
{ __vector4 r; unsigned s=b.vector4_u32[uim&3u]; for (unsigned i=0;i<4u;++i) r.vector4_u32[i]=s; return r; }

/* Convert from signed fixed-point: (float)(int)word / 2^shift. */
__vector4 __vcfsx(__vector4 b, unsigned short shift)
{ __vector4 r; float d=(float)(1u<<shift); for (unsigned i=0;i<4u;++i) r.vector4_f32[i]=(float)(int)b.vector4_u32[i]/d; return r; }
/* Convert to signed fixed-point, saturating: (int)(float * 2^shift). */
__vector4 __vctsxs(__vector4 b, unsigned shift)
{
    __vector4 r; float m=(float)(1u<<shift);
    for (unsigned i=0;i<4u;++i) {
        double v = (double)b.vector4_f32[i]*m;
        if (v >  2147483647.0) v =  2147483647.0;
        if (v < -2147483648.0) v = -2147483648.0;
        r.vector4_u32[i] = (unsigned)(int)v;
    }
    return r;
}

/* Byte permute: result byte i = {VRA:VRB}[control_byte_i & 0x1F] (big-endian). */
__vector4 __vperm(__vector4 a, __vector4 b, __vector4 c)
{
    const unsigned char* ab=(const unsigned char*)&a; const unsigned char* bb=(const unsigned char*)&b;
    const unsigned char* cb=(const unsigned char*)&c; __vector4 r; unsigned char* o=(unsigned char*)&r;
    for (unsigned i=0;i<16u;++i) { unsigned idx=cb[i]&0x1Fu; o[i]=idx<16u?ab[idx]:bb[idx-16u]; }
    return r;
}
/* Word permute immediate: 2 bits per output word (word 0 in bits [7:6]). */
__vector4 __vpermwi(__vector4 a, unsigned imm)
{ __vector4 r; for (unsigned i=0;i<4u;++i) r.vector4_u32[i]=a.vector4_u32[(imm>>(6u-2u*i))&3u]; return r; }
/* Rotate VRB left by SHW words, insert into VRT where WMASK (4-bit, 0x8=word0) is set. */
__vector4 __vrlimi(__vector4 t, __vector4 b, unsigned wmask, unsigned shw)
{
    __vector4 r;
    for (unsigned i=0;i<4u;++i)
        r.vector4_u32[i] = (wmask & (0x8u>>i)) ? b.vector4_u32[(i+shw)&3u] : t.vector4_u32[i];
    return r;
}

/* MSVC compiler-scheduling barriers. Titles declare these with
   `#pragma intrinsic(_WriteBarrier)`; MSVC emits them inline as pure
   reorder fences (no code). clang ignores the pragma and leaves a real call,
   so provide the family out-of-line -- the opaque call plus the "memory"
   clobber is exactly the compiler barrier the intrinsic promises. These are
   compiler-only (not hardware) fences, matching MSVC's semantics. */
void _WriteBarrier(void)     { __asm__ __volatile__("" ::: "memory"); }
void _ReadBarrier(void)      { __asm__ __volatile__("" ::: "memory"); }
void _ReadWriteBarrier(void) { __asm__ __volatile__("" ::: "memory"); }

/* ---- integer VMX: averages, shifts, compares, merges ---------------------
 * Byte-exact, big-endian element order (byte 0 = most significant). Declared in
 * vectorintrinsics.h; the modern clang lowers the MS spellings to these calls.
 * FastBlockCompress (BC1/DXT anchor selection) uses the byte forms; the halfword
 * and word siblings are provided for a complete integer set. */

/* Average, unsigned, rounding: (a+b+1)>>1 per element. */
__vector4 __vavgub(__vector4 a, __vector4 b)
{
    __vector4 r; const unsigned char* pa=(const unsigned char*)&a; const unsigned char* pb=(const unsigned char*)&b;
    unsigned char* o=(unsigned char*)&r;
    for (unsigned i=0;i<16u;++i) o[i]=(unsigned char)(((unsigned)pa[i]+(unsigned)pb[i]+1u)>>1);
    return r;
}
__vector4 __vavguh(__vector4 a, __vector4 b)
{
    __vector4 r; const unsigned short* pa=(const unsigned short*)&a; const unsigned short* pb=(const unsigned short*)&b;
    unsigned short* o=(unsigned short*)&r;
    for (unsigned i=0;i<8u;++i) o[i]=(unsigned short)(((unsigned)pa[i]+(unsigned)pb[i]+1u)>>1);
    return r;
}
__vector4 __vavguw(__vector4 a, __vector4 b)
{
    __vector4 r;
    for (unsigned i=0;i<4u;++i)
        r.vector4_u32[i]=(unsigned int)(((unsigned long long)a.vector4_u32[i]+b.vector4_u32[i]+1ull)>>1);
    return r;
}

/* Shift Left Double by Octet Immediate: 16 bytes of a:b starting at byte `shb`. */
__vector4 __vsldoi(__vector4 a, __vector4 b, unsigned shb)
{
    unsigned char cat[32]; const unsigned char* pa=(const unsigned char*)&a; const unsigned char* pb=(const unsigned char*)&b;
    for (unsigned i=0;i<16u;++i){ cat[i]=pa[i]; cat[16u+i]=pb[i]; }
    __vector4 r; unsigned char* o=(unsigned char*)&r; shb&=0x1Fu;
    for (unsigned i=0;i<16u;++i) o[i]=cat[shb+i<32u?shb+i:31u];
    return r;
}

/* Compare Greater Than, signed/unsigned byte: 0xFF where a>b else 0x00. */
__vector4 __vcmpgtsb(__vector4 a, __vector4 b)
{
    __vector4 r; const signed char* pa=(const signed char*)&a; const signed char* pb=(const signed char*)&b;
    unsigned char* o=(unsigned char*)&r;
    for (unsigned i=0;i<16u;++i) o[i]=pa[i]>pb[i]?0xFFu:0x00u;
    return r;
}
__vector4 __vcmpgtub(__vector4 a, __vector4 b)
{
    __vector4 r; const unsigned char* pa=(const unsigned char*)&a; const unsigned char* pb=(const unsigned char*)&b;
    unsigned char* o=(unsigned char*)&r;
    for (unsigned i=0;i<16u;++i) o[i]=pa[i]>pb[i]?0xFFu:0x00u;
    return r;
}

/* Merge low/high bytes: interleave the low (bytes 8..15) or high (0..7) halves. */
__vector4 __vmrglb(__vector4 a, __vector4 b)
{
    __vector4 r; const unsigned char* pa=(const unsigned char*)&a; const unsigned char* pb=(const unsigned char*)&b;
    unsigned char* o=(unsigned char*)&r;
    for (unsigned i=0;i<8u;++i){ o[2u*i]=pa[8u+i]; o[2u*i+1u]=pb[8u+i]; }
    return r;
}
__vector4 __vmrghb(__vector4 a, __vector4 b)
{
    __vector4 r; const unsigned char* pa=(const unsigned char*)&a; const unsigned char* pb=(const unsigned char*)&b;
    unsigned char* o=(unsigned char*)&r;
    for (unsigned i=0;i<8u;++i){ o[2u*i]=pa[i]; o[2u*i+1u]=pb[i]; }
    return r;
}

/* ---- VMX float compares (all-ones per element where the relation holds) --- */
static inline unsigned _f_gt(float a, float b){ return a >  b ? 0xFFFFFFFFu : 0u; }
static inline unsigned _f_ge(float a, float b){ return a >= b ? 0xFFFFFFFFu : 0u; }
static inline unsigned _f_eq(float a, float b){ return a == b ? 0xFFFFFFFFu : 0u; }
__vector4 __vcmpgtfp(__vector4 a, __vector4 b)
{ __vector4 r; for(unsigned i=0;i<4u;++i) r.vector4_u32[i]=_f_gt(a.vector4_f32[i],b.vector4_f32[i]); return r; }
__vector4 __vcmpgefp(__vector4 a, __vector4 b)
{ __vector4 r; for(unsigned i=0;i<4u;++i) r.vector4_u32[i]=_f_ge(a.vector4_f32[i],b.vector4_f32[i]); return r; }
__vector4 __vcmpeqfp(__vector4 a, __vector4 b)
{ __vector4 r; for(unsigned i=0;i<4u;++i) r.vector4_u32[i]=_f_eq(a.vector4_f32[i],b.vector4_f32[i]); return r; }
/* Bounds compare: bit31 = a > b (out above), bit30 = a < -b (out below); 0 => in bounds. */
__vector4 __vcmpbfp(__vector4 a, __vector4 b)
{
    __vector4 r;
    for(unsigned i=0;i<4u;++i){
        unsigned v=0; float x=a.vector4_f32[i], lim=b.vector4_f32[i];
        if(!(x<=lim)) v|=0x80000000u;
        if(!(x>=-lim)) v|=0x40000000u;
        r.vector4_u32[i]=v;
    }
    return r;
}

/* ---- VMX saturating integer add/sub --------------------------------------- */
static inline int _clampi(long long v,long long lo,long long hi){ return (int)(v<lo?lo:v>hi?hi:v); }
__vector4 __vaddshs(__vector4 a, __vector4 b)  /* signed 16-bit saturate */
{ __vector4 r; const short*pa=(const short*)&a,*pb=(const short*)&b; short*o=(short*)&r;
  for(unsigned i=0;i<8u;++i) o[i]=(short)_clampi((long long)pa[i]+pb[i],-32768,32767); return r; }
__vector4 __vsubshs(__vector4 a, __vector4 b)
{ __vector4 r; const short*pa=(const short*)&a,*pb=(const short*)&b; short*o=(short*)&r;
  for(unsigned i=0;i<8u;++i) o[i]=(short)_clampi((long long)pa[i]-pb[i],-32768,32767); return r; }
__vector4 __vaddsws(__vector4 a, __vector4 b)  /* signed 32-bit saturate */
{ __vector4 r; const int*pa=(const int*)&a,*pb=(const int*)&b; int*o=(int*)&r;
  for(unsigned i=0;i<4u;++i) o[i]=_clampi((long long)pa[i]+pb[i],-2147483648LL,2147483647LL); return r; }
__vector4 __vsubsws(__vector4 a, __vector4 b)
{ __vector4 r; const int*pa=(const int*)&a,*pb=(const int*)&b; int*o=(int*)&r;
  for(unsigned i=0;i<4u;++i) o[i]=_clampi((long long)pa[i]-pb[i],-2147483648LL,2147483647LL); return r; }
__vector4 __vaddubs(__vector4 a, __vector4 b)  /* unsigned 8-bit saturate */
{ __vector4 r; const unsigned char*pa=(const unsigned char*)&a,*pb=(const unsigned char*)&b; unsigned char*o=(unsigned char*)&r;
  for(unsigned i=0;i<16u;++i){ unsigned s=(unsigned)pa[i]+pb[i]; o[i]=(unsigned char)(s>255u?255u:s);} return r; }
__vector4 __vadduhs(__vector4 a, __vector4 b)  /* unsigned 16-bit saturate */
{ __vector4 r; const unsigned short*pa=(const unsigned short*)&a,*pb=(const unsigned short*)&b; unsigned short*o=(unsigned short*)&r;
  for(unsigned i=0;i<8u;++i){ unsigned s=(unsigned)pa[i]+pb[i]; o[i]=(unsigned short)(s>65535u?65535u:s);} return r; }

/* ---- VMX splat immediate (byte/halfword; word form is defined above) ------
 * SIM arrives already sign-extended to int (matching __vspltisw); splat its
 * low element-width bits across every element. */
__vector4 __vspltisb(int sim)
{ __vector4 r; unsigned char v=(unsigned char)sim; unsigned char*o=(unsigned char*)&r;
  for(unsigned i=0;i<16u;++i) o[i]=v; return r; }
__vector4 __vspltish(int sim)
{ __vector4 r; short v=(short)sim; short*o=(short*)&r;
  for(unsigned i=0;i<8u;++i) o[i]=v; return r; }

/* ---- scalar float select (fsel single) ----------------------------------- */
/* __fsel/__fself: comparand >= 0.0 (including -0.0) selects valGE, else valLT. */
float __fself(float c, float ge, float lt) { return c >= 0.0f ? ge : lt; }

/* ---- scalar float estimate/sqrt intrinsics (ppcintrinsics.h) --------------- */
/* MSVC treats these as compiler intrinsics that emit a single PPC instruction;
   clang has no such builtins, so a title using them (InlineAssembly's
   ArrayMathFunctionCPP calls __frsqrte) otherwise links to an undefined symbol.
   Provide them as the exact one-instruction leaf functions the XDK documents. */
double __fsqrt(double x)  { double r; __asm__("fsqrt %0,%1"   : "=f"(r) : "f"(x)); return r; }
float  __fsqrts(float x)  { float  r; __asm__("fsqrts %0,%1"  : "=f"(r) : "f"(x)); return r; }
double __frsqrte(double x){ double r; __asm__("frsqrte %0,%1" : "=f"(r) : "f"(x)); return r; }
float  __fres(double x)   { float  r; __asm__("fres %0,%1"    : "=f"(r) : "f"(x)); return r; }

/* ---- VMX select / splat-halfword / alignment loads / partial stores ------- */

/* Bitwise select: result bit = (VRC bit) ? VRB : VRA. */
__vector4 __vsel(__vector4 a, __vector4 b, __vector4 c)
{ __vector4 r; for(unsigned i=0;i<4u;++i) r.vector4_u32[i]=(a.vector4_u32[i]&~c.vector4_u32[i])|(b.vector4_u32[i]&c.vector4_u32[i]); return r; }

/* Splat halfword: every halfword = VRB halfword[uim & 7]. */
__vector4 __vsplth(__vector4 b, unsigned uim)
{ __vector4 r; const unsigned short* pb=(const unsigned short*)&b; unsigned short* o=(unsigned short*)&r;
  unsigned short v=pb[uim&7u]; for(unsigned i=0;i<8u;++i) o[i]=v; return r; }

/* Load Vector for Shift Left/Right: permute-control vectors for aligning an
   unaligned 16-byte access. sh = EA & 0xF. lvsl[i]=sh+i, lvsr[i]=16-sh+i. */
__vector4 __lvsl(const void* base, int offset)
{ __vector4 r; unsigned sh=(unsigned)(((uintptr_t)base+(uintptr_t)offset)&15u); unsigned char* o=(unsigned char*)&r;
  for(unsigned i=0;i<16u;++i) o[i]=(unsigned char)(sh+i); return r; }
__vector4 __lvsr(const void* base, int offset)
{ __vector4 r; unsigned sh=(unsigned)(((uintptr_t)base+(uintptr_t)offset)&15u); unsigned char* o=(unsigned char*)&r;
  for(unsigned i=0;i<16u;++i) o[i]=(unsigned char)(16u-sh+i); return r; }
__vector4 __lvsl_volatile(const volatile void* base, int offset){ return __lvsl((const void*)base,offset); }
__vector4 __lvsr_volatile(const volatile void* base, int offset){ return __lvsr((const void*)base,offset); }

/* Store Vector Left/Right Indexed: partial stores, the mirror of __lvlx/__lvrx.
   stvlx stores bytes [EA, next-16-boundary); stvrx stores [prev-boundary, EA).
   Emit the real instructions: each writes ONLY the bytes on its side of the EA,
   so together they store exactly the vector's 16 bytes with no read-modify-write
   of neighbouring bytes (RXDK-360 finding #18). EA = base + offset; RA (base) is
   a nonzero base register -> "b". */
void __stvlx(__vector4 vSrc, void* base, int offset)
{ union { __rxvf4_t v; __vector4 s; } u; u.s = vSrc;
  __asm__ volatile("stvlx %0, %1, %2" : : "v"(u.v), "b"(base), "r"(offset) : "memory"); }
void __stvrx(__vector4 vSrc, void* base, int offset)
{ union { __rxvf4_t v; __vector4 s; } u; u.s = vSrc;
  __asm__ volatile("stvrx %0, %1, %2" : : "v"(u.v), "b"(base), "r"(offset) : "memory"); }
void __stvlx_volatile(__vector4 v, volatile void* base, int offset){ __stvlx(v,(void*)base,offset); }
void __stvrx_volatile(__vector4 v, volatile void* base, int offset){ __stvrx(v,(void*)base,offset); }

/* ---- VMX pack/unpack to D3D vertex formats (__vpkd3d / __vupkd3d) ---------
 * DT selects the packed datatype, MS/SHW where the packed bits land in VRT.
 * The common formats (D3DCOLOR and FLOAT16_2/4) are handled precisely; other
 * NORM* formats fall back to a truncating 8-bit pack (linkable, rarely hit).
 * Enum values mirror __VECTOR_PACK_TYPES/__VECTOR_PACK_MASK in vectorintrinsics.h. */
enum { RXDK_VPACK_D3DCOLOR=0, RXDK_VPACK_NORMSHORT2=1, RXDK_VPACK_NORMPACKED32=2,
       RXDK_VPACK_FLOAT16_2=3, RXDK_VPACK_NORMSHORT4=4, RXDK_VPACK_FLOAT16_4=5,
       RXDK_VPACK_NORMPACKED64=6 };
enum { RXDK_VPACK_32=1, RXDK_VPACK_64LO=2, RXDK_VPACK_64HI=3 };

static unsigned short _f32_to_f16(float f)
{
    union { float f; unsigned u; } v; v.f=f;
    unsigned s=(v.u>>16)&0x8000u; int e=(int)((v.u>>23)&0xFF)-127+15; unsigned m=v.u&0x7FFFFFu;
    if(e<=0){ if(e<-10) return (unsigned short)s; m|=0x800000u; unsigned t=(unsigned)(14-e); unsigned a=(m+(1u<<(t-1))+(((m>>t)&1u)?0u:0u))>>t; return (unsigned short)(s|a); }
    if(e>=31) return (unsigned short)(s|0x7C00u);
    return (unsigned short)(s|((unsigned)e<<10)|((m+0x00001000u)>>13 & 0x3FFu));
}
static float _f16_to_f32(unsigned short h)
{
    unsigned s=(h&0x8000u)<<16; int e=(h>>10)&0x1F; unsigned m=h&0x3FFu; union{unsigned u;float f;}v;
    if(e==0){ if(m==0){ v.u=s; return v.f; } while(!(m&0x400u)){ m<<=1; --e; } ++e; m&=~0x400u; }
    else if(e==31){ v.u=s|0x7F800000u|(m<<13); return v.f; }
    v.u=s|((unsigned)(e+112)<<23)|(m<<13); return v.f;
}
static unsigned char _clampu8f(float f){ int v=(int)(f+0.5f); return (unsigned char)(v<0?0:v>255?255:v); }

__vector4 __vpkd3d(__vector4 t, __vector4 b, unsigned dt, unsigned ms, unsigned shw)
{
    unsigned long long packed=0; unsigned bits=32;
    if(dt==RXDK_VPACK_D3DCOLOR){
        packed=((unsigned long long)_clampu8f(b.vector4_f32[3])<<24)|((unsigned)_clampu8f(b.vector4_f32[0])<<16)
              |((unsigned)_clampu8f(b.vector4_f32[1])<<8)|_clampu8f(b.vector4_f32[2]); bits=32;   /* ARGB */
    } else if(dt==RXDK_VPACK_FLOAT16_2){
        packed=((unsigned)_f32_to_f16(b.vector4_f32[0])<<16)|_f32_to_f16(b.vector4_f32[1]); bits=32;
    } else if(dt==RXDK_VPACK_FLOAT16_4){
        packed=((unsigned long long)_f32_to_f16(b.vector4_f32[0])<<48)|((unsigned long long)_f32_to_f16(b.vector4_f32[1])<<32)
              |((unsigned long long)_f32_to_f16(b.vector4_f32[2])<<16)|_f32_to_f16(b.vector4_f32[3]); bits=64;
    } else { /* NORM* fallback: 8-bit truncating pack */
        packed=((unsigned long long)_clampu8f(b.vector4_f32[0])<<24)|((unsigned)_clampu8f(b.vector4_f32[1])<<16)
              |((unsigned)_clampu8f(b.vector4_f32[2])<<8)|_clampu8f(b.vector4_f32[3]); bits=32;
    }
    __vector4 r=t;
    if(ms==RXDK_VPACK_32 || bits==32){ r.vector4_u32[shw&3u]=(unsigned)packed; }
    else { unsigned w=(shw&3u); if(w>2u) w=2u; r.vector4_u32[w]=(unsigned)(packed>>32); r.vector4_u32[w+1u]=(unsigned)packed; }
    return r;
}
__vector4 __vupkd3d(__vector4 b, unsigned dt)
{
    __vector4 r; unsigned p=b.vector4_u32[0];
    if(dt==RXDK_VPACK_D3DCOLOR){
        r.vector4_f32[0]=(float)((p>>16)&0xFF); r.vector4_f32[1]=(float)((p>>8)&0xFF);
        r.vector4_f32[2]=(float)(p&0xFF);       r.vector4_f32[3]=(float)((p>>24)&0xFF);
    } else if(dt==RXDK_VPACK_FLOAT16_2){
        r.vector4_f32[0]=_f16_to_f32((unsigned short)(p>>16)); r.vector4_f32[1]=_f16_to_f32((unsigned short)p);
        r.vector4_f32[2]=0.0f; r.vector4_f32[3]=1.0f;
    } else if(dt==RXDK_VPACK_FLOAT16_4){
        unsigned p1=b.vector4_u32[1];
        r.vector4_f32[0]=_f16_to_f32((unsigned short)(p>>16)); r.vector4_f32[1]=_f16_to_f32((unsigned short)p);
        r.vector4_f32[2]=_f16_to_f32((unsigned short)(p1>>16)); r.vector4_f32[3]=_f16_to_f32((unsigned short)p1);
    } else {
        r.vector4_f32[0]=(float)((p>>24)&0xFF); r.vector4_f32[1]=(float)((p>>16)&0xFF);
        r.vector4_f32[2]=(float)((p>>8)&0xFF);  r.vector4_f32[3]=(float)(p&0xFF);
    }
    return r;
}

/* ======================================================================== *
 * Complete VMX integer families (byte/halfword/word, signed/unsigned).     *
 * Big-endian element order (element 0 = most significant). Scalar, spec-    *
 * faithful. Declared in vectorintrinsics.h.                                 *
 * ======================================================================== */

#define VB(v)  ((unsigned char*)&(v))
#define VBc(v) ((const unsigned char*)&(v))
#define VH(v)  ((unsigned short*)&(v))
#define VHc(v) ((const unsigned short*)&(v))
#define VSHc(v)((const short*)&(v))
#define VSWc(v)((const int*)&(v))
static int _clmp(long long v,long long lo,long long hi){return (int)(v<lo?lo:v>hi?hi:v);}

/* ---- logical ---- */
__vector4 __vandc(__vector4 a,__vector4 b){__vector4 r;for(unsigned i=0;i<4;++i)r.vector4_u32[i]=a.vector4_u32[i]&~b.vector4_u32[i];return r;}
__vector4 __vnor (__vector4 a,__vector4 b){__vector4 r;for(unsigned i=0;i<4;++i)r.vector4_u32[i]=~(a.vector4_u32[i]|b.vector4_u32[i]);return r;}

/* ---- add/sub modulo (wraparound) ---- */
__vector4 __vaddubm(__vector4 a,__vector4 b){__vector4 r;for(unsigned i=0;i<16;++i)VB(r)[i]=(unsigned char)(VBc(a)[i]+VBc(b)[i]);return r;}
__vector4 __vadduhm(__vector4 a,__vector4 b){__vector4 r;for(unsigned i=0;i<8;++i)VH(r)[i]=(unsigned short)(VHc(a)[i]+VHc(b)[i]);return r;}
__vector4 __vadduwm(__vector4 a,__vector4 b){__vector4 r;for(unsigned i=0;i<4;++i)r.vector4_u32[i]=a.vector4_u32[i]+b.vector4_u32[i];return r;}
__vector4 __vsububm(__vector4 a,__vector4 b){__vector4 r;for(unsigned i=0;i<16;++i)VB(r)[i]=(unsigned char)(VBc(a)[i]-VBc(b)[i]);return r;}
__vector4 __vsubuhm(__vector4 a,__vector4 b){__vector4 r;for(unsigned i=0;i<8;++i)VH(r)[i]=(unsigned short)(VHc(a)[i]-VHc(b)[i]);return r;}
__vector4 __vsubuwm(__vector4 a,__vector4 b){__vector4 r;for(unsigned i=0;i<4;++i)r.vector4_u32[i]=a.vector4_u32[i]-b.vector4_u32[i];return r;}

/* ---- add/sub saturate (the ones not already defined) ---- */
__vector4 __vaddsbs(__vector4 a,__vector4 b){__vector4 r;for(unsigned i=0;i<16;++i)((signed char*)VB(r))[i]=(signed char)_clmp((long long)((const signed char*)VBc(a))[i]+((const signed char*)VBc(b))[i],-128,127);return r;}
__vector4 __vadduws(__vector4 a,__vector4 b){__vector4 r;for(unsigned i=0;i<4;++i){unsigned long long s=(unsigned long long)a.vector4_u32[i]+b.vector4_u32[i];r.vector4_u32[i]=(unsigned)(s>0xFFFFFFFFull?0xFFFFFFFFull:s);}return r;}
__vector4 __vsubsbs(__vector4 a,__vector4 b){__vector4 r;for(unsigned i=0;i<16;++i)((signed char*)VB(r))[i]=(signed char)_clmp((long long)((const signed char*)VBc(a))[i]-((const signed char*)VBc(b))[i],-128,127);return r;}
__vector4 __vsububs(__vector4 a,__vector4 b){__vector4 r;for(unsigned i=0;i<16;++i){int d=(int)VBc(a)[i]-VBc(b)[i];VB(r)[i]=(unsigned char)(d<0?0:d);}return r;}
__vector4 __vsubuhs(__vector4 a,__vector4 b){__vector4 r;for(unsigned i=0;i<8;++i){int d=(int)VHc(a)[i]-VHc(b)[i];VH(r)[i]=(unsigned short)(d<0?0:d);}return r;}
__vector4 __vsubuws(__vector4 a,__vector4 b){__vector4 r;for(unsigned i=0;i<4;++i){long long d=(long long)a.vector4_u32[i]-b.vector4_u32[i];r.vector4_u32[i]=(unsigned)(d<0?0:d);}return r;}

/* ---- max/min ---- */
#define MINMAXB(nm,ty,cmp) __vector4 nm(__vector4 a,__vector4 b){__vector4 r;for(unsigned i=0;i<16;++i){ty x=((const ty*)VBc(a))[i],y=((const ty*)VBc(b))[i];((ty*)VB(r))[i]=cmp;}return r;}
#define MINMAXH(nm,ty,cmp) __vector4 nm(__vector4 a,__vector4 b){__vector4 r;for(unsigned i=0;i<8;++i){ty x=((const ty*)VHc(a))[i],y=((const ty*)VHc(b))[i];((ty*)VH(r))[i]=cmp;}return r;}
#define MINMAXW(nm,ty,cmp) __vector4 nm(__vector4 a,__vector4 b){__vector4 r;for(unsigned i=0;i<4;++i){ty x=((const ty*)&a)[i],y=((const ty*)&b)[i];((ty*)&r)[i]=cmp;}return r;}
MINMAXB(__vmaxub,unsigned char,x>y?x:y) MINMAXB(__vmaxsb,signed char,x>y?x:y) MINMAXB(__vminub,unsigned char,x<y?x:y) MINMAXB(__vminsb,signed char,x<y?x:y)
MINMAXH(__vmaxuh,unsigned short,x>y?x:y) MINMAXH(__vmaxsh,short,x>y?x:y) MINMAXH(__vminuh,unsigned short,x<y?x:y) MINMAXH(__vminsh,short,x<y?x:y)
MINMAXW(__vmaxuw,unsigned int,x>y?x:y) MINMAXW(__vmaxsw,int,x>y?x:y) MINMAXW(__vminuw,unsigned int,x<y?x:y) MINMAXW(__vminsw,int,x<y?x:y)

/* ---- compare (all-ones where true) ---- */
#define CMPB(nm,ty,op) __vector4 nm(__vector4 a,__vector4 b){__vector4 r;for(unsigned i=0;i<16;++i)VB(r)[i]=(((const ty*)VBc(a))[i] op ((const ty*)VBc(b))[i])?0xFFu:0;return r;}
#define CMPH(nm,ty,op) __vector4 nm(__vector4 a,__vector4 b){__vector4 r;for(unsigned i=0;i<8;++i)VH(r)[i]=(((const ty*)VHc(a))[i] op ((const ty*)VHc(b))[i])?0xFFFFu:0;return r;}
#define CMPW(nm,ty,op) __vector4 nm(__vector4 a,__vector4 b){__vector4 r;for(unsigned i=0;i<4;++i)r.vector4_u32[i]=(((const ty*)&a)[i] op ((const ty*)&b)[i])?0xFFFFFFFFu:0;return r;}
CMPB(__vcmpequb,unsigned char,==) CMPH(__vcmpequh,unsigned short,==) CMPW(__vcmpequw,unsigned int,==)
CMPH(__vcmpgtsh,short,>) CMPH(__vcmpgtuh,unsigned short,>) CMPW(__vcmpgtsw,int,>) CMPW(__vcmpgtuw,unsigned int,>)

/* ---- merge (halfword/word; byte forms already defined) ---- */
__vector4 __vmrghh(__vector4 a,__vector4 b){__vector4 r;for(unsigned i=0;i<4;++i){VH(r)[2*i]=VHc(a)[i];VH(r)[2*i+1]=VHc(b)[i];}return r;}
__vector4 __vmrglh(__vector4 a,__vector4 b){__vector4 r;for(unsigned i=0;i<4;++i){VH(r)[2*i]=VHc(a)[4+i];VH(r)[2*i+1]=VHc(b)[4+i];}return r;}
__vector4 __vmrghw(__vector4 a,__vector4 b){__vector4 r;for(unsigned i=0;i<2;++i){r.vector4_u32[2*i]=a.vector4_u32[i];r.vector4_u32[2*i+1]=b.vector4_u32[i];}return r;}
__vector4 __vmrglw(__vector4 a,__vector4 b){__vector4 r;for(unsigned i=0;i<2;++i){r.vector4_u32[2*i]=a.vector4_u32[2+i];r.vector4_u32[2*i+1]=b.vector4_u32[2+i];}return r;}

/* ---- pack (a -> high half of result, b -> low half) ---- */
__vector4 __vpkuhum(__vector4 a,__vector4 b){__vector4 r;for(unsigned i=0;i<8;++i)VB(r)[i]=(unsigned char)VHc(a)[i];for(unsigned i=0;i<8;++i)VB(r)[8+i]=(unsigned char)VHc(b)[i];return r;}
__vector4 __vpkuwum(__vector4 a,__vector4 b){__vector4 r;for(unsigned i=0;i<4;++i)VH(r)[i]=(unsigned short)a.vector4_u32[i];for(unsigned i=0;i<4;++i)VH(r)[4+i]=(unsigned short)b.vector4_u32[i];return r;}
__vector4 __vpkuhus(__vector4 a,__vector4 b){__vector4 r;for(unsigned i=0;i<8;++i)VB(r)[i]=(unsigned char)(VHc(a)[i]>255?255:VHc(a)[i]);for(unsigned i=0;i<8;++i)VB(r)[8+i]=(unsigned char)(VHc(b)[i]>255?255:VHc(b)[i]);return r;}
__vector4 __vpkuwus(__vector4 a,__vector4 b){__vector4 r;for(unsigned i=0;i<4;++i)VH(r)[i]=(unsigned short)(a.vector4_u32[i]>65535?65535:a.vector4_u32[i]);for(unsigned i=0;i<4;++i)VH(r)[4+i]=(unsigned short)(b.vector4_u32[i]>65535?65535:b.vector4_u32[i]);return r;}
__vector4 __vpkshus(__vector4 a,__vector4 b){__vector4 r;for(unsigned i=0;i<8;++i)VB(r)[i]=(unsigned char)_clmp(VSHc(a)[i],0,255);for(unsigned i=0;i<8;++i)VB(r)[8+i]=(unsigned char)_clmp(VSHc(b)[i],0,255);return r;}
__vector4 __vpkshss(__vector4 a,__vector4 b){__vector4 r;for(unsigned i=0;i<8;++i)((signed char*)VB(r))[i]=(signed char)_clmp(VSHc(a)[i],-128,127);for(unsigned i=0;i<8;++i)((signed char*)VB(r))[8+i]=(signed char)_clmp(VSHc(b)[i],-128,127);return r;}
__vector4 __vpkswus(__vector4 a,__vector4 b){__vector4 r;for(unsigned i=0;i<4;++i)VH(r)[i]=(unsigned short)_clmp(VSWc(a)[i],0,65535);for(unsigned i=0;i<4;++i)VH(r)[4+i]=(unsigned short)_clmp(VSWc(b)[i],0,65535);return r;}
__vector4 __vpkswss(__vector4 a,__vector4 b){__vector4 r;for(unsigned i=0;i<4;++i)((short*)VH(r))[i]=(short)_clmp(VSWc(a)[i],-32768,32767);for(unsigned i=0;i<4;++i)((short*)VH(r))[4+i]=(short)_clmp(VSWc(b)[i],-32768,32767);return r;}

/* ---- unpack signed (high = elements 0..n/2, low = the rest) ---- */
__vector4 __vupkhsb(__vector4 b){__vector4 r;for(unsigned i=0;i<8;++i)((short*)VH(r))[i]=(short)((const signed char*)VBc(b))[i];return r;}
__vector4 __vupklsb(__vector4 b){__vector4 r;for(unsigned i=0;i<8;++i)((short*)VH(r))[i]=(short)((const signed char*)VBc(b))[8+i];return r;}
__vector4 __vupkhsh(__vector4 b){__vector4 r;for(unsigned i=0;i<4;++i)((int*)&r)[i]=(int)VSHc(b)[i];return r;}
__vector4 __vupklsh(__vector4 b){__vector4 r;for(unsigned i=0;i<4;++i)((int*)&r)[i]=(int)VSHc(b)[4+i];return r;}

/* ---- per-element shifts (count = low bits of the matching element in VRB) ---- */
__vector4 __vslb(__vector4 a,__vector4 b){__vector4 r;for(unsigned i=0;i<16;++i)VB(r)[i]=(unsigned char)(VBc(a)[i]<<(VBc(b)[i]&7));return r;}
__vector4 __vslh(__vector4 a,__vector4 b){__vector4 r;for(unsigned i=0;i<8;++i)VH(r)[i]=(unsigned short)(VHc(a)[i]<<(VHc(b)[i]&15));return r;}
__vector4 __vslw(__vector4 a,__vector4 b){__vector4 r;for(unsigned i=0;i<4;++i)r.vector4_u32[i]=a.vector4_u32[i]<<(b.vector4_u32[i]&31);return r;}
__vector4 __vsrb(__vector4 a,__vector4 b){__vector4 r;for(unsigned i=0;i<16;++i)VB(r)[i]=(unsigned char)(VBc(a)[i]>>(VBc(b)[i]&7));return r;}
__vector4 __vsrh(__vector4 a,__vector4 b){__vector4 r;for(unsigned i=0;i<8;++i)VH(r)[i]=(unsigned short)(VHc(a)[i]>>(VHc(b)[i]&15));return r;}
__vector4 __vsrw(__vector4 a,__vector4 b){__vector4 r;for(unsigned i=0;i<4;++i)r.vector4_u32[i]=a.vector4_u32[i]>>(b.vector4_u32[i]&31);return r;}
__vector4 __vsrab(__vector4 a,__vector4 b){__vector4 r;for(unsigned i=0;i<16;++i)((signed char*)VB(r))[i]=(signed char)(((const signed char*)VBc(a))[i]>>(VBc(b)[i]&7));return r;}
__vector4 __vsrah(__vector4 a,__vector4 b){__vector4 r;for(unsigned i=0;i<8;++i)((short*)VH(r))[i]=(short)(VSHc(a)[i]>>(VHc(b)[i]&15));return r;}
__vector4 __vsraw(__vector4 a,__vector4 b){__vector4 r;for(unsigned i=0;i<4;++i)((int*)&r)[i]=VSWc(a)[i]>>(b.vector4_u32[i]&31);return r;}

/* ---- rotate left per element ---- */
__vector4 __vrlb(__vector4 a,__vector4 b){__vector4 r;for(unsigned i=0;i<16;++i){unsigned s=VBc(b)[i]&7,v=VBc(a)[i];VB(r)[i]=(unsigned char)((v<<s)|(v>>((8-s)&7)));}return r;}
__vector4 __vrlh(__vector4 a,__vector4 b){__vector4 r;for(unsigned i=0;i<8;++i){unsigned s=VHc(b)[i]&15,v=VHc(a)[i];VH(r)[i]=(unsigned short)((v<<s)|(v>>((16-s)&15)));}return r;}
__vector4 __vrlw(__vector4 a,__vector4 b){__vector4 r;for(unsigned i=0;i<4;++i){unsigned s=b.vector4_u32[i]&31,v=a.vector4_u32[i];r.vector4_u32[i]=s?((v<<s)|(v>>(32-s))):v;}return r;}

/* ---- whole-vector bit shift (count = low 3 bits of the last byte of VRB) ---- */
__vector4 __vsl(__vector4 a,__vector4 b){__vector4 r;unsigned s=VBc(b)[15]&7;const unsigned char*p=VBc(a);unsigned char*o=VB(r);for(unsigned i=0;i<16;++i){unsigned hi=p[i]<<s;unsigned lo=(i+1<16)?(p[i+1]>>(8-s)):0;o[i]=(unsigned char)(s?(hi|lo):p[i]);}return r;}
__vector4 __vsr(__vector4 a,__vector4 b){__vector4 r;unsigned s=VBc(b)[15]&7;const unsigned char*p=VBc(a);unsigned char*o=VB(r);for(int i=15;i>=0;--i){unsigned lo=p[i]>>s;unsigned hi=(i>0)?(p[i-1]<<(8-s)):0;o[i]=(unsigned char)(s?(lo|hi):p[i]);}return r;}

/* ---- splat byte ---- */
__vector4 __vspltb(__vector4 b,unsigned uim){__vector4 r;unsigned char v=VBc(b)[uim&15];for(unsigned i=0;i<16;++i)VB(r)[i]=v;return r;}

/* ---- convert unsigned <-> float with a power-of-two scale ---- */
__vector4 __vcfux(__vector4 b,unsigned shift){__vector4 r;float d=(float)(1u<<(shift&31));for(unsigned i=0;i<4;++i)r.vector4_f32[i]=(float)b.vector4_u32[i]/d;return r;}
__vector4 __vctuxs(__vector4 b,unsigned shift){__vector4 r;float m=(float)(1u<<(shift&31));for(unsigned i=0;i<4;++i){double v=(double)b.vector4_f32[i]*m;r.vector4_u32[i]=(unsigned)(v<0?0:v>4294967295.0?4294967295.0:v);}return r;}

/* ---- store vector element byte/halfword (word form already defined) ---- */
void __stvebx(__vector4 v,void* base,int off){uintptr_t ea=(uintptr_t)base+(uintptr_t)off;*(unsigned char*)ea=VBc(v)[ea&15];}
void __stvehx(__vector4 v,void* base,int off){uintptr_t ea=(uintptr_t)base+(uintptr_t)off;*(unsigned short*)(ea&~(uintptr_t)1)=VHc(v)[(ea>>1)&7];}

/* ---- VMX float round-to-integer family (result stays float) --------------- */
#include <math.h>
__vector4 __vrfin(__vector4 a){ __vector4 r; for(unsigned i=0;i<4;++i) r.vector4_f32[i]=rintf(a.vector4_f32[i]); return r; }   /* nearest */
__vector4 __vrfim(__vector4 a){ __vector4 r; for(unsigned i=0;i<4;++i) r.vector4_f32[i]=floorf(a.vector4_f32[i]); return r; }  /* minus  */
__vector4 __vrfip(__vector4 a){ __vector4 r; for(unsigned i=0;i<4;++i) r.vector4_f32[i]=ceilf(a.vector4_f32[i]); return r; }   /* plus   */
__vector4 __vrfiz(__vector4 a){ __vector4 r; for(unsigned i=0;i<4;++i) r.vector4_f32[i]=truncf(a.vector4_f32[i]); return r; }  /* zero   */
