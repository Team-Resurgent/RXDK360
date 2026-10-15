<!--
SPDX-License-Identifier: GPL-3.0-or-later
2026 - Team Resurgent - Part of RXDK.
-->

# Scoping plan: an ILP32-on-ppc64 Xbox 360 target (correct 64-bit-by-value ABI)

## Problem

Our toolchain builds titles with the triple `powerpc-unknown-xbox360` — a **32-bit
PowerPC** target (`-target-cpu ppc`). On a 32-bit PPC target `i64` is illegal, so a
`long long` / `ULONGLONG` passed or returned **by value** is legalized into an
**r3:r4 register pair** (r3 = high 32, r4 = low 32), and the backend emits only
32-bit instructions (no `std`/`ld`/`rldicl`).

The real Xbox 360 (Xenon) is a **64-bit** PowerPC. The Microsoft compiler passes
and returns a 64-bit value in a **single 64-bit register** (`rldicr`/`std`,
`addi r3,r3,1`, `xor r3,r3,r4`). The two ABIs therefore disagree wherever a 64-bit
integer crosses the **our-clang ↔ MS-compiled boundary by value**.

Ground truth (verified `ret_const`/`add64`/`passthru`, our clang vs cl.exe):

| | our clang (`powerpc-…`) | cl.exe (MS) |
|---|---|---|
| return `u64` | r3 = high, r4 = low (pair) | single 64-bit r3 |
| `u64` arg | r3:r4, r5:r6 pairs | one 64-bit reg each |
| 64-bit ops | 32-bit sequences only | native `rldicr`/`std` |

**Impact is latent / near-zero today.** Within our own clang-compiled code i64 is
self-consistent and works; the mismatch only bites when our clang directly
calls (or is called by) MS-compiled code passing i64 *by value*, which is rare —
360 APIs pass 64-bit values through `LARGE_INTEGER*` pointers. Evidence it is not
currently hurting anything: the stdlib suite is 54/54 · 800/800 (incl. timers),
and vcomp's own `omp_get_wtick`/`omp_get_wtime` are correct (the "5.305e-315
denormal" was only a `%g` vararg-FP `DbgPrint` display artifact; the raw bits were
`0x3E55798E…` = 2e-8). This plan is therefore **optional**, to be scheduled only
if real i64-by-value interop with MS-compiled code is needed.

## The fix in one line

Retarget titles from the 32-bit `powerpc-unknown-xbox360` to a **64-bit-register /
32-bit-pointer** model (`powerpc64-unknown-xbox360`), which makes `i64` a legal
type carried in a single 64-bit GPR — byte-for-byte the MS ABI.

**This is already proven.** Compiling with `--target=powerpc64-unknown-xbox360`
produces exactly cl.exe's codegen: `ret_const` → single r3 (`rldic`), `add64` →
`addi 3,3,1`, `passthru` → `xor 3,4,3`.

## What already exists (do NOT rebuild)

The ILP32-on-ppc64 target is **partially present** in our LLVM fork
(Team-Resurgent/llvm-project, branch `xbox`):

- **clang target info** — `clang/lib/Basic/Targets.cpp` maps arch `ppc64` + OS
  `Xbox360` to `Xbox360TargetInfo<PPC64TargetInfo>`
  (`clang/lib/Basic/Targets/OSTargets.h`), which sets `PointerWidth = 32`,
  `SizeType = UnsignedInt`, `Int64Type = SignedLongLong`. The emitted datalayout
  is `E-m:e-p:32:32-Fi64-i64:64-i128:128-n32:64` — **32-bit pointers, 64-bit
  native integers**. That is the ILP32-on-ppc64 model.
- **backend ABI** — `PPCTargetMachine.cpp` (`getPPCABI`) already returns
  `PPC_ABI_ELFv2` for `Triple::Xbox360` "has neither function descriptors nor a
  TOC", independent of arch.
- **calling convention** — `CC_PPC32_Xbox360` / `RetCC` hooks and the
  `VR_Xbox360` vector-register order (v1-based) already encode the MS slot rules,
  linkage area, and always-allocated parameter save area
  (`PPCCallingConv.cpp`, `PPCISelLowering.cpp`, gated on
  `Subtarget.isXbox360ABI()`).

The gap is that our build uses the **32-bit** `powerpc` arch (which never reaches
`Xbox360TargetInfo<PPC64TargetInfo>`), and the 64-bit path still emits artifacts
our downstream pipeline cannot consume.

## Gaps to close

