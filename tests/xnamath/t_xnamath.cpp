/*
 * 2026 - Team Resurgent
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Part of RXDK - see LICENSE.md for the full GNU GPL v3.
 *
 * Scalar xnamath (XMVECTOR/XMMATRIX) coverage for the modern clang toolchain.
 *
 * Every RXDK-360 title builds xnamath with _XM_NO_INTRINSICS_ (see the clang
 * Toolset.props / ClangCompile.cs / installer): XMVECTOR is the plain {x,y,z,w}
 * struct and the vector/matrix math is scalar C the compiler lowers directly --
 * this is the exact code path ATG samples build their camera/world matrices on.
 * So this test compiles like a title (xtl.h + the Win32/Xbox define set +
 * _XM_NO_INTRINSICS_, driven by tools/run_xnamath_test.py) rather than through
 * the pure-libc stdlib harness.
 *
 * The load-bearing checks feed a NaN in the .w lane into the 3-component
 * operations (dot/length/normalize/cross) and the LookAt/transform matrix path.
 * ATG samples routinely leave .w uninitialised (e.g. `dir.x = ...; dir.y = ...;`
 * without touching .w), so a 3-component op that wrongly reads .w turns that
 * stack garbage into NaN and scatters every transformed vertex to infinity. If
 * the scalar math correctly restricts itself to x/y/z, a NaN .w is inert; these
 * assertions fail loudly if it is not.
 */
#include <xtl.h>
#include <xnamath.h>
#include <math.h>
#include "rxdk_test.h"

static int finite1(float x) { return (x == x) && (fabsf(x) < 1.0e20f); }
static int near1(float a, float b, float eps) { return finite1(a) && fabsf(a - b) <= eps; }

