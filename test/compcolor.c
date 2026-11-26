/* SPDX-License-Identifier: MIT
 *
 * Tests for the compositor's colour library, Xext/composite/compcolor.c,
 * against published reference values (IEC 61966-2-1, ITU-R BT.2087,
 * SMPTE ST 2084, ITU-R BT.2100 and BT.2408).
 */

/* Test relies on assert() */
#undef NDEBUG

#include <dix-config.h>

#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "Xext/composite/compcolor.h"
#include "tests-common.h"

static void
near(double got, double want, double tol, const char *what)
{
    if (fabs(got - want) > tol) {
        fprintf(stderr, "%s: got %.7f, want %.7f (+-%g)\n", what, got, want, tol);
        assert(fabs(got - want) <= tol);
    }
}

/* chromaticity of an XYZ colour with Y = 1 */
static XpColorXY
xyOfXYZ(double x, double z)
{
    return (XpColorXY) { x / (x + 1 + z), 1 / (x + 1 + z) };
}

static void
matrix_near(const XpColorMatrix got, const XpColorMatrix want, double tol,
            const char *what)
{
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            near(got[i][j], want[i][j], tol, what);
}

static void
compcolor_matrices(void)
{
    XpColorMatrix m, inv, id;

    /* IEC 61966-2-1 (sRGB) RGB to XYZ, as commonly published, derived from
       the white point as XYZ (0.95047, 1, 1.08883): checked exactly with
       that white point, and loosely with the standard's xy */
    static const XpColorMatrix srgbToXYZ = {
        { 0.4124564, 0.3575761, 0.1804375 },
        { 0.2126729, 0.7151522, 0.0721750 },
        { 0.0193339, 0.1191920, 0.9503041 },
    };
    XpColorPrimaries srgb5 = xpColorPrimariesSRGB;
    srgb5.white = xyOfXYZ(0.95047, 1.08883);
    assert(xpColorRGBToXYZ(&srgb5, m));
    matrix_near(m, srgbToXYZ, 1e-6, "sRGB to XYZ");
    assert(xpColorRGBToXYZ(&xpColorPrimariesSRGB, m));
    matrix_near(m, srgbToXYZ, 5e-4, "sRGB to XYZ (4 digits)");

    /* BT.2020 luminance coefficients are the matrix's middle row */
    assert(xpColorRGBToXYZ(&xpColorPrimariesBT2020, m));
    near(m[1][0], 0.2627, 1e-4, "BT.2020 Kr");
    near(m[1][1], 0.6780, 1e-4, "BT.2020 Kg");
    near(m[1][2], 0.0593, 1e-4, "BT.2020 Kb");

    /* ITU-R BT.2087: BT.709 to BT.2020 */
    static const XpColorMatrix bt709To2020 = {
        { 0.6274, 0.3293, 0.0433 },
        { 0.0691, 0.9195, 0.0114 },
        { 0.0164, 0.0880, 0.8956 },
    };
    assert(xpColorConversion(&xpColorPrimariesSRGB, &xpColorPrimariesBT2020, m));
    matrix_near(m, bt709To2020, 1e-4, "BT.709 to BT.2020");

    /* Display P3 to sRGB */
    static const XpColorMatrix p3ToSRGB = {
        { 1.2249401, -0.2249404, 0.0 },
        { -0.0420569, 1.0420571, 0.0 },
        { -0.0196376, -0.0786361, 1.0982735 },
    };
    assert(xpColorConversion(&xpColorPrimariesDisplayP3, &xpColorPrimariesSRGB, m));
    matrix_near(m, p3ToSRGB, 1e-6, "P3 to sRGB");

    /* there and back is the identity */
    static const XpColorMatrix ident = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    assert(xpColorConversion(&xpColorPrimariesBT2020, &xpColorPrimariesSRGB, inv));
    assert(xpColorConversion(&xpColorPrimariesSRGB, &xpColorPrimariesBT2020, m));
    xpColorMatrixMultiply(inv, m, id);
    matrix_near(id, ident, 1e-9, "BT.2020 round trip");
    assert(xpColorMatrixInvert(m, inv));
    xpColorMatrixMultiply(m, inv, id);
    matrix_near(id, ident, 1e-9, "inverse");

    /* a different white point: white stays white (Bradford adaptation) */
    XpColorPrimaries d50 = xpColorPrimariesSRGB;
    d50.white = (XpColorXY) { 0.3457, 0.3585 };
    assert(xpColorConversion(&xpColorPrimariesSRGB, &d50, m));
    double white[3] = { 1, 1, 1 }, out[3];
    xpColorMatrixApply(m, white, out);
    for (int i = 0; i < 3; i++)
        near(out[i], 1, 1e-9, "adapted white");
    /* and the primaries move, unlike without adaptation */
    assert(fabs(m[0][1]) + fabs(m[1][0]) > 1e-3);
    /* sRGB to XYZ under D50 through the Bradford transform, as published
       (ICC profiles' PCS), from whites given as XYZ */
    static const XpColorMatrix srgbToXYZD50 = {
        { 0.4360747, 0.3850649, 0.1430804 },
        { 0.2225045, 0.7168786, 0.0606169 },
        { 0.0139322, 0.0971045, 0.7141733 },
    };
    XpColorMatrix toXYZ;
    XpColorPrimaries d50_5 = srgb5;
    d50_5.white = xyOfXYZ(0.96422, 0.82521);
    assert(xpColorConversion(&srgb5, &d50_5, m));
    assert(xpColorRGBToXYZ(&d50_5, toXYZ));
    xpColorMatrixMultiply(toXYZ, m, id);
    matrix_near(id, srgbToXYZD50, 1e-6, "sRGB to XYZ (D50)");

    /* degenerate primaries are refused */
    XpColorPrimaries bad = xpColorPrimariesSRGB;
    bad.g = bad.r;
    assert(!xpColorRGBToXYZ(&bad, m));
    bad = xpColorPrimariesSRGB;
    bad.white.y = 0;
    assert(!xpColorConversion(&xpColorPrimariesSRGB, &bad, m));
}

