# Correction: the 64-bit base does not work, and why

An earlier conclusion in [llvm-patch-design.md](llvm-patch-design.md) was wrong and is
recorded here rather than quietly edited, because the reasoning matters.

## What was claimed

That the platform ABI - 64-bit PowerPC parameter passing with a 32-bit data model -
already existed in LLVM via `Triple::Lv2`, the PS3, whose Cell PPE is the same
generation of the architecture. `computePowerDataLayout` does contain:

```c
// PPC32 has 32 bit pointers. The PS3 (OS Lv2) is a PPC64 machine with 32 bit
// pointers.
if (!is64Bit || T.getOS() == Triple::Lv2)
  Ret += "-p:32:32";
```

and the 64-bit argument lowering does keep `PtrByteSize = 8` independent of the pointer
size. From reading the source, this looked like the whole data model was in place.

## What building it showed

The data layout is in place. The code generation is not.

```
$ clang --target=powerpc64-unknown-lv2 -O2 -S -c 'int add(int a,int b){return a+b;}'
Assertion failed: getPointerTy(...).getSizeInBits() == 64
                  && "Only expecting to use this on 64 bit targets."
                  PPCISelDAGToDAG.cpp:2934
```

The in-tree PS3 target does not compile a function taking two integers. It is a
declared configuration that was never made to work end to end.

Five sites inferred *register* width from *pointer* width
(`bool isPPC64 = (PtrVT == MVT::i64)`). Fixing those is correct on its own merits and
is committed upstream-style on the branch; `powerpc64-unknown-lv2` now compiles that
function where it previously asserted.

But the next failure is structural:

```
$ clang --target=powerpc64-unknown-xbox360 -O2 -S -c 'int f(int*p){return *p;}'
Impossible reg-to-reg copy
UNREACHABLE at PPCInstrInfo.cpp:1905
```

A copy between the 32-bit (`GPRC`) and 64-bit (`G8RC`) register classes. The PPC64
backend models pointers as living in 64-bit registers. With 32-bit pointers, pointer
values land in `GPRC` while every addressing mode expects `G8RC`. Making that work is
the equivalent of x86's x32 ABI - months of backend and pattern work, not a set of
overrides.

**Lesson: reading the source showed the data layout existed and stopped there. Only
building it showed that nothing downstream honoured it.**

## The base that does work

`powerpc-unknown-xbox360` - the 32-bit architecture - handles everything tested:

```
OK : int f(int*p){return *p;}
OK : extern int h(int); int f(void){return h(3);}
OK : long long f(long long a,long long b){return a+b;}
OK : double f(double a,int b){return a*b;}
target datalayout = "E-m:e-p:32:32-Fn32-i64:64-n32"
```

Correct data model, and it compiles.

## What that costs

Properties the 64-bit path would have given free must now be overridden in the 32-bit
lowering, or absorbed by thunks:

| property | on the 32-bit base |
|---|---|
| 8-byte parameter slots, area base r1+0x10 | **override** in `Lower{FormalArguments,Call}_32SVR4` |
| parameter save area always reserved | **override** |
| positional GPR/FPR slot consumption | **override** (EABI uses independent counters) |
| structs by value in GPRs | **override** (EABI passes a pointer) |
| vector arguments in vr1-vr13 | **override** |
| v20-v31 volatile | **override** (already done, applies to both bases) |
| LR at -8 from incoming r1 | **override** (already done) |
| `long long` in one GPR | **thunk** - a register pair instead; 31 public prototypes |
| callee-saved GPRs saved 64 bits wide | **thunk** - reverse thunks for the 83 callback typedefs must preserve the full width of r14-r31 |

More override work than the 64-bit path implied, but on a foundation that runs. The two
thunked items were already in the bounded set measured in
[interop-surface.md](interop-surface.md).

## Status

Committed on `xbox`:

| commit | contents |
|---|---|
| `9165ac8` | `Triple::Xbox360`, data layout, `isXbox360ABI()`, linkage area 0x10 |
| `ff9ed46` | vector arg registers, no callee-saved vectors, LR slot, parameter area |
| `8557024` | no function descriptors, r2 reserved |
| `dfd12ff` | clang target info |
| `7e67c1c` | register-width/pointer-width fixes (independently correct; revives Lv2 partially) |

The first three target the 64-bit lowering path and need re-pointing at the 32-bit one.
The last two stand as they are.
