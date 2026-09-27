/*
 * 2026 - Team Resurgent
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Part of RXDK - see LICENSE.md for the full GNU GPL v3.
 *
 * 64-bit-argument ABI conformance against the Xenon / Xbox 360 MSVC convention.
 *
 * The Xenon is a 64-bit PowerPC; MSVC passes a 64-bit integer argument in ONE
 * 64-bit GPR (r3..r10), not a 32-bit register pair. The translated MSVC libs
 * we link (d3d9, xapilib, ...) rely on this -- e.g. D3DDevice_SetStreamSource
 * reads its UINT64 'pendingMask3' straight out of r8 ("or r11,r11,r8" over a
 * 64-bit m_Pending). If our clang (a 32-bit-pointer target) splits a 64-bit
 * argument across a register PAIR (r8:r9), the callee sees only the high half
 * and the low 32 bits are lost -- which silently corrupts every 64-bit value
 * handed to those libs (the AdvancedLighting vertex-fetch "spikes" were exactly
 * this: pendingMask3's dirty bit lives in the low word, so the fetch constant
 * was never marked dirty and every mesh drew the first mesh's vertex buffer).
 *
 * These asm shims read the argument as the single 64-bit register the Xenon ABI
 * places it in and store the full doubleword back, so the check fails loudly if
 * clang delivered only half of it.
 */
#include "rxdk_test.h"

/* void abi_store64_6th(u64* out=r3, i32=r4, i32=r5, i32=r6, i32=r7, u64 m=r8);
 * matches D3DDevice_SetStreamSource's shape: 5 word args then a 64-bit arg. */
__asm__(
"  .text\n"
"  .globl abi_store64_6th\n"
"abi_store64_6th:\n"
"  std 8, 0(3)\n"           /* *out = full 64-bit r8 */
"  blr\n"
"  .globl abi_store64_4th\n"
"abi_store64_4th:\n"        /* u64* out=r3, i32=r4, i32=r5, u64 m=r6 */
"  std 6, 0(3)\n"
"  blr\n"
);
extern void abi_store64_6th(unsigned long long* out, int a, int b, int c, int d, unsigned long long m);
extern void abi_store64_4th(unsigned long long* out, int a, int b, unsigned long long m);

static int eq64(unsigned long long a, unsigned long long b) { return a == b; }

int main(void)
{
    unsigned long long v;

    /* the exact failing shape: 64-bit as the 6th argument (after 5 words) */
    v = 0;
    abi_store64_6th(&v, 1, 2, 3, 4, 0x1122334455667788ULL);
    CHECK(eq64(v, 0x1122334455667788ULL), "u64 6th arg in one 64-bit reg (SetStreamSource shape)");

    /* low word alone must survive (this is the pendingMask3=1 case that broke) */
    v = 0;
    abi_store64_6th(&v, 0, 0, 0, 0, 0x0000000000000001ULL);
    CHECK(eq64(v, 0x0000000000000001ULL), "u64 6th arg low-word-only survives");

    /* a 64-bit as the 4th arg (after 2 words), a different register slot */
    v = 0;
    abi_store64_4th(&v, 7, 8, 0xAABBCCDD11223344ULL);
    CHECK(eq64(v, 0xAABBCCDD11223344ULL), "u64 4th arg in one 64-bit reg");

    CHECK_DONE("abi64");
    return 0;
}
