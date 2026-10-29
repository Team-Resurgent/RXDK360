/**************************************************************************
 * clang_vmx_intrinsics.h  --  RXDK-360
 *
 * Native-clang implementation of the Xbox 360 VMX128 intrinsic layer that the
 * stock <VectorIntrinsics.h> declares for the Microsoft compiler. Included by
 * the patched VectorIntrinsics.h under `#if defined(__clang__)` in place of the
 * `__declspec(intrin_type) __vector4` + extern prototypes.
 *
 * Why: MSVC's __declspec(intrin_type) makes __vector4 a register-resident
 * vector while still exposing named members; clang has no equivalent. Here
 * __vector4 is a union over a native clang vector (.vf/.vu) plus the member
 * arrays. Every intrinsic is force-inline over clang's AltiVec (altivec.h)
 * vec_* API / native operators, so after xnamath's own force-inlining the whole
 * thing stays in vector registers (SROA), matching the real XDK codegen -- and
 * the load path (lvx/lvsl/vperm, lvlx/lvrx) tolerates misaligned source, which
 * the _XM_NO_INTRINSICS_ scalar path did not.
 **************************************************************************/
#ifndef __RXDK_CLANG_VMX_INTRINSICS_H__
#define __RXDK_CLANG_VMX_INTRINSICS_H__

/* Suppress the stock <VectorIntrinsics.h> body: this header is the complete
   replacement for clang (type + every intrinsic). Force-included before xnamath
   so the stock file's `#ifndef __VECTORINTRINSICS_H__` guard skips it. */
#define __VECTORINTRINSICS_H__

#include <altivec.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Packed vertex formats for __vpkd3d/__vupkd3d (from the stock header). */
typedef enum __VECTOR_PACK_TYPES {
    VPACK_D3DCOLOR     = 0x00, VPACK_NORMSHORT2   = 0x01, VPACK_NORMPACKED32 = 0x02,
    VPACK_FLOAT16_2    = 0x03, VPACK_NORMSHORT4   = 0x04, VPACK_FLOAT16_4    = 0x05,
    VPACK_NORMPACKED64 = 0x06,
} __VECTOR_PACK_TYPES;
typedef enum __VECTOR_PACK_MASK {
    VPACK_32 = 1, VPACK_64LO = 2, VPACK_64HI = 3,
} __VECTOR_PACK_MASK;

typedef __attribute__((__vector_size__(16), __aligned__(16))) float          __vf4_t;
typedef __attribute__((__vector_size__(16), __aligned__(16))) unsigned int   __vu4_t;
typedef __attribute__((__vector_size__(16), __aligned__(16))) int            __vi4_t;
typedef __attribute__((__vector_size__(16), __aligned__(16))) unsigned char  __vb16_t;

#ifndef _DECLSPEC_ALIGN_16_
#ifndef _NOALIGNMENT_
    #define _DECLSPEC_ALIGN_16_   __attribute__((__aligned__(16)))
#else
    #define _DECLSPEC_ALIGN_16_
#endif
#endif

typedef union _DECLSPEC_ALIGN_16_ __vector4 {
    __vf4_t      vf;
    __vu4_t      vu;
    float        vector4_f32[4];
    unsigned int vector4_u32[4];
#ifndef _VECTOR4_STRICT_MEMBER_ACCESS_
    struct { float x, y, z, w; };
    float        v[4];
    unsigned int u[4];
#endif
} __vector4;

/* __vector4i is defined by xnamath.h itself (typedef UINT __vector4i[4]); do not
   define it here or the typedefs conflict. */

#define _RXVMX static __inline__ __attribute__((__always_inline__, __nodebug__))
#define _RXV(...)  (__extension__({ __vector4 _r; _r.vf = (__vf4_t)(__VA_ARGS__); _r; }))

/* ---- loads (EA truncated/handled exactly as the real instruction) --------- */
/* __lvx: load 16 bytes from (base+offset)&~0xF */
_RXVMX __vector4 __lvx(const void* base, int offset) {
    return _RXV(vec_ld(offset, (const float*)base));
}
_RXVMX __vector4 __lvx_volatile(const volatile void* base, int offset) {
    return _RXV(vec_ld(offset, (const float*)base));
}
/* left/right indexed unaligned loads */
_RXVMX __vector4 __lvlx(const void* base, int offset) {
    return _RXV(vec_lvlx(offset, (const float*)base));
}
_RXVMX __vector4 __lvrx(const void* base, int offset) {
    return _RXV(vec_lvrx(offset, (const float*)base));
}
/* shift control vectors for software alignment */
_RXVMX __vector4 __lvsl(const void* base, int offset) {
    return _RXV((__vf4_t)vec_lvsl(offset, (const unsigned char*)base));
}
_RXVMX __vector4 __lvsr(const void* base, int offset) {
    return _RXV((__vf4_t)vec_lvsr(offset, (const unsigned char*)base));
}

/* ---- stores -------------------------------------------------------------- */
_RXVMX void __stvx(__vector4 s, void* base, int offset) {
    vec_st(s.vf, offset, (float*)base);
}
_RXVMX void __stvx_volatile(__vector4 s, volatile void* base, int offset) {
    vec_st(s.vf, offset, (float*)base);
}
_RXVMX void __stvlx(__vector4 s, void* base, int offset) {
    vec_stvlx(s.vf, offset, (vector float*)base);
}
_RXVMX void __stvrx(__vector4 s, void* base, int offset) {
    vec_stvrx(s.vf, offset, (vector float*)base);
}
_RXVMX void __stvewx(__vector4 s, void* base, int offset) {
    vec_ste(s.vu, offset, (unsigned int*)base);
}
_RXVMX void __stvehx(__vector4 s, void* base, int offset) {
    vec_ste((vector unsigned short)s.vu, offset, (unsigned short*)base);
}