static void
compcolor_transfer(void)
{
    const XpColorSpace *srgb = &xpColorSpaceSRGB;
    const XpColorSpace *pq = &xpColorSpaceBT2100PQ;
    XpColorSpace g22 = xpColorSpaceSRGB, lin = xpColorSpaceSRGB;

    g22.transfer = XpColorTransferGamma22;
    lin.transfer = XpColorTransferLinear;

    /* sRGB, IEC 61966-2-1 */
    near(xpColorDecode(srgb, 0), 0, 0, "sRGB 0");
    near(xpColorDecode(srgb, 1), 1, 1e-12, "sRGB 1");
    near(xpColorDecode(srgb, 0.5), 0.2140411, 1e-6, "sRGB 0.5");
    near(xpColorDecode(srgb, 0.04), 0.04 / 12.92, 1e-12, "sRGB linear segment");
    near(xpColorEncode(srgb, 0.0031308), 0.04045, 1e-5, "sRGB breakpoint");
    near(xpColorEncode(srgb, 2), 1, 1e-12, "sRGB clamps");
    near(xpColorDecode(&g22, 0.5), 0.2176376, 1e-6, "gamma 2.2");
    near(xpColorDecode(&lin, 0.3), 0.3, 0, "linear");

    /* SMPTE ST 2084 */
    near(xpColorPQToNits(1), 10000, 1e-6, "PQ 1");
    near(xpColorPQToNits(0), 0, 0, "PQ 0");
    near(xpColorPQToNits(0.5), 92.24570, 1e-3, "PQ 0.5");
    near(xpColorNitsToPQ(100), 0.5080784, 1e-6, "PQ 100 nits");
    near(xpColorNitsToPQ(1000), 0.7518271, 1e-6, "PQ 1000 nits");
    /* BT.2408: HDR reference white, 203 cd/m^2, is PQ 58% */
    near(xpColorNitsToPQ(203), 0.58, 0.001, "PQ reference white");
    /* relative: 1.0 is the space's reference white */
    near(xpColorDecode(pq, xpColorNitsToPQ(203)), 1, 1e-9, "PQ relative");
    near(xpColorEncode(pq, 1000.0 / 203), xpColorNitsToPQ(1000), 1e-9, "PQ encode");

    /* round trips over each curve */
    const XpColorSpace *spaces[] = { srgb, &g22, &lin, pq };
    for (unsigned s = 0; s < sizeof(spaces) / sizeof(spaces[0]); s++)
        for (int i = 0; i <= 100; i++) {
            double v = i / 100.0;
            near(xpColorEncode(spaces[s], xpColorDecode(spaces[s], v)), v, 1e-9,
                 "round trip");
        }

    /* decoding is monotonic */
    for (unsigned s = 0; s < sizeof(spaces) / sizeof(spaces[0]); s++)
        for (int i = 1; i <= 1000; i++)
            assert(xpColorDecode(spaces[s], i / 1000.0) >
                   xpColorDecode(spaces[s], (i - 1) / 1000.0));
}