1. **ELF class / machine.** A `ppc64` triple emits **ELFCLASS64**, machine
   `EM_PPC64`. Our entire downstream is **ELFCLASS32 / `EM_PPC`**: `tools/coff2elf.py`
   (COFF→ELF archives), `tools/elf2xex.py` (linked ELF→XEX), `tools/gen_import_stubs.py`,
   and the `mktitle.py` linker script. **This is the central decision (below).**
2. **Function descriptors + TOC.** Despite `getPPCABI` → ELFv2, `-S` on the
   `powerpc64` triple still emits OPD descriptors (`.quad .Lfunc_begin`,
   `.quad .TOC.@tocbase`, `.quad 0`) and `.TOC.` references. ELFv2 direct-entry
   must be fully honored for this triple: no `.opd`, functions are their own entry
   points, and the `Fi64` datalayout / any descriptor modelling reconciled with
   no-TOC. Confirm the ELFv2 path is actually reached for the `ppc64` arch (not
   only the `ppc` arch), and that `r2`/TOC setup is suppressed.
3. **Object emission.** `clang -c` for `powerpc64-unknown-xbox360` currently fails
   to produce an object (only `-S` works). The MC/object path for this triple
   needs to work end to end.
4. **Calling-convention reuse on the 64-bit path.** `CC_PPC32_Xbox360` and the
   spill logic in `LowerFormalArguments`/`LowerCall` are written for the 32-bit
   arch; verify they behave when the CC now sees a legal `i64` (one 64-bit GPR /
   one 8-byte slot) instead of two legalized `i32` halves. The "spill incoming
   args into the caller's frame" rule and 8-byte slots should carry over cleanly,
   but must be re-derived against real objects.
5. **`-target-cpu`.** Move off generic `ppc` to a Xenon-appropriate CPU that
   enables 64-bit instructions (`std`/`ld`/`rldicl`), matching cl.exe.

## The central decision: ELFCLASS32 vs adapt the pipeline to ELFCLASS64

**Option A — make the ppc64 Xbox360 target emit ELFCLASS32.** Keeps
coff2elf/elf2xex/linker untouched. But ELFCLASS32 with 64-bit-register code is a
non-standard ELF flavor (32-bit `Elf32_*` structures, `EM_PPC`, ELF32 relocations,
but ppc64 register semantics). It would require custom MC/backend ELF emission and
a bespoke lld path. **High novelty, fragile.** Not recommended.

**Option B — adopt standard ppc64 BE ELF (ELFCLASS64) end to end.** *(recommended)*
lld already links ppc64 BE ELF natively. 32-bit addresses simply live in 64-bit
ELF fields (they fit). Work is contained and uses standard formats:

- `tools/coff2elf.py`: emit ELFCLASS64 (`Elf64_Ehdr/Shdr/Sym/Rela`, `EM_PPC64`,
  8-byte `r_info` with the ppc64 reloc numbers) instead of ELF32. The COFF PPC
  relocation → ELF reloc mapping is the main surface; symbol values and section
  layout are otherwise unchanged (addresses are still ≤ 32-bit).
- `tools/elf2xex.py`: read ELFCLASS64 program/section headers (64-bit fields),
  keeping the existing XEX/basefile logic (still a flat 32-bit-addressed image).
- `tools/gen_import_stubs.py`: emit the thunk objects as ELFCLASS64; import
  descriptor format is unchanged (still `0x0100xxxx` records at 32-bit addresses).
- `mktitle.py` linker script: unchanged in substance (addresses identical); verify
  `ld.lld` selects the ppc64 BE emulation for these inputs.

Option B is more moving parts but each is mechanical and standard; Option A is
fewer files but invents a format. **Recommend Option B.**

## Phased implementation

- **Phase 0 — spike (no pipeline changes).** Get `clang -c
  --target=powerpc64-unknown-xbox360` to produce a clean ELFv2 object: no OPD, no
  `.TOC.`, single-register i64 args/returns, native 64-bit ops. Diff a handful of
  functions against cl.exe `/FAs` listings (extend the existing `abi-spike`
  fixtures). Gate: object matches MS ABI for int/ptr/float/`i64`/struct/vararg.
- **Phase 1 — backend/clang.** Close gaps 2–5: full ELFv2 direct-entry for the
  ppc64 Xbox360 arch, working object emission, the Xenon `-target-cpu`, and
  re-validated `CC_PPC32_Xbox360` behavior with legal `i64`. Rename the CC helper
  (`_PPC32_` is now a misnomer).
- **Phase 2 — pipeline (Option B).** ELFCLASS64 in coff2elf, elf2xex,
  gen_import_stubs; confirm the lld emulation. Reconvert `vcomp.a`/`xapilib.a` and
  relink one title.