/* ---- float arithmetic ---------------------------------------------------- */
_RXVMX __vector4 __vaddfp (__vector4 a, __vector4 b){ return _RXV(a.vf + b.vf); }
_RXVMX __vector4 __vsubfp (__vector4 a, __vector4 b){ return _RXV(a.vf - b.vf); }
_RXVMX __vector4 __vmulfp (__vector4 a, __vector4 b){ return _RXV(a.vf * b.vf); }
_RXVMX __vector4 __vmaddfp(__vector4 a, __vector4 b, __vector4 c){ return _RXV(__builtin_altivec_vmaddfp(a.vf,b.vf,c.vf)); }
/* __vmaddcfp: xnamath.h #defines it as a macro -> __vmaddfp; do not define here. */
_RXVMX __vector4 __vnmsubfp(__vector4 a, __vector4 b, __vector4 c){ return _RXV(__builtin_altivec_vnmsubfp(a.vf,b.vf,c.vf)); }
_RXVMX __vector4 __vmaxfp (__vector4 a, __vector4 b){ return _RXV(__builtin_altivec_vmaxfp(a.vf,b.vf)); }
_RXVMX __vector4 __vminfp (__vector4 a, __vector4 b){ return _RXV(__builtin_altivec_vminfp(a.vf,b.vf)); }
_RXVMX __vector4 __vrefp  (__vector4 a){ return _RXV(__builtin_altivec_vrefp(a.vf)); }
_RXVMX __vector4 __vrsqrtefp(__vector4 a){ return _RXV(__builtin_altivec_vrsqrtefp(a.vf)); }
_RXVMX __vector4 __vexptefp(__vector4 a){ return _RXV(__builtin_altivec_vexptefp(a.vf)); }
_RXVMX __vector4 __vlogefp (__vector4 a){ return _RXV(__builtin_altivec_vlogefp(a.vf)); }

/* ---- rounding ------------------------------------------------------------ */
_RXVMX __vector4 __vrfim(__vector4 a){ return _RXV(vec_floor(a.vf)); }
_RXVMX __vector4 __vrfip(__vector4 a){ return _RXV(vec_ceil(a.vf)); }
_RXVMX __vector4 __vrfiz(__vector4 a){ return _RXV(vec_trunc(a.vf)); }
_RXVMX __vector4 __vrfin(__vector4 a){ return _RXV(vec_round(a.vf)); }

/* ---- logical ------------------------------------------------------------- */
_RXVMX __vector4 __vand (__vector4 a, __vector4 b){ return _RXV((__vf4_t)(a.vu & b.vu)); }
_RXVMX __vector4 __vandc(__vector4 a, __vector4 b){ return _RXV((__vf4_t)vec_andc(a.vu,b.vu)); }
_RXVMX __vector4 __vor  (__vector4 a, __vector4 b){ return _RXV((__vf4_t)(a.vu | b.vu)); }
_RXVMX __vector4 __vnor (__vector4 a, __vector4 b){ return _RXV((__vf4_t)vec_nor(a.vu,b.vu)); }
_RXVMX __vector4 __vxor (__vector4 a, __vector4 b){ return _RXV((__vf4_t)(a.vu ^ b.vu)); }

/* ---- compare (result is a mask) ------------------------------------------ */
_RXVMX __vector4 __vcmpeqfp(__vector4 a, __vector4 b){ return _RXV((__vf4_t)__builtin_altivec_vcmpeqfp(a.vf,b.vf)); }
_RXVMX __vector4 __vcmpgefp(__vector4 a, __vector4 b){ return _RXV((__vf4_t)__builtin_altivec_vcmpgefp(a.vf,b.vf)); }
_RXVMX __vector4 __vcmpgtfp(__vector4 a, __vector4 b){ return _RXV((__vf4_t)__builtin_altivec_vcmpgtfp(a.vf,b.vf)); }
_RXVMX __vector4 __vcmpbfp (__vector4 a, __vector4 b){ return _RXV((__vf4_t)__builtin_altivec_vcmpbfp(a.vf,b.vf)); }
_RXVMX __vector4 __vcmpequw(__vector4 a, __vector4 b){ return _RXV((__vf4_t)(__vu4_t)vec_cmpeq(a.vu,b.vu)); }

/* ---- convert int<->float with scale -------------------------------------- */
/* immediate scale must be a compile-time constant -> macros */
#define __vcfsx(a,uimm) _RXV(__builtin_altivec_vcfsx((__vi4_t)(a).vu,(uimm)))
#define __vcfux(a,uimm) _RXV(__builtin_altivec_vcfux((a).vu,(uimm)))
#define __vctsxs(a,uimm) (__extension__({ __vector4 _r; _r.vu=(__vu4_t)__builtin_altivec_vctsxs((a).vf,(uimm)); _r; }))
#define __vctuxs(a,uimm) (__extension__({ __vector4 _r; _r.vu=(__vu4_t)__builtin_altivec_vctuxs((a).vf,(uimm)); _r; }))

/* ---- merge --------------------------------------------------------------- */
_RXVMX __vector4 __vmrghb(__vector4 a, __vector4 b){ return _RXV((__vf4_t)(__vu4_t)vec_mergeh((vector unsigned char)a.vu,(vector unsigned char)b.vu)); }
_RXVMX __vector4 __vmrghh(__vector4 a, __vector4 b){ return _RXV((__vf4_t)(__vu4_t)vec_mergeh((vector unsigned short)a.vu,(vector unsigned short)b.vu)); }
_RXVMX __vector4 __vmrghw(__vector4 a, __vector4 b){ return _RXV((__vf4_t)vec_mergeh(a.vu,b.vu)); }
_RXVMX __vector4 __vmrglw(__vector4 a, __vector4 b){ return _RXV((__vf4_t)vec_mergel(a.vu,b.vu)); }

