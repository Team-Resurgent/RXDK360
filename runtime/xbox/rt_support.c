/*
 * Low-level runtime support the modern libc needs that is neither picolibc nor
 * a kernel import:
 *
 *  - the 64-bit integer division helpers the compiler emits on this 32-bit
 *    target (picolibc's integer formatting divides 64-bit values), and
 *  - a temporary allocator so malloc/free resolve.
 *
 * The division helpers use plain binary long division (only shifts, compares and
 * subtracts -- no 64-bit divide -- so with -fno-builtin they never recurse into
 * __udivdi3). The allocator is a placeholder bump allocator; the real malloc
 * will come from the console heap (RtlAllocateHeap) like RXDK-Libs' heapalloc.c.
 */
#include <stddef.h>
#include <stdint.h>

/* ---- 64-bit division (compiler-rt equivalents) --------------------------- */

unsigned long long __udivmoddi4(unsigned long long n, unsigned long long d,
                                unsigned long long *rem) {
    unsigned long long q = 0, r = 0;
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1ULL);
        if (r >= d) {
            r -= d;
            q |= (1ULL << i);
        }
    }
    if (rem)
        *rem = r;
    return q;
}

unsigned long long __udivdi3(unsigned long long a, unsigned long long b) {
    return __udivmoddi4(a, b, 0);
}

unsigned long long __umoddi3(unsigned long long a, unsigned long long b) {
    unsigned long long r;
    __udivmoddi4(a, b, &r);
    return r;
}

long long __divdi3(long long a, long long b) {
    int neg = (a < 0) ^ (b < 0);
    unsigned long long q = __udivdi3(a < 0 ? -(unsigned long long)a : a,
                                     b < 0 ? -(unsigned long long)b : b);
    return neg ? -(long long)q : (long long)q;
}

long long __moddi3(long long a, long long b) {
    unsigned long long r;
    __udivmoddi4(a < 0 ? -(unsigned long long)a : a,
                 b < 0 ? -(unsigned long long)b : b, &r);
    return a < 0 ? -(long long)r : (long long)r;
}

/* ---- 64-bit integer -> double (compiler-rt equivalents) ------------------ */

/*
 * clang lowers a 64-bit int -> double conversion to a libcall on this 32-bit
 * PPC target (the picolibc/libc++ math paths need it). The Xenon core is a
 * 64-bit CPU, so we do the conversion in one hardware instruction, fcfid (Float
 * Convert From Integer Doubleword): load the raw 64-bit pattern into an FPR and
 * let fcfid read it as a signed doubleword. There is no unsigned fcfidu on this
 * ISA, so __floatundidf halves the value first when the top bit is set (the half
 * always fits in the signed range) and doubles the result back.
 */
static inline double cvt_i64_to_f64(long long a) {
    union { long long i; double d; } u;
    u.i = a;
    double f = u.d;                 /* raw 8-byte load into an FPR (lfd) */
    __asm__("fcfid %0, %1" : "=f"(f) : "f"(f));
    return f;
}

double __floatdidf(long long a) {
    return cvt_i64_to_f64(a);
}

double __floatundidf(unsigned long long a) {
    if (a < 0x8000000000000000ULL)
        return cvt_i64_to_f64((long long)a);
    /* top bit set: a = 2*(a>>1) + (a&1); (a>>1) is always in signed range. */
    double half = cvt_i64_to_f64((long long)(a >> 1));
    return half * 2.0 + (double)(int)(a & 1u);
}

/* ---- 64-bit integer <-> float, float/double -> 64-bit integer ------------ */

/*
 * The single-precision int->float libcalls (__floatdisf/__floatundisf) and the
 * float/double -> 64-bit int libcalls (__fix*di) are lowered the same way on
 * this 32-bit PPC target. fctidz (Float Convert To Integer Doubleword, round
 * toward Zero) gives the exact truncation C requires for float->i64; the
 * int->float direction reuses fcfid (see cvt_i64_to_f64) and rounds to single
 * afterwards -- a value that needs more than 24 significant bits is inherently
 * lossy, and the double->single re-rounding is at most 1 ULP off the perfectly
 * rounded result, which no title depends on.
 */
