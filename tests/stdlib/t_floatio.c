/*
 * 2026 - Team Resurgent
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Part of RXDK - see LICENSE.md for the full GNU GPL v3.
 */

/* Exact float <-> decimal conversion in printf and strtod. printf rounds the
   exact binary value to the requested digits however many that is (picolibc's
   Ryu engine up to 17 significant digits, dtoa_big.c past that), and strtod
   rounds decimal input to the nearest double. The approximate engines the
   runtime used to build printed only ~17 significant digits and did not round
   decimal input correctly, which broke SDL3's stdlib tests (snprintf "%.0f",
   strtod). Shared by both consoles; floatio_cases.h comes from
   gen_floatio_cases.py. */
#include "rxdk_test.h"
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "floatio_cases.h"

static void check_print(const char *fmt, double v, const char *want, const char *desc)
{
    char buf[128];
    snprintf(buf, sizeof buf, fmt, v);
    CHECK_STR(buf, want, desc);
}

static void check_parse(const char *text, double want, const char *desc)
{
    char *end = NULL;
    double got = strtod(text, &end);
    CHECK(got == want && *end == '\0', desc);
}

static void check_roundtrip(double v, const char *desc)
{
    char buf[64];
    snprintf(buf, sizeof buf, "%.17g", v);
    CHECK(strtod(buf, NULL) == v, desc);
}

static double from_bits(unsigned long long bits)
{
    double d;
    memcpy(&d, &bits, sizeof d);
    return d;
}

/* Every generated case; reports up to 32 mismatches, then one result. */
static void check_cases(void)
{
    char buf[192];
    int bad = 0;
    size_t i;

    for (i = 0; i < sizeof floatio_cases / sizeof floatio_cases[0]; i++) {
        const struct floatio_case *c = &floatio_cases[i];
        snprintf(buf, sizeof buf, c->fmt, from_bits(c->bits));
        if (strcmp(buf, c->want) != 0) {
            if (++bad <= 32)
                DbgPrint("[T] note case %d: %s got '%s' want '%s'\n", (int)i, c->fmt, buf, c->want);
        }
    }
    CHECK_EQI(bad, 0, "generated e/f/g cases print exactly (mismatch count)");
}

/* The longest conversions: every digit of the smallest denormal and of DBL_MAX. */
static void check_longest(void)
{
    static char buf[1200];
    int n;

    n = snprintf(buf, sizeof buf, "%.1074f", 4.9406564584124654e-324);
    CHECK_EQI(n, 1076, "%.1074f of the smallest denormal is 1076 characters");
    CHECK(strncmp(buf, "0.000", 5) == 0 && strcmp(buf + n - 20, "19718265533447265625") == 0,
          "%.1074f of the smallest denormal ends in its exact last digits");

    n = snprintf(buf, sizeof buf, "%.766e", 4.9406564584124654e-324);
    CHECK_EQI(n, 773, "%.766e of the smallest denormal is 773 characters");
    CHECK(strncmp(buf, "4.940656458412465441", 20) == 0
              && strcmp(buf + n - 30, "4472656250000000000000000e-324") == 0,
          "%.766e of the smallest denormal is exact");

    n = snprintf(buf, sizeof buf, "%.0f", DBL_MAX);
    CHECK_EQI(n, 309, "%.0f of DBL_MAX is 309 digits");
    CHECK(strncmp(buf, "17976931348623157081", 20) == 0
              && strcmp(buf + n - 20, "50404026184124858368") == 0,
          "%.0f of DBL_MAX is exact");
}