/* ---- pack / unpack ------------------------------------------------------- */
_RXVMX __vector4 __vpkshss(__vector4 a, __vector4 b){ return _RXV((__vf4_t)(__vu4_t)vec_packs((vector short)a.vu,(vector short)b.vu)); }
_RXVMX __vector4 __vpkswss(__vector4 a, __vector4 b){ return _RXV((__vf4_t)(__vu4_t)vec_packs((vector int)a.vu,(vector int)b.vu)); }
_RXVMX __vector4 __vpkuhus(__vector4 a, __vector4 b){ return _RXV((__vf4_t)(__vu4_t)vec_packsu((vector unsigned short)a.vu,(vector unsigned short)b.vu)); }
_RXVMX __vector4 __vpkuwus(__vector4 a, __vector4 b){ return _RXV((__vf4_t)(__vu4_t)vec_packsu(a.vu,b.vu)); }
_RXVMX __vector4 __vupkhsb(__vector4 a){ return _RXV((__vf4_t)(__vu4_t)vec_unpackh((vector signed char)a.vu)); }
_RXVMX __vector4 __vupkhsh(__vector4 a){ return _RXV((__vf4_t)(__vu4_t)vec_unpackh((vector short)a.vu)); }

/* ---- shift / splat ------------------------------------------------------- */
_RXVMX __vector4 __vslw(__vector4 a, __vector4 b){ return _RXV((__vf4_t)vec_sl(a.vu,b.vu)); }
_RXVMX __vector4 __vsrw(__vector4 a, __vector4 b){ return _RXV((__vf4_t)vec_sr(a.vu,b.vu)); }
_RXVMX __vector4 __vsraw(__vector4 a, __vector4 b){ return _RXV((__vf4_t)(__vu4_t)vec_sra((__vi4_t)a.vu,b.vu)); }
#define __vspltw(a,uimm)   _RXV(__builtin_shufflevector((a).vf,(a).vf,(uimm),(uimm),(uimm),(uimm)))
#define __vsplth(a,uimm)   (__extension__({ __vector4 _r; vector unsigned short _s=(vector unsigned short)(a).vu; _r.vu=(__vu4_t)__builtin_shufflevector(_s,_s,(uimm),(uimm),(uimm),(uimm),(uimm),(uimm),(uimm),(uimm)); _r; }))
#define __vspltisw(simm)   (__extension__({ __vector4 _r; _r.vu=(__vu4_t)vec_splat_s32(simm); _r; }))
#define __vsldoi(a,b,uimm) _RXV((__vf4_t)vec_sld((a).vf,(b).vf,(uimm)))

/* ---- select / permute ---------------------------------------------------- */
_RXVMX __vector4 __vsel(__vector4 a, __vector4 b, __vector4 c){ return _RXV((__vf4_t)vec_sel(a.vu,b.vu,c.vu)); }
_RXVMX __vector4 __vperm(__vector4 a, __vector4 b, __vector4 c){ return _RXV((__vf4_t)vec_perm(a.vf,b.vf,(vector unsigned char)c.vu)); }

/* __vpermwi: permute words by an 8-bit immediate (two bits per result word,
   selecting source word 0..3). Immediate -> four shufflevector indices. */
#define __vpermwi(a,uimm) _RXV(__builtin_shufflevector((a).vf,(a).vf, \
    (((uimm)>>6)&3), (((uimm)>>4)&3), (((uimm)>>2)&3), ((uimm)&3)))

/* __vrlimi(vd, vb, mask4, shift): rotate vb left by `shift` words, then insert
   into vd the words whose bit is set in mask4 (bit3=word0 .. bit0=word3). */
#define __vrlimi(vd,vb,mask,shift) (__extension__({ \
    __vf4_t _vb=(vb).vf; \
    __vf4_t _rot=__builtin_shufflevector(_vb,_vb,(shift)&3,((shift)+1)&3,((shift)+2)&3,((shift)+3)&3); \
    const __vu4_t _m={ ((mask)&8)?0xffffffffu:0u, ((mask)&4)?0xffffffffu:0u, ((mask)&2)?0xffffffffu:0u, ((mask)&1)?0xffffffffu:0u }; \
    __vector4 _r; _r.vf=(__vf4_t)vec_sel((vd).vu,(__vu4_t)_rot,_m); _r; }))

/* ---- dot products (VMX128 vmsum) ----------------------------------------- */
_RXVMX __vector4 __vmsum4fp(__vector4 a, __vector4 b){
    __vf4_t p = a.vf * b.vf;
    __vf4_t s1 = p + __builtin_shufflevector(p,p,2,3,0,1);
    __vf4_t s2 = s1 + __builtin_shufflevector(s1,s1,1,0,3,2);
    return _RXV(s2);                       /* dot4 broadcast to all lanes */
}
_RXVMX __vector4 __vmsum3fp(__vector4 a, __vector4 b){
    __vf4_t p = a.vf * b.vf;               /* x y z w */
    /* Force the w product to +0 by MASKING (not *0: NaN*0=NaN leaves a NaN in w);
       bitwise-AND clears every bit of lane w, a NaN's included. */
    const __vu4_t _wmask={0xffffffffu,0xffffffffu,0xffffffffu,0u};
    p = (__vf4_t)((__vu4_t)p & _wmask);
    __vf4_t s1 = p + __builtin_shufflevector(p,p,2,3,0,1);
    __vf4_t s2 = s1 + __builtin_shufflevector(s1,s1,1,0,3,2);
    return _RXV(s2);                       /* dot3 broadcast to all lanes */
}

