/* Reproducer: swscanf with 16 float outputs (the ATG matrix parse).
 *
 * AdvancedLighting's scene parser calls
 *   swscanf_s(L"%f %f ...(16)", &m._11, ..., &m._44)
 * which routes into picolibc's vswscanf. Floats 7..16 are passed in the
 * variadic overflow area; if vswscanf (in prebuilt libc.a) consumes va_arg
 * with the pre-fix PowerPC lowering, it stores parsed values through wrong
 * pointers. The corpus only ever exercised the 3-float form, so this pins the
 * 16-output case specifically. */
#include "rxdk_test.h"
#include <cwchar>

int main() {
    // Three floats: the case the corpus already covered (register args only).
    {
        float a = 0, b = 0, c = 0;
        int n = swscanf(L"1.5 2.5 3.5", L"%f %f %f", &a, &b, &c);
        CHECK_EQI(n, 3, "swscanf 3 floats count");
        CHECK(a == 1.5f && b == 2.5f && c == 3.5f, "swscanf 3 floats values");
    }

    // Sixteen floats: forces the variadic overflow area (args 7..16).
    {
        float m[16];
        for (int i = 0; i < 16; ++i) m[i] = -1.0f;
        int n = swscanf(
            L"0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15",
            L"%f %f %f %f %f %f %f %f %f %f %f %f %f %f %f %f",
            &m[0], &m[1], &m[2], &m[3], &m[4], &m[5], &m[6], &m[7],
            &m[8], &m[9], &m[10], &m[11], &m[12], &m[13], &m[14], &m[15]);
        CHECK_EQI(n, 16, "swscanf 16 floats count");
        int ok = 1;
        for (int i = 0; i < 16; ++i) if (m[i] != (float)i) { ok = 0; break; }
        CHECK(ok, "swscanf 16 floats values 0..15");
    }

    CHECK_DONE("scanf16");
    return 0;
}