static inline long long cvt_f64_to_i64(double f) {
    __asm__("fctidz %0, %1" : "=f"(f) : "f"(f));
    union { double d; long long i; } u;
    u.d = f;                        /* raw 8-byte store from the FPR (stfd) */
    return u.i;
}

long long __fixdfdi(double a) { return cvt_f64_to_i64(a); }
long long __fixsfdi(float a)  { return cvt_f64_to_i64((double)a); }

unsigned long long __fixunsdfdi(double a) {
    if (a < 1.0) return 0ULL;
    if (a < 9223372036854775808.0)  /* < 2^63: the signed path is exact */
        return (unsigned long long)cvt_f64_to_i64(a);
    /* a - 2^63 lands in the signed range; add the top bit back. */
    return 0x8000000000000000ULL +
           (unsigned long long)cvt_f64_to_i64(a - 9223372036854775808.0);
}
unsigned long long __fixunssfdi(float a) { return __fixunsdfdi((double)a); }

float __floatdisf(long long a)            { return (float)cvt_i64_to_f64(a); }
float __floatundisf(unsigned long long a) { return (float)__floatundidf(a); }

/* ---- allocator: the process heap (RtlAllocateHeap over XapiProcessHeap) --- */

/*
 * malloc/free live on the *process heap* (RtlAllocateHeap over XapiProcessHeap,
 * created in crt_start), exactly like the retail CRT (libcMT) -- NOT a separate
 * kernel-pool allocator. This is load-bearing: the shipped XDK libraries assume
 * malloc == HeapAlloc(GetProcessHeap()), and mix `new`/`delete`,
 * malloc/free, HeapAlloc/HeapFree and XuiAlloc/XuiFree on the same object. If
 * malloc used a different allocator (e.g. ExAllocatePool), a pointer from one
 * would be handed to the other's free: RtlFreeHeap walking a foreign pointer
 * corrupts the heap free list (observed as XUI's XuiInit trashing a HEAP_FREE_
 * ENTRY.Flink). One heap for everyone keeps the invariant. RtlAllocateHeap on
 * the 360 process heap returns 16-aligned blocks, so __vector4/XMVECTOR storage
 * is aligned without extra work.
 */
extern void *XapiProcessHeap;
extern void *RtlAllocateHeap(void *heap, unsigned flags, size_t size);
extern int   RtlFreeHeap(void *heap, unsigned flags, void *p);
extern void *RtlReAllocateHeap(void *heap, unsigned flags, void *p, size_t size);

#define RXDK_HEAP_ZERO_MEMORY 0x00000008u
/* aligned_alloc (alignment > 16) can't be expressed to RtlAllocateHeap, so it
   over-allocates a normal block and returns an aligned pointer inside it, with
   {magic, raw-block} stashed in the two words just below the user pointer so
   free() can recover and release the real block. The magic plus a bounds/align
   sanity check on the stashed pointer makes a false positive on an ordinary
   block's heap header astronomically unlikely. */
#define RXDK_ALIGN_MAGIC 0xA11C0A11u

void *malloc(size_t n) {
    return XapiProcessHeap ? RtlAllocateHeap(XapiProcessHeap, 0, n) : 0;
}

void free(void *p) {
    if (!p)
        return;
    void *raw = ((void **)p)[-1];
    if (((unsigned *)p)[-2] == RXDK_ALIGN_MAGIC &&
        (uintptr_t)raw < (uintptr_t)p &&
        (uintptr_t)p - (uintptr_t)raw < 0x10000u &&
        ((uintptr_t)raw & 15u) == 0)
        RtlFreeHeap(XapiProcessHeap, 0, raw);      /* over-aligned block */
    else
        RtlFreeHeap(XapiProcessHeap, 0, p);
}