/* ---- D3D pack/unpack (VMX128 vpkd3d/vupkd3d) -----------------------------
   Pack types: D3DCOLOR=0 NORMSHORT2=1 NORMPACKED32=2 FLOAT16_2=3 NORMSHORT4=4
   FLOAT16_4=5 NORMPACKED64=6. `type` is always a compile-time constant at the
   call sites, so the switch folds. SHORT2/FLOAT16_2 default z=0,w=1 -- which is
   exactly what XMMatrixIdentity relies on (it unpacks NORMSHORT2 then permutes
   the 0.0/1.0 lanes into the identity rows). */
_RXVMX float _rx_f16_to_f32(unsigned short h){
    unsigned s=(unsigned)(h&0x8000u)<<16; int e=(h>>10)&0x1F; unsigned m=h&0x3FFu;
    union{unsigned u; float f;} v;
    if(e==0){ if(m==0){ v.u=s; return v.f; } while(!(m&0x400u)){ m<<=1; --e; } ++e; m&=~0x400u; }
    else if(e==31){ v.u=s|0x7F800000u|(m<<13); return v.f; }
    v.u=s|((unsigned)(e+112)<<23)|(m<<13); return v.f;
}
_RXVMX unsigned short _rx_f32_to_f16(float f){
    union{float f; unsigned u;} v; v.f=f; unsigned s=(v.u>>16)&0x8000u; int e=(int)((v.u>>23)&0xFF)-112; unsigned m=v.u&0x7FFFFFu;
    if(e<=0){ if(e<-10) return (unsigned short)s; m|=0x800000u; unsigned t=(unsigned)(14-e); return (unsigned short)(s|((m+(1u<<(t-1)))>>t)); }
    if(e>=31) return (unsigned short)(s|0x7C00u);
    return (unsigned short)(s|((unsigned)e<<10)|((m+0x00001000u)>>13 & 0x3FFu));
}
_RXVMX unsigned char _rx_clampu8f(float f){ int v=(int)(f+0.5f); return (unsigned char)(v<0?0:v>255?255:v); }
_RXVMX __vector4 __vupkd3d(__vector4 b, unsigned type){
    __vector4 r; unsigned p=b.vector4_u32[0], p1=b.vector4_u32[1];
    switch(type){
    case 0: /* D3DCOLOR (ARGB bytes -> R,G,B,A) */
        r.vector4_f32[0]=(float)((p>>16)&0xFF); r.vector4_f32[1]=(float)((p>>8)&0xFF);
        r.vector4_f32[2]=(float)(p&0xFF);        r.vector4_f32[3]=(float)((p>>24)&0xFF); break;
    case 1: /* NORMSHORT2 -> (s0/32767, s1/32767, 0, 1) */
        r.vector4_f32[0]=(float)((short)(p>>16))*(1.0f/32767.0f);
        r.vector4_f32[1]=(float)((short)(p&0xFFFF))*(1.0f/32767.0f);
        r.vector4_f32[2]=0.0f; r.vector4_f32[3]=1.0f; break;
    case 4: /* NORMSHORT4 -> four normalized shorts */
        r.vector4_f32[0]=(float)((short)(p>>16))*(1.0f/32767.0f);
        r.vector4_f32[1]=(float)((short)(p&0xFFFF))*(1.0f/32767.0f);
        r.vector4_f32[2]=(float)((short)(p1>>16))*(1.0f/32767.0f);
        r.vector4_f32[3]=(float)((short)(p1&0xFFFF))*(1.0f/32767.0f); break;
    case 3: /* FLOAT16_2 -> (h0, h1, 0, 1) */
        r.vector4_f32[0]=_rx_f16_to_f32((unsigned short)(p>>16)); r.vector4_f32[1]=_rx_f16_to_f32((unsigned short)p);
        r.vector4_f32[2]=0.0f; r.vector4_f32[3]=1.0f; break;
    case 5: /* FLOAT16_4 */
        r.vector4_f32[0]=_rx_f16_to_f32((unsigned short)(p>>16)); r.vector4_f32[1]=_rx_f16_to_f32((unsigned short)p);
        r.vector4_f32[2]=_rx_f16_to_f32((unsigned short)(p1>>16)); r.vector4_f32[3]=_rx_f16_to_f32((unsigned short)p1); break;
    default: /* NORMPACKED* fallback: bytes */
        r.vector4_f32[0]=(float)((p>>24)&0xFF); r.vector4_f32[1]=(float)((p>>16)&0xFF);
        r.vector4_f32[2]=(float)((p>>8)&0xFF);  r.vector4_f32[3]=(float)(p&0xFF); break;
    }
    return r;
}
_RXVMX __vector4 __vpkd3d(__vector4 t, __vector4 b, unsigned type, unsigned mask, unsigned shw){
    unsigned long long packed=0; unsigned bits=32;
    switch(type){
    case 0: packed=((unsigned long long)_rx_clampu8f(b.vector4_f32[3])<<24)|((unsigned)_rx_clampu8f(b.vector4_f32[0])<<16)
                  |((unsigned)_rx_clampu8f(b.vector4_f32[1])<<8)|_rx_clampu8f(b.vector4_f32[2]); break;
    case 3: packed=((unsigned)_rx_f32_to_f16(b.vector4_f32[0])<<16)|_rx_f32_to_f16(b.vector4_f32[1]); break;
    case 5: packed=((unsigned long long)_rx_f32_to_f16(b.vector4_f32[0])<<48)|((unsigned long long)_rx_f32_to_f16(b.vector4_f32[1])<<32)
                  |((unsigned long long)_rx_f32_to_f16(b.vector4_f32[2])<<16)|_rx_f32_to_f16(b.vector4_f32[3]); bits=64; break;
    case 1: packed=((unsigned)(int)(b.vector4_f32[0]*32767.0f)&0xFFFFu)<<16 | ((unsigned)(int)(b.vector4_f32[1]*32767.0f)&0xFFFFu); break;
    case 4: packed=((unsigned long long)((unsigned)(int)(b.vector4_f32[0]*32767.0f)&0xFFFFu)<<48)|((unsigned long long)((unsigned)(int)(b.vector4_f32[1]*32767.0f)&0xFFFFu)<<32)
                  |(((unsigned)(int)(b.vector4_f32[2]*32767.0f)&0xFFFFu)<<16)|((unsigned)(int)(b.vector4_f32[3]*32767.0f)&0xFFFFu); bits=64; break;
    default: packed=((unsigned long long)_rx_clampu8f(b.vector4_f32[0])<<24)|((unsigned)_rx_clampu8f(b.vector4_f32[1])<<16)
                  |((unsigned)_rx_clampu8f(b.vector4_f32[2])<<8)|_rx_clampu8f(b.vector4_f32[3]); break;
    }
    __vector4 r=t;
    if(mask==1 || bits==32){ r.vector4_u32[shw&3u]=(unsigned)packed; }
    else { unsigned w=(shw&3u); if(w>2u) w=2u; r.vector4_u32[w]=(unsigned)(packed>>32); r.vector4_u32[w+1u]=(unsigned)packed; }
    return r;
}