- **Phase 3 — cutover + validation.** Point `mktitle.py` `MS_TRIPLE` at
  `powerpc64-unknown-xbox360`. Re-run the full regression: stdlib suite
  (54/54 · 800/800), `compare_official.py`, the EH/threads suites, the vcomp
  `omp.xex` end-to-end, and re-verify all shipped-XDK libs still link. Add an
  i64-by-value interop test (call `KeQueryPerformanceFrequency` directly from our
  clang and assert 50 MHz) — the case this whole plan is about.
- **Phase 4 — decommission the 32-bit path** only after Phase 3 is green, or keep
  both behind a flag for one release.

## Risk & recommendation

This touches the ABI foundation the entire working toolchain rests on (129 libs,
800/800, EH/threads, vcomp). It must be done on a branch with the ppc32 toolchain
kept intact until Phase 3 passes clean. Given the **near-zero present impact**,
the pragmatic default is to **not** undertake this now and instead follow the
by-value-avoidance guidance:

- Do not pass or return 64-bit integers **by value** across the our-clang ↔ MS
  boundary; use `LARGE_INTEGER*` / pointer out-params (what the XDK itself does).
- Wrap the few i64-*by-value* kernel exports —
  `KeQueryPerformanceFrequency`, `KeQueryPerformanceCounter`,
  `KeQueryInterruptTime` — to write through a pointer if our clang code needs them.
  **Done:** `runtime/xbox/ke_perf.S` provides three pointer-out accessors that
  capture the single-register result with a 64-bit `std`/`mftb` (which our
  *assembler* emits on the 32-bit target even though the *compiler* will not — a
  C wrapper cannot, it would re-read the return as an r3:r4 pair):
  `rxdk_query_perf_freq64` (→ `KeQueryPerformanceFrequency`, wired into clock.c's
  `clock_gettime(CLOCK_MONOTONIC)`/`clock()`), `rxdk_query_interrupt_time64`
  (→ `KeQueryInterruptTime`, ord 0x82), and `rxdk_query_perf_counter64` (a single
  64-bit `mftb` — the 360 has **no** `KeQueryPerformanceCounter` export; the perf
  counter *is* the time-base register, which the XDK's own
  `QueryPerformanceCounter` reads the same way). ⚠ xenia's `ResultBase::Store`
  truncates `qword_result_t` returns to 32 bits, so `KeQueryInterruptTime` reads
  back with a garbage high word under xenia once it exceeds 31 bits; the thunk is
  hardware-correct.

Schedule Phases 0–4 only when i64-by-value interop becomes a real requirement.

## Related

- `docs/abi-spike.md`, `docs/abi-complete.md`, `docs/base-target-correction.md`,
  `docs/llvm-patch-design.md` — the existing MS-PPC ABI work and its fixtures.
- Prior clang ABI fix: the printf/va_list frame-lowering bug (LR slot).

## Same root cause: variadic floating-point (`DbgPrint %g`)

The `DbgPrint("%g", d)` misprint is **not** a separate bug — it is this same
64-bit-GPR gap in the variadic path. For a variadic call the MS ABI homes an FP
argument into its positional **64-bit GPR**, not an FPR. cl.exe:

```
stfd fr1, 0x18(r1)     ; spill the double
mr   r5, r4            ; next positional arg
ld   r4, 0x18(r1)      ; reload the double's 64 bits into r4 (its slot's GPR)
bl   DbgPrint
```

Our 32-bit clang leaves the double in `f1` and leaves the shadow GPR undefined
(it sets `creqv 6,6,6` and reserves the slot but never fills it), so the MS
kernel `DbgPrint`, reading the value from the GPR, gets garbage — the observed
`5.3e-315`/`9.29e-315` denormals.

Confirmed in xenia: our own libc `snprintf("%g | %.3f | %e", …)` prints
`2e-08 | 3.142 | 5.000000e+07` (correct — our clang's FP-vararg pass and our
libc's `va_arg` are mutually consistent, so titles using `<stdio.h>` are fine),
while a *direct* `DbgPrint("%g", d)` prints garbage.

Impact is the same narrow boundary: only our-clang code calling an MS variadic
function (the kernel `DbgPrint`) with a floating-point argument **by value**. The
ILP32-on-ppc64 target fixes it for free (the `ld` into a 64-bit slot GPR is
exactly what a 64-bit-register target emits). **Interim workaround:** format with
our own libc (`snprintf` into a buffer) and pass the resulting **string** to
`DbgPrint` — never a raw FP value.