int main(void)
{
    const float QNAN = nanf("");   /* the uninitialised-.w stand-in */

    /* ---- component get/set roundtrip ----------------------------------- */
    {
        XMVECTOR v = XMVectorSet(1.5f, -2.25f, 3.0f, 4.0f);
        CHECK(near1(XMVectorGetX(v), 1.5f, 0) && near1(XMVectorGetY(v), -2.25f, 0)
           && near1(XMVectorGetZ(v), 3.0f, 0) && near1(XMVectorGetW(v), 4.0f, 0), "XMVectorSet/Get roundtrip");
    }

    /* ---- 3-component ops must ignore a NaN .w --------------------------- */
    {
        XMVECTOR a = XMVectorSet(1, 2, 3, QNAN);
        XMVECTOR b = XMVectorSet(4, 5, 6, QNAN);
        CHECK(near1(XMVectorGetX(XMVector3Dot(a, b)), 32.0f, 1e-4f), "XMVector3Dot ignores .w");

        XMVECTOR c = XMVectorSet(3, 4, 0, QNAN);
        CHECK(near1(XMVectorGetX(XMVector3LengthSq(c)), 25.0f, 1e-4f), "XMVector3LengthSq ignores .w");
        CHECK(near1(XMVectorGetX(XMVector3Length(c)), 5.0f, 1e-3f), "XMVector3Length ignores .w");

        XMVECTOR n = XMVector3Normalize(c);
        CHECK(near1(XMVectorGetX(n), 0.6f, 1e-3f) && near1(XMVectorGetY(n), 0.8f, 1e-3f)
           && near1(XMVectorGetZ(n), 0.0f, 1e-3f), "XMVector3Normalize ignores .w (unit x/y)");

        XMVECTOR x = XMVectorSet(1, 0, 0, QNAN);
        XMVECTOR y = XMVectorSet(0, 1, 0, QNAN);
        XMVECTOR cr = XMVector3Cross(x, y);   /* +x cross +y = +z */
        CHECK(near1(XMVectorGetX(cr), 0.0f, 1e-4f) && near1(XMVectorGetY(cr), 0.0f, 1e-4f)
           && near1(XMVectorGetZ(cr), 1.0f, 1e-4f) && finite1(XMVectorGetW(cr)),
              "XMVector3Cross ignores .w, w finite");
    }

    /* ---- matrix identity / multiply / transpose ------------------------ */
    {
        XMMATRIX I = XMMatrixIdentity();
        XMVECTOR p = XMVector3TransformCoord(XMVectorSet(7, 8, 9, 1), I);
        CHECK(near1(XMVectorGetX(p), 7, 1e-4f) && near1(XMVectorGetY(p), 8, 1e-4f)
           && near1(XMVectorGetZ(p), 9, 1e-4f), "identity transform");

        XMMATRIX T = XMMatrixTranslation(1, 2, 3);
        XMMATRIX TI = XMMatrixMultiply(T, I);            /* A*I == A */
        XMVECTOR q = XMVector3TransformCoord(XMVectorZero(), TI);
        CHECK(near1(XMVectorGetX(q), 1, 1e-4f) && near1(XMVectorGetY(q), 2, 1e-4f)
           && near1(XMVectorGetZ(q), 3, 1e-4f), "translation * identity");

        /* transpose swaps the translation row into the last column */
        XMMATRIX Tt = XMMatrixTranspose(T);
        XMFLOAT4X4 m; XMStoreFloat4x4(&m, Tt);
        CHECK(near1(m._14, 1, 1e-4f) && near1(m._24, 2, 1e-4f) && near1(m._34, 3, 1e-4f), "XMMatrixTranspose");
    }

    /* ---- the camera path: LookAtLH built from vectors with NaN .w ------- *
     * This mirrors AdvancedLighting's camera update (eye/dir/up assembled a  *
     * lane at a time, .w never set). A correct view matrix must be finite    *
     * and place the world origin 5 units down +z; a leaked NaN .w scatters   *
     * every transformed point to NaN.                                        */
    {
        XMVECTOR eye   = XMVectorSet(0, 0, -5, QNAN);
        XMVECTOR focus = XMVectorSet(0, 0,  0, QNAN);
        XMVECTOR up    = XMVectorSet(0, 1,  0, QNAN);
        XMMATRIX view  = XMMatrixLookAtLH(eye, focus, up);

        XMFLOAT4X4 vm; XMStoreFloat4x4(&vm, view);
        int allfinite = 1;
        const float *e = &vm._11;
        for (int i = 0; i < 16; ++i) if (!finite1(e[i])) allfinite = 0;
        CHECK(allfinite, "LookAtLH view matrix all-finite (no NaN .w leak)");

        XMVECTOR origin = XMVector3TransformCoord(XMVectorSet(0, 0, 0, 1), view);
        CHECK(near1(XMVectorGetZ(origin), 5.0f, 1e-3f) && near1(XMVectorGetX(origin), 0.0f, 1e-3f)
           && near1(XMVectorGetY(origin), 0.0f, 1e-3f), "world origin maps to (0,0,5) in view space");

        XMVECTOR eyeview = XMVector3TransformCoord(eye, view);
        CHECK(near1(XMVectorGetX(eyeview), 0, 1e-3f) && near1(XMVectorGetY(eyeview), 0, 1e-3f)
           && near1(XMVectorGetZ(eyeview), 0, 1e-3f), "eye maps to view-space origin");

        /* full WVP-style chain: view * projection, transform a point, stay finite */
        XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV4, 16.0f / 9.0f, 1.0f, 100.0f);
        XMMATRIX wvp  = XMMatrixMultiply(view, proj);
        XMFLOAT4X4 pm; XMStoreFloat4x4(&pm, wvp);
        const float *pe = &pm._11; int projfinite = 1;
        for (int i = 0; i < 16; ++i) if (!finite1(pe[i])) projfinite = 0;
        CHECK(projfinite, "view*proj all-finite");
        XMVECTOR clip = XMVector3TransformCoord(XMVectorSet(1, 1, 10, 1), wvp);
        CHECK(finite1(XMVectorGetX(clip)) && finite1(XMVectorGetY(clip)) && finite1(XMVectorGetZ(clip)),
              "point through WVP stays finite");
    }

    CHECK_DONE("xnamath");
    return 0;
}