/* ============== long-tail surface (full stock VectorIntrinsics parity) ===== */
#define _RXVU(...) (__extension__({ __vector4 _r; _r.vu=(__vu4_t)(__VA_ARGS__); _r; }))
typedef vector signed char    __vsc16_t; typedef vector unsigned char  __vuc16_t;
typedef vector signed short   __vss8_t;  typedef vector unsigned short __vus8_t;
typedef vector signed int     __vsi4_t;  typedef vector unsigned int   __vui4_t;
#define _AS(T,x) ((T)(x).vu)

/* zero */
_RXVMX __vector4 __vzero(void){ return _RXVU((__vu4_t){0,0,0,0}); }

/* integer add (modulo / saturate, signed+unsigned, b/h/w) */
_RXVMX __vector4 __vaddubm(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)(_AS(__vuc16_t,a)+_AS(__vuc16_t,b))); }
_RXVMX __vector4 __vadduhm(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)(_AS(__vus8_t,a)+_AS(__vus8_t,b))); }
_RXVMX __vector4 __vadduwm(__vector4 a,__vector4 b){ return _RXVU(a.vu+b.vu); }
_RXVMX __vector4 __vaddubs(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)vec_adds(_AS(__vuc16_t,a),_AS(__vuc16_t,b))); }
_RXVMX __vector4 __vadduhs(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)vec_adds(_AS(__vus8_t,a),_AS(__vus8_t,b))); }
_RXVMX __vector4 __vadduws(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)vec_adds(a.vu,b.vu)); }
_RXVMX __vector4 __vaddsbs(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)vec_adds(_AS(__vsc16_t,a),_AS(__vsc16_t,b))); }
_RXVMX __vector4 __vaddshs(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)vec_adds(_AS(__vss8_t,a),_AS(__vss8_t,b))); }
_RXVMX __vector4 __vaddsws(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)vec_adds(_AS(__vsi4_t,a),_AS(__vsi4_t,b))); }
_RXVMX __vector4 __vaddcuw(__vector4 a,__vector4 b){ return _RXVU(vec_addc(a.vu,b.vu)); }
/* integer sub */
_RXVMX __vector4 __vsububm(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)(_AS(__vuc16_t,a)-_AS(__vuc16_t,b))); }
_RXVMX __vector4 __vsubuhm(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)(_AS(__vus8_t,a)-_AS(__vus8_t,b))); }
_RXVMX __vector4 __vsubuwm(__vector4 a,__vector4 b){ return _RXVU(a.vu-b.vu); }
_RXVMX __vector4 __vsububs(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)vec_subs(_AS(__vuc16_t,a),_AS(__vuc16_t,b))); }
_RXVMX __vector4 __vsubuhs(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)vec_subs(_AS(__vus8_t,a),_AS(__vus8_t,b))); }
_RXVMX __vector4 __vsubuws(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)vec_subs(a.vu,b.vu)); }
_RXVMX __vector4 __vsubsbs(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)vec_subs(_AS(__vsc16_t,a),_AS(__vsc16_t,b))); }
_RXVMX __vector4 __vsubshs(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)vec_subs(_AS(__vss8_t,a),_AS(__vss8_t,b))); }
_RXVMX __vector4 __vsubsws(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)vec_subs(_AS(__vsi4_t,a),_AS(__vsi4_t,b))); }
_RXVMX __vector4 __vsubcuw(__vector4 a,__vector4 b){ return _RXVU(vec_subc(a.vu,b.vu)); }
/* averages */
_RXVMX __vector4 __vavgub(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)vec_avg(_AS(__vuc16_t,a),_AS(__vuc16_t,b))); }
_RXVMX __vector4 __vavguh(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)vec_avg(_AS(__vus8_t,a),_AS(__vus8_t,b))); }
_RXVMX __vector4 __vavguw(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)vec_avg(a.vu,b.vu)); }
_RXVMX __vector4 __vavgsb(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)vec_avg(_AS(__vsc16_t,a),_AS(__vsc16_t,b))); }
_RXVMX __vector4 __vavgsh(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)vec_avg(_AS(__vss8_t,a),_AS(__vss8_t,b))); }
_RXVMX __vector4 __vavgsw(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)vec_avg(_AS(__vsi4_t,a),_AS(__vsi4_t,b))); }
/* integer max/min */
_RXVMX __vector4 __vmaxub(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)vec_max(_AS(__vuc16_t,a),_AS(__vuc16_t,b))); }
_RXVMX __vector4 __vmaxuh(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)vec_max(_AS(__vus8_t,a),_AS(__vus8_t,b))); }
_RXVMX __vector4 __vmaxuw(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)vec_max(a.vu,b.vu)); }
_RXVMX __vector4 __vmaxsb(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)vec_max(_AS(__vsc16_t,a),_AS(__vsc16_t,b))); }
_RXVMX __vector4 __vmaxsh(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)vec_max(_AS(__vss8_t,a),_AS(__vss8_t,b))); }
_RXVMX __vector4 __vmaxsw(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)vec_max(_AS(__vsi4_t,a),_AS(__vsi4_t,b))); }
_RXVMX __vector4 __vminub(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)vec_min(_AS(__vuc16_t,a),_AS(__vuc16_t,b))); }
_RXVMX __vector4 __vminuh(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)vec_min(_AS(__vus8_t,a),_AS(__vus8_t,b))); }
_RXVMX __vector4 __vminuw(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)vec_min(a.vu,b.vu)); }
_RXVMX __vector4 __vminsb(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)vec_min(_AS(__vsc16_t,a),_AS(__vsc16_t,b))); }
_RXVMX __vector4 __vminsh(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)vec_min(_AS(__vss8_t,a),_AS(__vss8_t,b))); }
_RXVMX __vector4 __vminsw(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)vec_min(_AS(__vsi4_t,a),_AS(__vsi4_t,b))); }
/* integer compares (non-R; R variants also set CR6 -- aliased here) */
_RXVMX __vector4 __vcmpequb(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)vec_cmpeq(_AS(__vuc16_t,a),_AS(__vuc16_t,b))); }
_RXVMX __vector4 __vcmpequh(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)vec_cmpeq(_AS(__vus8_t,a),_AS(__vus8_t,b))); }
_RXVMX __vector4 __vcmpgtub(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)vec_cmpgt(_AS(__vuc16_t,a),_AS(__vuc16_t,b))); }
_RXVMX __vector4 __vcmpgtuh(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)vec_cmpgt(_AS(__vus8_t,a),_AS(__vus8_t,b))); }
_RXVMX __vector4 __vcmpgtuw(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)vec_cmpgt(a.vu,b.vu)); }
_RXVMX __vector4 __vcmpgtsb(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)vec_cmpgt(_AS(__vsc16_t,a),_AS(__vsc16_t,b))); }
_RXVMX __vector4 __vcmpgtsh(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)vec_cmpgt(_AS(__vss8_t,a),_AS(__vss8_t,b))); }
_RXVMX __vector4 __vcmpgtsw(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)vec_cmpgt(_AS(__vsi4_t,a),_AS(__vsi4_t,b))); }
/* record-form (R) compares are defined at the end (they use the integer
   compares + _AS helpers declared later). */