int main(void)
{
    /* Integers past 17 digits print exactly. */
    check_print("%.0f", 13835058055282163712.0, "13835058055282163712", "%.0f of 1.5 * 2^63 is exact");
    check_print("%06.0f", 9007199254740993.0, "9007199254740992", "%.0f of 2^53 + 1 (rounds to 2^53)");
    check_print("%.0f", 1e22, "10000000000000000000000", "%.0f of 1e22");
    check_print("%.0f", 1e23, "99999999999999991611392", "%.0f of 1e23 (its double is below 1e23)");

    /* Fractions print their exact binary value. */
    check_print("%.20f", 0.1, "0.10000000000000000555", "%.20f of 0.1 shows its binary value");
    check_print("%.3f", 999.555, "999.555", "%.3f of 999.555");
    check_print("%.2f", 2.675, "2.67", "%.2f of 2.675 (binary value is below .675)");
    check_print("%.30f", 1.0 / 3.0, "0.333333333333333314829616256247", "%.30f of 1/3");
    check_print("%.25e", 0.1, "1.0000000000000000555111512e-01", "%.25e of 0.1");
    check_print("%.21g", 0.1, "0.100000000000000005551", "%.21g of 0.1 (DECIMAL_DIG of a 80-bit long double)");
    check_print("%.40g", 0.5, "0.5", "%.40g drops trailing zeros");
    check_print("%#.20g", 0.5, "0.50000000000000000000", "%#.20g keeps trailing zeros");

    /* Rounding past 17 digits is to nearest, ties to even, on the exact value. */
    check_print("%.0f", 0.5, "0", "%.0f of 0.5 rounds to even");
    check_print("%.0f", 2.5, "2", "%.0f of 2.5 rounds to even");
    check_print("%.0f", 0x1.fffffffffffffp+51, "4503599627370496", "%.0f of 2^52 - 0.5 rounds to even");
    check_print("%.51f", 0x1.0000000000001p+0, "1.000000000000000222044604925031308084726333618164062",
                "%.51f of 1 + 2^-52 rounds its final ...0625 tie to even");
    check_print("%.50f", 0x1.0000000000001p+0, "1.00000000000000022204460492503130808472633361816406",
                "%.50f of 1 + 2^-52 rounds down");
    check_print("%.18f", 0.125, "0.125000000000000000", "%.18f of 0.125 is exact");
    check_print("%.17f", 0x1.3ffffffffffffp+3, "9.99999999999999822", "%.17f of the double below 10 (18 digits)");
    check_print("%.16f", 0x1.3ffffffffffffp+3, "9.9999999999999982", "%.16f of the double below 10");
    check_print("%.14f", 0x1.3ffffffffffffp+3, "10.00000000000000", "%.14f of the double below 10 carries");
    check_print("%.18e", 0x1.3ffffffffffffp+3, "9.999999999999998224e+00", "%.18e of the double below 10");
    check_print("%.20f", 1e-30, "0.00000000000000000000", "%.20f of 1e-30 rounds to zero");
    check_print("%.30f", 6e-30, "0.000000000000000000000000000006", "%.30f of 6e-30 rounds up into the last place");
    check_print("%.30Lf", (long double)(1.0 / 3.0), "0.333333333333333314829616256247", "%.30Lf matches %.30f");

    check_cases();
    check_longest();

    /* Decimal input rounds correctly to the nearest double. */
    check_parse("999.555", 999.555, "strtod 999.555");
    check_parse("-999.555", -999.555, "strtod -999.555");
    check_parse("0.1", 0.1, "strtod 0.1");
    check_parse("2.2250738585072011e-308", 2.2250738585072011e-308, "strtod near DBL_MIN (classic hard case)");
    check_parse("1.7976931348623157e308", DBL_MAX, "strtod DBL_MAX");
    check_parse("4.9406564584124654e-324", 4.9406564584124654e-324, "strtod smallest denormal");
    check_parse("9007199254740993", 9007199254740992.0, "strtod 2^53 + 1 rounds to even");

    /* Shortest-digits output reads back to the same double. */
    check_roundtrip(1.0 / 3.0, "1/3 round-trips through %.17g");
    check_roundtrip(DBL_MAX, "DBL_MAX round-trips");
    check_roundtrip(4.9406564584124654e-324, "smallest denormal round-trips");
    check_roundtrip(0.30000000000000004, "0.1 + 0.2 round-trips");

    check_print("%g", 1e-5, "1e-05", "%g of 1e-5");
    check_print("%e", 12345.678, "1.234568e+04", "%e of 12345.678");

    CHECK_DONE("floatio");
    return 0;
}
