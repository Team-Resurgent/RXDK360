# Pending stdlib sections

Tests here are for standard features not yet fully brought up. They are kept out
of the globbed suite (tools/run_stdlib_tests.py only scans tests/stdlib/*.c[pp])
so the suite stays green for what works.

- **t_abi64.c** — Xenon 64-bit-argument ABI. A 64-bit integer arg must be passed
  in ONE 64-bit GPR (the Xbox 360 CPU is 64-bit and the translated MSVC libs read
  it that way — e.g. `D3DDevice_SetStreamSource` does `or r11,r11,r8` over a
  64-bit `m_Pending`). Our clang (32-bit-pointer target) splits it across a
  register pair and the callee sees only the high half, dropping the low 32 bits.
  Passes once clang lowers 64-bit args to single 64-bit registers for the
  `powerpc-unknown-xbox360` target. (Root cause of the AdvancedLighting spikes.)

- **t_format.cpp** — `<std::format>` (C++20/23). Needs libc++'s `charconv.cpp`,
  whose float `from_chars` pulls llvm-libc's shared `FPBits.h`; that expects a
  `_LIBCPP_VERBOSE_ABORT` integration this libc++ snapshot doesn't wire up
  cleanly for our out-of-tree build. Integer `to_chars` alone would need
  separating from the float path. Revisit with the llvm-libc shared-header setup.