/* VMX128 convert aliases (scale immediate) */
#define __vcfpsxws(a,uimm) __vctsxs((a),(uimm))
#define __vcfpuxws(a,uimm) __vctuxs((a),(uimm))
#define __vcsxwfp(a,uimm)  __vcfsx((a),(uimm))
#define __vcuxwfp(a,uimm)  __vcfux((a),(uimm))
/* VMX128 dot products (same as vmsum) */
_RXVMX __vector4 __vdot3fp(__vector4 a,__vector4 b){ return __vmsum3fp(a,b); }
_RXVMX __vector4 __vdot4fp(__vector4 a,__vector4 b){ return __vmsum4fp(a,b); }
/* merge low (byte/half); high/low word already above */
_RXVMX __vector4 __vmrglb(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)vec_mergel(_AS(__vuc16_t,a),_AS(__vuc16_t,b))); }
_RXVMX __vector4 __vmrglh(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)vec_mergel(_AS(__vus8_t,a),_AS(__vus8_t,b))); }
/* more pack/unpack */
_RXVMX __vector4 __vpkshus(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)vec_packsu(_AS(__vss8_t,a),_AS(__vss8_t,b))); }
_RXVMX __vector4 __vpkswus(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)vec_packsu(_AS(__vsi4_t,a),_AS(__vsi4_t,b))); }
_RXVMX __vector4 __vpkuhum(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)vec_pack(_AS(__vus8_t,a),_AS(__vus8_t,b))); }
_RXVMX __vector4 __vpkuwum(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)vec_pack(a.vu,b.vu)); }
_RXVMX __vector4 __vpkpx  (__vector4 a,__vector4 b){ return _RXVU((__vu4_t)vec_packpx(a.vu,b.vu)); }
_RXVMX __vector4 __vupklsb(__vector4 a){ return _RXVU((__vu4_t)vec_unpackl(_AS(__vsc16_t,a))); }
_RXVMX __vector4 __vupklsh(__vector4 a){ return _RXVU((__vu4_t)vec_unpackl(_AS(__vss8_t,a))); }
_RXVMX __vector4 __vupkhpx(__vector4 a){ return _RXVU((__vu4_t)vec_unpackh((vector pixel)a.vu)); }
_RXVMX __vector4 __vupklpx(__vector4 a){ return _RXVU((__vu4_t)vec_unpackl((vector pixel)a.vu)); }
/* rotates / shifts (by vector) */
_RXVMX __vector4 __vrlb(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)vec_rl(_AS(__vuc16_t,a),_AS(__vuc16_t,b))); }
_RXVMX __vector4 __vrlh(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)vec_rl(_AS(__vus8_t,a),_AS(__vus8_t,b))); }
_RXVMX __vector4 __vrlw(__vector4 a,__vector4 b){ return _RXVU(vec_rl(a.vu,b.vu)); }
_RXVMX __vector4 __vslb(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)vec_sl(_AS(__vuc16_t,a),_AS(__vuc16_t,b))); }
_RXVMX __vector4 __vslh(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)vec_sl(_AS(__vus8_t,a),_AS(__vus8_t,b))); }
_RXVMX __vector4 __vsrb(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)vec_sr(_AS(__vuc16_t,a),_AS(__vuc16_t,b))); }
_RXVMX __vector4 __vsrh(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)vec_sr(_AS(__vus8_t,a),_AS(__vus8_t,b))); }
_RXVMX __vector4 __vsrab(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)vec_sra(_AS(__vsc16_t,a),_AS(__vuc16_t,b))); }
_RXVMX __vector4 __vsrah(__vector4 a,__vector4 b){ return _RXVU((__vu4_t)vec_sra(_AS(__vss8_t,a),_AS(__vus8_t,b))); }
/* whole-vector shifts: by bits (vsl/vsr), by octets (vslo/vsro) */
_RXVMX __vector4 __vsl (__vector4 a,__vector4 b){ return _RXVU(vec_sll(a.vu,_AS(__vuc16_t,b))); }
_RXVMX __vector4 __vsr (__vector4 a,__vector4 b){ return _RXVU(vec_srl(a.vu,_AS(__vuc16_t,b))); }
_RXVMX __vector4 __vslo(__vector4 a,__vector4 b){ return _RXV((__vf4_t)vec_slo(a.vf,_AS(__vsc16_t,b))); }
_RXVMX __vector4 __vsro(__vector4 a,__vector4 b){ return _RXV((__vf4_t)vec_sro(a.vf,_AS(__vsc16_t,b))); }
/* splats (immediate element / immediate value) */
#define __vspltb(a,uimm)  _RXVU((__vu4_t)vec_splat(_AS(__vuc16_t,a),(uimm)))
#define __vspltisb(simm)  _RXVU((__vu4_t)vec_splat_s8(simm))
#define __vspltish(simm)  _RXVU((__vu4_t)vec_splat_s16(simm))
/* element loads/stores (and their volatile twins) */
_RXVMX __vector4 __lvebx(const void* b,int o){ return _RXVU((__vu4_t)vec_lde(o,(const unsigned char*)b)); }
_RXVMX __vector4 __lvehx(const void* b,int o){ return _RXVU((__vu4_t)vec_lde(o,(const unsigned short*)b)); }
_RXVMX __vector4 __lvewx(const void* b,int o){ return _RXVU((__vu4_t)vec_lde(o,(const unsigned int*)b)); }
_RXVMX void __stvebx(__vector4 s,void* b,int o){ vec_ste((vector unsigned char)s.vu,o,(unsigned char*)b); }
/* volatile aliases (clang ignores the volatile distinction here) */
_RXVMX __vector4 __lvlx_volatile(const volatile void* b,int o){ return __lvlx((const void*)b,o); }
_RXVMX __vector4 __lvrx_volatile(const volatile void* b,int o){ return __lvrx((const void*)b,o); }
_RXVMX __vector4 __lvsl_volatile(const volatile void* b,int o){ return __lvsl((const void*)b,o); }
_RXVMX __vector4 __lvsr_volatile(const volatile void* b,int o){ return __lvsr((const void*)b,o); }
_RXVMX __vector4 __lvebx_volatile(const volatile void* b,int o){ return __lvebx((const void*)b,o); }
_RXVMX __vector4 __lvehx_volatile(const volatile void* b,int o){ return __lvehx((const void*)b,o); }
_RXVMX __vector4 __lvewx_volatile(const volatile void* b,int o){ return __lvewx((const void*)b,o); }
_RXVMX void __stvlx_volatile(__vector4 s,volatile void* b,int o){ __stvlx(s,(void*)b,o); }
_RXVMX void __stvrx_volatile(__vector4 s,volatile void* b,int o){ __stvrx(s,(void*)b,o); }
_RXVMX void __stvebx_volatile(__vector4 s,volatile void* b,int o){ __stvebx(s,(void*)b,o); }
_RXVMX void __stvehx_volatile(__vector4 s,volatile void* b,int o){ __stvehx(s,(void*)b,o); }
_RXVMX void __stvewx_volatile(__vector4 s,volatile void* b,int o){ __stvewx(s,(void*)b,o); }
/* record-form (R) compares: also write CR6 to *pCR. CR6 layout matches xnamath's
   XM_CRMASK_CR6: bit 0x80 = "all elements true", bit 0x20 = "all elements false". */