static void
compcolor_hlg(void)
{
    const XpColorSpace *hlg = &xpColorSpaceBT2100HLG;
    double in[3], out[3], back[3];

    /* BT.2408: HLG reference white, 75%, is 203 cd/m^2 on a 1000 cd/m^2
       display (BT.2100 OOTF, system gamma 1.2) */
    near(xpColorDecode(hlg, 0.75) * hlg->refWhite, 203, 1, "HLG 75%");
    near(xpColorDecode(hlg, 1) * hlg->refWhite, 1000, 1e-3, "HLG peak");    /* a is given to 8 digits */
    near(xpColorDecode(hlg, 0), 0, 0, "HLG black");

    /* the OOTF works on luminance: a saturated colour keeps its hue
       (channel ratios of scene light) */
    in[0] = 0.75; in[1] = 0.5; in[2] = 0.25;
    xpColorDecodeRGB(hlg, in, out);
    assert(out[0] > out[1] && out[1] > out[2] && out[2] > 0);
    xpColorEncodeRGB(hlg, out, back);
    for (int i = 0; i < 3; i++)
        near(back[i], in[i], 1e-9, "HLG RGB round trip");

    for (int i = 0; i <= 100; i++) {
        double v = i / 100.0;
        near(xpColorEncode(hlg, xpColorDecode(hlg, v)), v, 1e-9, "HLG round trip");
    }

    /* a brighter display shows the same signal with a higher gamma */
    XpColorSpace bright = *hlg;
    bright.maxLum = 2000;
    near(xpColorDecode(&bright, 1) * bright.refWhite, 2000, 1e-3, "HLG 2000 peak");
    assert(xpColorDecode(&bright, 0.75) < 2 * xpColorDecode(hlg, 0.75));
}

static void
compcolor_tonemap(void)
{
    /* identity when the display can show everything */
    near(xpColorToneMap(500, 1000, 1000), 500, 0, "no tone mapping");
    near(xpColorToneMap(500, 1000, 4000), 500, 0, "no tone mapping (brighter)");
    near(xpColorToneMap(0, 1000, 100), 0, 0, "black");

    /* the peak lands on the display's peak */
    near(xpColorToneMap(1000, 1000, 400), 400, 1e-6, "peak 1000 to 400");
    near(xpColorToneMap(10000, 10000, 1000), 1000, 1e-6, "peak 10000 to 1000");
    near(xpColorToneMap(20000, 10000, 1000), 1000, 1e-6, "beyond the peak");

    /* below the knee (KS = 1.5 maxLum - 0.5, in PQ) nothing changes */
    double srcPQ = xpColorNitsToPQ(1000);
    double ks = 1.5 * xpColorNitsToPQ(400) / srcPQ - 0.5;
    double knee = xpColorPQToNits(ks * srcPQ);
    near(xpColorToneMapKnee(1000, 400), knee, 1e-9, "knee");
    near(xpColorToneMapKnee(1000, 1000), 1000, 0, "no knee");
    assert(knee > 100 && knee < 400);
    near(xpColorToneMap(100, 1000, 400), 100, 1e-9, "below the knee");
    near(xpColorToneMap(knee * 0.99, 1000, 400), knee * 0.99, 1e-9, "at the knee");

    /* monotonic, never above the display's peak, and continuous at the
       knee */
    double prev = 0;
    for (int n = 1; n <= 1000; n++) {
        double v = xpColorToneMap(n, 1000, 400);
        assert(v > prev);
        assert(v <= 400 + 1e-9);
        assert(v <= n + 1e-9);
        prev = v;
    }
    near(xpColorToneMap(knee * 1.0001, 1000, 400), knee * 1.0001, 0.01, "past the knee");
}

const testfunc_t*
compcolor_test(void)
{
    static const testfunc_t testfuncs[] = {
        compcolor_matrices,
        compcolor_transfer,
        compcolor_hlg,
        compcolor_tonemap,
        NULL,
    };

    return testfuncs;
}
