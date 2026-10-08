/*
 * 2026 - Team Resurgent
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Part of RXDK - see LICENSE.md for the full GNU GPL v3.
 */

/* Bit-field layout. Both consoles allocate bit-fields least-significant-bit
   first within their storage unit, like the platform (MS) compilers -- on the
   big-endian 360 too, so structs shared with the XDK libraries and the GPU
   (e.g. GPUTEXTURE_FETCH_CONSTANT) match. A statically initialized struct must
   come out byte-identical to the same values assigned at run time: the 360
   compiler once emitted constant initializers in a different bit order from
   the one its loads and stores use. Shared by both consoles. */
#include "rxdk_test.h"
#include <stdint.h>
#include <string.h>

/* One flag bit beside a wide field in a 32-bit unit (libc++'s string rep). */
struct flag_word {
    unsigned flag : 1;
    unsigned rest : 31;
};

/* The shape of the GPU texture fetch constant's first word. */
struct fetch_word {
    unsigned DataFormat : 6;
    unsigned Endian : 2;
    unsigned RequestSize : 2;
    unsigned Stacked : 1;
    unsigned ClampPolicy : 1;
    unsigned BaseAddress : 20;
};

/* Several storage units of different widths, after a plain member. */
struct mixed {
    uint16_t       tag;
    unsigned char  a : 3;
    unsigned char  b : 5;
    unsigned short c : 9;
    unsigned short d : 7;
    unsigned int   e : 1;
    unsigned int   f : 31;
    int            g : 5; /* signed */
};

/* A 64-bit storage unit. */
struct wide {
    unsigned long long lo : 40;
    unsigned long long hi : 24;
};

union flag_union {
    struct flag_word w;
    uint32_t         raw;
};

static const struct flag_word s_flag = { 1, 0x12345 };
static const struct fetch_word s_fetch = { 3, 0, 0, 0, 0, 0xABCDE };
static const struct mixed s_mixed = { 0xBEEF, 5, 17, 300, 99, 1, 0x7ABCDEF1, -3 };
static const struct wide s_wide = { 0x123456789AULL, 0xFEDCBAULL };
static const union flag_union s_union = { { 1, 0x7FFFFFFF } };

/* Keep the compiler from folding the runtime copies back into constants. */
static volatile unsigned v_one = 1;

int main(void)
{
    struct flag_word  r_flag;
    struct fetch_word r_fetch;
    struct mixed      r_mixed;
    struct wide       r_wide;
    uint32_t          word;

    memset(&r_flag, 0, sizeof r_flag);
    r_flag.flag = v_one;
    r_flag.rest = 0x12345 * v_one;
    CHECK(memcmp(&s_flag, &r_flag, sizeof r_flag) == 0, "flag_word: static == runtime bytes");
    CHECK_EQI(s_flag.flag, 1, "flag_word: static flag reads back");
    CHECK_EQI(s_flag.rest, 0x12345, "flag_word: static rest reads back");
    memcpy(&word, &s_flag, sizeof word);
    CHECK_EQI((long)word, (long)((0x12345u << 1) | 1u), "flag_word: flag is bit 0 of the unit");

    memset(&r_fetch, 0, sizeof r_fetch);
    r_fetch.DataFormat = 3 * v_one;
    r_fetch.BaseAddress = 0xABCDE * v_one;
    CHECK(memcmp(&s_fetch, &r_fetch, sizeof r_fetch) == 0, "fetch_word: static == runtime bytes");
    memcpy(&word, &s_fetch, sizeof word);
    CHECK_EQI((long)word, (long)0xABCDE003u, "fetch_word: unit is 0xABCDE003 (platform compiler/GPU layout)");
    CHECK_EQI(s_fetch.BaseAddress, 0xABCDE, "fetch_word: static BaseAddress reads back");

    memset(&r_mixed, 0, sizeof r_mixed);
    r_mixed.tag = 0xBEEF;
    r_mixed.a = 5 * v_one;
    r_mixed.b = 17 * v_one;
    r_mixed.c = 300 * v_one;
    r_mixed.d = 99 * v_one;
    r_mixed.e = v_one;
    r_mixed.f = 0x7ABCDEF1 * v_one;
    r_mixed.g = -3 * (int)v_one;
    CHECK(memcmp(&s_mixed, &r_mixed, sizeof r_mixed) == 0, "mixed units: static == runtime bytes");
    CHECK(s_mixed.a == 5 && s_mixed.b == 17 && s_mixed.c == 300 && s_mixed.d == 99,
          "mixed units: 8/16-bit fields read back");
    CHECK(s_mixed.e == 1 && s_mixed.f == 0x7ABCDEF1, "mixed units: 32-bit fields read back");
    CHECK_EQI(s_mixed.g, -3, "mixed units: signed field reads back negative");

    memset(&r_wide, 0, sizeof r_wide);
    r_wide.lo = 0x123456789AULL * v_one;
    r_wide.hi = 0xFEDCBAULL * v_one;
    CHECK(memcmp(&s_wide, &r_wide, sizeof r_wide) == 0, "64-bit unit: static == runtime bytes");
    CHECK(s_wide.lo == 0x123456789AULL && s_wide.hi == 0xFEDCBAULL, "64-bit unit: fields read back");

    CHECK_EQI((long)s_union.raw, (long)0xFFFFFFFFu, "union: bit-field initializer fills the word");
    CHECK(s_union.w.flag == 1 && s_union.w.rest == 0x7FFFFFFF, "union: fields read back");

    CHECK_DONE("bitfield");
    return 0;
}