#define _RXCR(allT,allF) ((unsigned)((allT)?0x80u:0u) | (unsigned)((allF)?0x20u:0u))
_RXVMX __vector4 __vcmpeqfpR(__vector4 a,__vector4 b,unsigned* pCR){ *pCR=_RXCR(vec_all_eq(a.vf,b.vf), vec_all_ne(a.vf,b.vf)); return _RXV((__vf4_t)__builtin_altivec_vcmpeqfp(a.vf,b.vf)); }
_RXVMX __vector4 __vcmpgefpR(__vector4 a,__vector4 b,unsigned* pCR){ *pCR=_RXCR(vec_all_ge(a.vf,b.vf), !vec_any_ge(a.vf,b.vf)); return _RXV((__vf4_t)__builtin_altivec_vcmpgefp(a.vf,b.vf)); }
_RXVMX __vector4 __vcmpgtfpR(__vector4 a,__vector4 b,unsigned* pCR){ *pCR=_RXCR(vec_all_gt(a.vf,b.vf), !vec_any_gt(a.vf,b.vf)); return _RXV((__vf4_t)__builtin_altivec_vcmpgtfp(a.vf,b.vf)); }
_RXVMX __vector4 __vcmpbfpR (__vector4 a,__vector4 b,unsigned* pCR){ *pCR=_RXCR(vec_all_in(a.vf,b.vf), 0); return _RXV((__vf4_t)__builtin_altivec_vcmpbfp(a.vf,b.vf)); }
_RXVMX __vector4 __vcmpequwR(__vector4 a,__vector4 b,unsigned* pCR){ *pCR=_RXCR(vec_all_eq(a.vu,b.vu), vec_all_ne(a.vu,b.vu)); return _RXVU((__vu4_t)vec_cmpeq(a.vu,b.vu)); }
_RXVMX __vector4 __vcmpequbR(__vector4 a,__vector4 b,unsigned* pCR){ *pCR=_RXCR(vec_all_eq(_AS(__vuc16_t,a),_AS(__vuc16_t,b)), vec_all_ne(_AS(__vuc16_t,a),_AS(__vuc16_t,b))); return __vcmpequb(a,b); }
_RXVMX __vector4 __vcmpequhR(__vector4 a,__vector4 b,unsigned* pCR){ *pCR=_RXCR(vec_all_eq(_AS(__vus8_t,a),_AS(__vus8_t,b)), vec_all_ne(_AS(__vus8_t,a),_AS(__vus8_t,b))); return __vcmpequh(a,b); }
_RXVMX __vector4 __vcmpgtuwR(__vector4 a,__vector4 b,unsigned* pCR){ *pCR=_RXCR(vec_all_gt(a.vu,b.vu), !vec_any_gt(a.vu,b.vu)); return __vcmpgtuw(a,b); }
_RXVMX __vector4 __vcmpgtubR(__vector4 a,__vector4 b,unsigned* pCR){ *pCR=_RXCR(vec_all_gt(_AS(__vuc16_t,a),_AS(__vuc16_t,b)), !vec_any_gt(_AS(__vuc16_t,a),_AS(__vuc16_t,b))); return __vcmpgtub(a,b); }
_RXVMX __vector4 __vcmpgtuhR(__vector4 a,__vector4 b,unsigned* pCR){ *pCR=_RXCR(vec_all_gt(_AS(__vus8_t,a),_AS(__vus8_t,b)), !vec_any_gt(_AS(__vus8_t,a),_AS(__vus8_t,b))); return __vcmpgtuh(a,b); }
_RXVMX __vector4 __vcmpgtswR(__vector4 a,__vector4 b,unsigned* pCR){ *pCR=_RXCR(vec_all_gt(_AS(__vsi4_t,a),_AS(__vsi4_t,b)), !vec_any_gt(_AS(__vsi4_t,a),_AS(__vsi4_t,b))); return __vcmpgtsw(a,b); }
_RXVMX __vector4 __vcmpgtsbR(__vector4 a,__vector4 b,unsigned* pCR){ *pCR=_RXCR(vec_all_gt(_AS(__vsc16_t,a),_AS(__vsc16_t,b)), !vec_any_gt(_AS(__vsc16_t,a),_AS(__vsc16_t,b))); return __vcmpgtsb(a,b); }
_RXVMX __vector4 __vcmpgtshR(__vector4 a,__vector4 b,unsigned* pCR){ *pCR=_RXCR(vec_all_gt(_AS(__vss8_t,a),_AS(__vss8_t,b)), !vec_any_gt(_AS(__vss8_t,a),_AS(__vss8_t,b))); return __vcmpgtsh(a,b); }

