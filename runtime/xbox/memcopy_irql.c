/*
 * 2026 - Team Resurgent
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Part of RXDK - see LICENSE.md for the full GNU GPL v3.
 */

/*
 * memcpy / memmove / mempcpy that are safe at any IRQL.
 *
 * picolibc's generic copies are auto-vectorized for the Xenon (lvx/vperm/stvx),
 * which is what a title thread wants. But the kernel forbids floating-point and
 * vector use at DPC time: on a devkit, a DPC that called the vectorized memcpy
 * bugchecked the console with ATTEMPTED_FPU_USE_FROM_DPC (0xF0; the kernel
 * crash dump's exception record), while the same DPC copying with integer loads
 * and stores ran fine. The prebuilt XDK libraries a title links resolve memcpy
 * to this libc too, and their DPCs copy.
 *
 * build_libc.py renames picolibc's three to __rxdk_*_vector (llvm-objcopy
 * --redefine-sym, since the sources #undef their own names), and these entry
 * points pick: the vector copy at PASSIVE_LEVEL, an integer copy above it. This
 * file is compiled with -mno-altivec -msoft-float, so the compiler cannot put a
 * VMX or FP register in it. The IRQL is the byte at 0x18 in the processor
 * control region, which r13 points at.
 */
#include <stddef.h>
#include <stdint.h>

void *__rxdk_memcpy_vector(void *restrict dst, const void *restrict src, size_t n);
void *__rxdk_memmove_vector(void *dst, const void *src, size_t n);
void *__rxdk_mempcpy_vector(void *restrict dst, const void *restrict src, size_t n);

static inline unsigned current_irql(void)
{
    unsigned irql;
    __asm__ volatile("lbz %0, 0x18(13)" : "=r"(irql));
    return irql;
}

/* Integer copies: doublewords when both pointers share their alignment, bytes
   otherwise. Forward and backward variants for memmove. */
static void copy_forward(unsigned char *d, const unsigned char *s, size_t n)
{
    if ((((uintptr_t)d ^ (uintptr_t)s) & 7) == 0) {
        while (n && ((uintptr_t)d & 7)) { *d++ = *s++; n--; }
        for (; n >= 8; n -= 8, d += 8, s += 8)
            *(uint64_t *)d = *(const uint64_t *)s;
    }
    while (n--) *d++ = *s++;
}

static void copy_backward(unsigned char *d, const unsigned char *s, size_t n)
{
    d += n;
    s += n;
    if ((((uintptr_t)d ^ (uintptr_t)s) & 7) == 0) {
        while (n && ((uintptr_t)d & 7)) { *--d = *--s; n--; }
        for (; n >= 8; n -= 8) {
            d -= 8;
            s -= 8;
            *(uint64_t *)d = *(const uint64_t *)s;
        }
    }
    while (n--) *--d = *--s;
}

void *memcpy(void *restrict dst, const void *restrict src, size_t n)
{
    if (__builtin_expect(current_irql() != 0, 0)) {    /* above PASSIVE_LEVEL */
        copy_forward((unsigned char *)dst, (const unsigned char *)src, n);
        return dst;
    }
    return __rxdk_memcpy_vector(dst, src, n);
}

void *mempcpy(void *restrict dst, const void *restrict src, size_t n)
{
    if (__builtin_expect(current_irql() != 0, 0)) {
        copy_forward((unsigned char *)dst, (const unsigned char *)src, n);
        return (unsigned char *)dst + n;
    }
    return __rxdk_mempcpy_vector(dst, src, n);
}

void *memmove(void *dst, const void *src, size_t n)
{
    if (__builtin_expect(current_irql() != 0, 0)) {
        unsigned char *d = (unsigned char *)dst;
        const unsigned char *s = (const unsigned char *)src;
        if (d < s || d >= s + n)
            copy_forward(d, s, n);
        else
            copy_backward(d, s, n);
        return dst;
    }
    return __rxdk_memmove_vector(dst, src, n);
}
