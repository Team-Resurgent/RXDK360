/*
 * 2026 - Team Resurgent
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Part of RXDK - see LICENSE.md for the full GNU GPL v3.
 *
 * Regression for the 64-bit signed (arithmetic) right shift on the 32-bit PPC
 * target, exactly as the XDK's D3DTAG_MASKENCODE macro uses it:
 *
 *   ((UINT64)((INT64)0x8000000000000000 >> (EndBit-StartBit)) >> StartBit)
 *
 * D3DDevice_SetStreamSource_Inline (d3d9.h) builds its per-draw "fetch constant
 * dirty" mask this way. If the toolchain lowers the 64-bit arithmetic shift
 * wrong on ppc32, the mask is wrong, the vertex fetch constants are never
 * re-programmed per draw, and every mesh renders the first mesh's vertex buffer.
 * Inputs are volatile so the shift is a real runtime lowering, not folded.
 */
#include "rxdk_test.h"

static unsigned long long tagmask(int startbit, int endbit)
{
    return (unsigned long long)((long long)0x8000000000000000LL >> (endbit - startbit))
           >> startbit;
}

static int eq64(unsigned long long a, unsigned long long b) { return a == b; }

int main(void)
{
    volatile int sb, eb;
    sb = 32; eb = 63; CHECK(eq64(tagmask(sb, eb), 0x00000000FFFFFFFFULL), "tagmask(32,63)=0xFFFFFFFF");
    sb = 63; eb = 63; CHECK(eq64(tagmask(sb, eb), 0x0000000000000001ULL), "tagmask(63,63)=1");
    sb = 0;  eb = 63; CHECK(eq64(tagmask(sb, eb), 0xFFFFFFFFFFFFFFFFULL), "tagmask(0,63)=all");
    sb = 32; eb = 32; CHECK(eq64(tagmask(sb, eb), 0x0000000080000000ULL), "tagmask(32,32)=bit31");
    sb = 48; eb = 50; CHECK(eq64(tagmask(sb, eb), 0x000000000000E000ULL), "tagmask(48,50)");
    sb = 40; eb = 40; CHECK(eq64(tagmask(sb, eb), 0x0000000000800000ULL), "tagmask(40,40)=bit23");

    /* the plain building blocks, isolated */
    {
        volatile long long v = (long long)0x8000000000000000LL;
        volatile int n = 31;
        CHECK(eq64((unsigned long long)(v >> n), 0xFFFFFFFF00000000ULL), "INT64_MIN >>a 31 sign-extends");
        n = 0;
        CHECK(eq64((unsigned long long)(v >> n), 0x8000000000000000ULL), "INT64_MIN >>a 0");
        volatile unsigned long long u = 0x8000000000000000ULL;
        n = 63;
        CHECK(eq64(u >> n, 0x0000000000000001ULL), "1<<63 >>l 63 = 1");
    }

    CHECK_DONE("tagmask");
    return 0;
}