/* stock helper macros (from VectorIntrinsics.h, which this header suppresses) */
#define VPERMWI_CONST(x,y,z,w) ((((x)&3)<<6)|(((y)&3)<<4)|(((z)&3)<<2)|((w)&3))
#define __loadunalignedvector(ptr)        __vor(__lvlx((ptr),0), __lvrx((ptr),16))
#define __storeunalignedvector(v,ptr)     (__stvlx((v),(ptr),0), __stvrx((v),(ptr),16))
#define __loadunalignedvectorelement(v,ptr,dstelem) {\
    __vector4 tmp = __lvlx((ptr),0);\
    v = __vrlimi((v), tmp, 0x8>>(dstelem), (4-(dstelem))%4);\
}

/* reserved-register access intrinsics (/QVMXReserve) -- unused by xnamath */
_RXVMX __vector4 __VMXGetReg(const int r){ (void)r; return __vzero(); }
_RXVMX void      __VMXSetReg(const int r,__vector4 v){ (void)r; (void)v; }

/* ---- PPC scalar FP convert intrinsics ------------------------------------
   ppcintrinsics.h declares these extern (MSVC treats them as instructions);
   clang emits a call, so define them here. gnu_inline (extern inline) coexists
   with the stock extern prototype and emits no symbol -- used by xnamath range
   reduction (XMScalarModAngle/XMScalarSinCos etc.). The FPR holds the converted
   integer bit pattern, matching the fctidz/fcfid instruction pair.

   These MUST emit the classic FP-only convert instructions. The clang
   __builtin_ppc_fctidz/fcfid builtins lower through the VSX scalar converts
   (xscvdpsxds/xscvsxddp/xscvdpsxws) -- VSX is Power ISA 2.06+, which the Xenon
   VPU does NOT implement, so those faulted with 0xC000001D (illegal instruction)
   on hardware. Emit the exact FPR->FPR mnemonics via inline asm instead. */
#define _RXPPC extern __inline__ __attribute__((__gnu_inline__,__always_inline__,__nodebug__))
_RXPPC double __fctidz(double f){ double r; __asm__("fctidz %0,%1" : "=f"(r) : "f"(f)); return r; }
_RXPPC double __fctiwz(double f){ double r; __asm__("fctiwz %0,%1" : "=f"(r) : "f"(f)); return r; }
_RXPPC double __fctid (double f){ double r; __asm__("fctid %0,%1"  : "=f"(r) : "f"(f)); return r; }
_RXPPC double __fctiw (double f){ double r; __asm__("fctiw %0,%1"  : "=f"(r) : "f"(f)); return r; }
_RXPPC double __fcfid (double f){ double r; __asm__("fcfid %0,%1"  : "=f"(r) : "f"(f)); return r; }

#ifdef __cplusplus
}
#endif

#endif /* __RXDK_CLANG_VMX_INTRINSICS_H__ */