void *calloc(size_t nmemb, size_t size) {
    if (size && nmemb > (size_t)-1 / size)   /* overflow */
        return 0;
    return XapiProcessHeap
               ? RtlAllocateHeap(XapiProcessHeap, RXDK_HEAP_ZERO_MEMORY, nmemb * size)
               : 0;
}

void *realloc(void *p, size_t n) {
    if (!p)
        return malloc(n);
    if (n == 0) {
        free(p);
        return 0;
    }
    return RtlReAllocateHeap(XapiProcessHeap, 0, p, n);
}

/* ---- MSVC compiler-version guards ---------------------------------------- */

/*
 * Every object the XDK's cl.exe (build 11886) emits references __C1_11886 and
 * __C2_11886 -- a link-time check that all objects came from the same compiler,
 * defined by the matching CRT. They carry no runtime meaning (no relocation uses
 * them), so a dummy definition is all a prebuilt object needs to link.
 */
int __C1_11886 = 0;
int __C2_11886 = 0;

/* ---- process/abort + aligned_alloc + assert ------------------------------ */

extern void HalReturnToFirmware(unsigned);

/* No process to signal on the console: abort reboots the console, as exit()
   does (routine 0 would power a devkit off). */
void abort(void) {
    HalReturnToFirmware(2);
    for (;;) {}
}

/* C11 aligned_alloc, honouring alignments larger than the 16 bytes the process
   heap already guarantees (posix_memalign, std::aligned_alloc, over-aligned
   types). alignment <= 16 is a plain heap block (freeable normally). For a
   larger alignment, over-allocate on the process heap, align the user pointer
   up, and stash {magic, raw-block} in the two words below it so free() recovers
   the real block -- keeping everything on the one process heap. Unlike C11's
   aligned_alloc, size need not be a multiple of alignment here. */
void *aligned_alloc(size_t alignment, size_t size) {
    if (!XapiProcessHeap)
        return 0;
    if (alignment <= 16u)
        return RtlAllocateHeap(XapiProcessHeap, 0, size);
    size_t total = size + alignment + 8u;        /* room for the {magic,raw} tag */
    void *raw = RtlAllocateHeap(XapiProcessHeap, 0, total);
    if (!raw)
        return 0;
    uintptr_t user = ((uintptr_t)raw + 8u + (alignment - 1)) & ~(uintptr_t)(alignment - 1);
    ((unsigned *)user)[-2] = RXDK_ALIGN_MAGIC;
    ((void **)user)[-1]    = raw;
    return (void *)user;
}

/* SVR4/BSD memalign: same (alignment, size) contract as aligned_alloc, which
   already tags over-aligned blocks so free() reclaims them. */
void *memalign(size_t alignment, size_t size) {
    return aligned_alloc(alignment, size);
}

/* picolibc's assert() (no-message form) lands here on failure. */
void __assert_no_args(void) {
    abort();
}

/* errno: picolibc.h routes errno through __rxdk_errno(). Per-thread storage via
   kernel TLS (each thread gets its own int, lazily allocated); a shared fallback
   covers the pre-TLS/allocation-failure case. */
extern unsigned KeTlsAlloc(void);
extern void    *KeTlsGetValue(unsigned index);
extern unsigned KeTlsSetValue(unsigned index, void *value);
static unsigned g_errno_key = 0xFFFFFFFFu;
static int      g_errno_fallback;
int *__rxdk_errno(void) {
    if (g_errno_key == 0xFFFFFFFFu) {
        unsigned k = KeTlsAlloc();
        if (k == 0xFFFFFFFFu) return &g_errno_fallback;
        g_errno_key = k;
    }
    int *p = (int *)KeTlsGetValue(g_errno_key);
    if (!p) {
        p = (int *)malloc(sizeof(int));
        if (!p) return &g_errno_fallback;
        *p = 0;
        KeTlsSetValue(g_errno_key, p);
    }
    return p;
}

