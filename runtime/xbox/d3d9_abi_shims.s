/*
 * 2026 - Team Resurgent
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Part of RXDK - see LICENSE.md for the full GNU GPL v3.
 */

/*
 * INTERIM ABI bridge for d3d9 entry points that take a 64-bit integer BY VALUE.
 *
 * The Xbox 360 (Xenon) is a 64-bit PowerPC and the MS ABI passes a 64-bit
 * integer argument in a SINGLE 64-bit GPR. Our clang currently targets 32-bit
 * `powerpc`, where a `unsigned long long` argument is split across a register
 * PAIR (rN = high 32, rN+1 = low 32). So when our clang-compiled titles call the
 * translated MSVC d3d9, the callee reads only the high half and the low 32 bits
 * are dropped -- proven root cause of the AdvancedLighting vertex-fetch "spikes"
 * (D3DDevice_SetStreamSource's UINT64 `pendingMask3` dirty bit lives in the low
 * word, so the fetch constant was never marked dirty and every mesh drew the
 * first mesh's vertex buffer).
 *
 * These shims are linked with `ld.lld --wrap=<fn>` (added by tools/mktitle.py):
 * every title reference to <fn> resolves to __wrap_<fn> here, and __real_<fn>
 * resolves to the real translated entry point. Each shim re-packs the incoming
 * r(N):r(N+1) high:low pair into a single 64-bit rN and tail-calls the real fn.
 * The `rldicr`/`rldicl`/`or` (64-bit) ops assemble fine on the 32-bit target
 * (the assembler emits them even though the compiler will not).
 *
 * *** REMOVE these shims (and the mktitle --wrap flags) once clang is retargeted
 * to powerpc64-unknown-xbox360 (docs/ilp32-ppc64-abi-plan.md): the fixed clang
 * passes the 64-bit arg in a single register, and this re-pack would then CORRUPT
 * it. The shim and the ABI fix are mutually exclusive. ***
 */

    .text

/* D3DDevice_SetStreamSource(D3DDevice* r3, UINT StreamNumber r4,
 *   D3DVertexBuffer* pStreamData r5, UINT Offset r6, UINT Stride r7,
 *   UINT64 PendingMask3  -- our clang: r8=high, r9=low; real fn wants single r8) */
    .globl __wrap_D3DDevice_SetStreamSource
    .type  __wrap_D3DDevice_SetStreamSource, @function
__wrap_D3DDevice_SetStreamSource:
    rldicr 8, 8, 32, 31        /* r8 = high << 32 (keep bits 0..31) */
    rldicl 9, 9, 0, 32         /* r9 = low & 0xFFFFFFFF (clear bits 0..31) */
    or     8, 8, 9             /* r8 = (high << 32) | low  -> the full 64-bit value */
    b      __real_D3DDevice_SetStreamSource   /* tail call; LR/args r3..r7 unchanged */
    .size  __wrap_D3DDevice_SetStreamSource, . - __wrap_D3DDevice_SetStreamSource
