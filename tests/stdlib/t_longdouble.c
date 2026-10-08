/*
 * 2026 - Team Resurgent
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Part of RXDK - see LICENSE.md for the full GNU GPL v3.
 */

/* long double is the 64-bit double type on both consoles, as with the XDK's
   MSVC compiler: the original Xbox builds everything with -mlong-double-64
   (clang's i686-pc-windows-gnu default is the 80-bit x87 type, which made
   DECIMAL_DIG 21 and gave a title a different long double from the msvc-ABI
   SDK libraries). The long double math, I/O and varargs must agree with
   double. Shared by both consoles. */
#include "rxdk_test.h"
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static long double sum_va(int n, ...)
{
    va_list ap;
    long double s = 0;

    va_start(ap, n);
    while (n--)
        s += va_arg(ap, long double);
    va_end(ap);
    return s;
}

int main(void)
{
    volatile double x = 0.7, y = 2.5;
    volatile long double lx = 0.7L, ly = 2.5L;
    char buf[64];
    long double parsed = 0;
    int exp2 = 0;

    /* The type itself. */
    CHECK_EQI((long)sizeof(long double), 8, "sizeof(long double) is 8");
    CHECK_EQI(LDBL_MANT_DIG, DBL_MANT_DIG, "LDBL_MANT_DIG is DBL_MANT_DIG");
    CHECK_EQI(LDBL_MAX_EXP, DBL_MAX_EXP, "LDBL_MAX_EXP is DBL_MAX_EXP");
    CHECK_EQI(LDBL_MIN_EXP, DBL_MIN_EXP, "LDBL_MIN_EXP is DBL_MIN_EXP");
    CHECK_EQI(LDBL_DIG, DBL_DIG, "LDBL_DIG is DBL_DIG");
    CHECK_EQI(DECIMAL_DIG, 17, "DECIMAL_DIG is 17");
    CHECK(LDBL_MAX == DBL_MAX && LDBL_MIN == DBL_MIN && LDBL_EPSILON == DBL_EPSILON,
          "LDBL_MAX/MIN/EPSILON are the double limits");
    CHECK(lx == x && (long double)(lx * ly) == (long double)(x * y),
          "long double arithmetic rounds like double");

    /* The l math functions are the double ones. */
    CHECK(sinl(lx) == sin(x) && cosl(lx) == cos(x) && tanl(lx) == tan(x), "sinl/cosl/tanl");
    CHECK(expl(lx) == exp(x) && logl(lx) == log(x) && log10l(lx) == log10(x), "expl/logl/log10l");
    CHECK(powl(lx, ly) == pow(x, y) && sqrtl(ly) == sqrt(y) && cbrtl(ly) == cbrt(y), "powl/sqrtl/cbrtl");
    CHECK(atan2l(lx, ly) == atan2(x, y) && fmodl(ly, lx) == fmod(y, x), "atan2l/fmodl");
    CHECK(floorl(ly) == 2.0L && ceill(ly) == 3.0L && roundl(ly) == 3.0L && truncl(-ly) == -2.0L,
          "floorl/ceill/roundl/truncl");
    CHECK(frexpl(ly, &exp2) == 0.625L && exp2 == 2, "frexpl");
    CHECK(ldexpl(0.625L, 2) == 2.5L && scalbnl(0.625L, 2) == 2.5L, "ldexpl/scalbnl");
    CHECK(fabsl(-ly) == 2.5L && copysignl(ly, -1.0L) == -2.5L, "fabsl/copysignl");
    CHECK(nextafterl(1.0L, 2.0L) == nextafter(1.0, 2.0) && nexttowardl(1.0L, 2.0L) == nextafter(1.0, 2.0),
          "nextafterl/nexttowardl step one double ulp");
    CHECK(isnan(nanl("")) && isinf(HUGE_VALL) && fpclassify(LDBL_MIN / 2) == FP_SUBNORMAL,
          "nanl/HUGE_VALL/fpclassify");

    /* I/O and varargs. */
    snprintf(buf, sizeof buf, "%.20Le", (long double)0.1);
    CHECK_STR(buf, "1.00000000000000005551e-01", "%.20Le of 0.1 is the double's exact digits");
    snprintf(buf, sizeof buf, "%Lg %Lg", 1.5L, -0.25L);
    CHECK_STR(buf, "1.5 -0.25", "%Lg arguments are read as 8 bytes");
    CHECK(strtold("0.1", NULL) == 0.1L, "strtold(\"0.1\") is the double 0.1");
    CHECK(sscanf("2.5e-3", "%Lf", &parsed) == 1 && parsed == 2.5e-3L, "sscanf %Lf");
    CHECK(sum_va(3, 1.0L, 2.0L, 0.5L) == 3.5L, "long double varargs");

    CHECK_DONE("longdouble");
    return 0;
}
