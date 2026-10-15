# The LLVM patch: design

Branch `xbox` in Team-Resurgent/llvm-project.

## The key realisation

The MS Xenon ABI is **64-bit PowerPC parameter passing with a 32-bit data model**, and
LLVM already has that shape. `computePowerDataLayout` in
`llvm/lib/TargetParser/TargetDataLayout.cpp`:

```c
// PPC32 has 32 bit pointers. The PS3 (OS Lv2) is a PPC64 machine with 32 bit
// pointers.
if (!is64Bit || T.getOS() == Triple::Lv2)
  Ret += "-p:32:32";
```

`Triple::Lv2` is the PS3. Cell's PPE and Xenon are the same generation of 64-bit
PowerPC, and Sony and Microsoft made the same choice: 64-bit registers, 32-bit
addresses. So the data model we need is already implemented and in tree.

The second half is that the 64-bit argument lowering keeps the *slot* size separate
from the *pointer* size — `PPCISelLowering.cpp`, `LowerFormalArguments_64SVR4`:

```c
EVT PtrVT = getPointerTy(MF.getDataLayout());          // 32-bit under a Lv2-style layout
unsigned PtrByteSize = 8;                               // hardcoded: the slot size
unsigned ParamAreaSize = Num_GPR_Regs * PtrByteSize;    // 8 * 8 = 0x40
```

`ParamAreaSize` is exactly the 0x40 homing area we measured, and
`MinReservedArea = std::max(ArgOffset, LinkageSize + 8 * PtrByteSize)` (line ~4782) is
exactly the reservation rule MS callees depend on.

So this is **not** "implement a calling convention". It is "start from PPC64 ELFv2
lowering with a Lv2-style 32-bit-pointer data layout, and override five things".

## What we inherit for free

| MS-PPC property | already provided by the 64-bit path |
|---|---|
| 64-bit GPRs, `std`/`ld` callee saves | yes (A3 solved) |
| 8-byte parameter slots | yes, `PtrByteSize = 8` |
| `long long` in one GPR | yes |
| 32-bit pointers | yes, via the Lv2 data-layout branch |
| caller-allocated parameter save area | yes, `MinReservedArea` (A1 solved) |
| positional GPR/FPR slot consumption | yes, the 64SVR4 rule |
| structs by value in consecutive GPRs | yes |
| big-endian | yes |
| 16-byte stack alignment | yes |
| callee-saved r14-r31 / f14-f31 | yes, matches |

## What we must override

1. **Linkage size.** `computeLinkageSize` gives `6*8 = 0x30` for PPC64 non-ELFv2 and
   `4*8 = 0x20` for ELFv2. We measured **0x10**. One branch in that function.

2. **Vector argument registers.** `LowerFormalArguments_64SVR4` / `LowerCall_64SVR4`
   use `VR[] = {V2..V13}`. MS uses **`vr1`..`vr13`**. Add a V1-based array for this ABI.

3. **No callee-saved vector registers.** `CSR_Altivec` is `V20..V31`
   (`PPCCallingConv.td:294`). MS's callee-saved vectors are `vr108`-`vr127`, which LLVM
   cannot name at all, so from LLVM's point of view this ABI has **none** — v20-v31 must
   become volatile. An empty vector CSR list. (A2 solved.)

4. **Link register save slot.** MS stores LR at `(incoming r1) - 8`; the ELF
   convention is `+16(r1)`. `computeReturnSaveOffset` in `PPCFrameLowering.cpp`.

5. **Parameter area always present.** ELFv1 always guarantees it, ELFv2 only for
   varargs. MS always does, so `HasParameterArea = true` unconditionally.

## Resolved while implementing

- **Varargs floating-point needs no change.** The 64-bit path already passes unnamed FP
  arguments in *both* an FPR and a GPR (`PPCISelLowering.cpp`, "always in both locations
  (FPR *and* GPR or stack slot)"). Re-reading the measurement, MS does the same: at the
  call `fr1` still held 3.0 while `r6` held its raw bits. The two agree.
- **No TOC, no function descriptors.** Handled by reporting the ABI as ELFv2 from
  `computeTargetABI`, which only affects `usesFunctionDescriptors()`; the linkage size
  and parameter-area rules are overridden separately.
- **r2 is reserved.** Zero occurrences across 56k lines of disassembled `xapilib.lib`
  (against 8112 for r3 and 15635 for r11), so it is marked system-reserved rather than
  left allocatable. r13 was already reserved via the SVR4 path.

## Status

Branch `xbox` (merged through PRs #2-#4), six commits building on the design above:

| commit | contents |
|---|---|
| `9165ac8` | `Triple::Xbox360` + `"xbox360"` name, Lv2-style 32-bit-pointer data layout, `isXbox360ABI()`, linkage area 0x10 |
| `ff9ed46` | vector arg registers from v1, no callee-saved vectors (v20-v31 volatile), LR at -8, parameter area always present |
| `8557024` | no function descriptors, r2 reserved |
| `dfd12ff` | clang `powerpc-unknown-xbox360` target info |
| `7e67c1c` | do not infer register width from pointer width (ILP32 with 64-bit GPRs) |
| `2600644` | the 32-bit-base Xbox 360 argument convention |

**Built and verified.** `tools/build-llvm.bat` builds clang + lld + llc for the
PowerPC target only (`build/llvm/bin/clang.exe`). `tools/verify_abi.py` compiles
the `spike/` probes with both this clang (`--target=powerpc-unknown-xbox360`) and
the XDK `cl.exe` and compares, rule by rule:

```
stack parameter slots      OK   cl=[84,92,100,108]  clang=[84,92,100,108]
mixed int/float registers  OK   ...
vector argument registers  OK   cl=[1,2]  clang=[1,2]
return address slot        OK   cl=[-8]  clang=[-8]
4 agree, 0 differ, 0 skipped
```

**Wired into the toolchain.** `mktitle.py --cc clang` compiles a title with this
compiler (the `_start` frame reserves the 0x50 homing area, where zig's EABI
reserves 8), links it against the translated MS libraries and packs it. Both a
freestanding title (writes `.data`/`.bss`) and one linking `xapilib.a` + the
`libcMT.a` CRT build and run in xenia -- MS-ABI title code calling MS-ABI
library code with matching conventions.

## Target triple

Needs a new `Triple::OSType`. Proposed `powerpc64-unknown-xbox360`, matching how `Lv2`
is modelled, with `isXbox360ABI()` on `PPCSubtarget` alongside `isAIXABI()`.

Designing it as a real target rather than local hacks is what makes eventual upstreaming
possible - and if it lands, every future clang (including the one zig bundles) carries
it with no fork to maintain.
