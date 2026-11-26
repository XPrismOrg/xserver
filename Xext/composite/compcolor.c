/*
 * SPDX-License-Identifier: MIT
 *
 * Colour spaces and conversions for the built-in compositor; see
 * compcolor.h and doc/color.md.
 */
#include <dix-config.h>

#include <math.h>
#include <string.h>

#include "compcolor.h"

#define D65 { 0.3127, 0.3290 }

const XpColorPrimaries xpColorPrimariesSRGB = {
    { 0.640, 0.330 }, { 0.300, 0.600 }, { 0.150, 0.060 }, D65,
};
const XpColorPrimaries xpColorPrimariesDisplayP3 = {
    { 0.680, 0.320 }, { 0.265, 0.690 }, { 0.150, 0.060 }, D65,
};
const XpColorPrimaries xpColorPrimariesBT2020 = {
    { 0.708, 0.292 }, { 0.170, 0.797 }, { 0.131, 0.046 }, D65,
};

const XpColorSpace xpColorSpaceSRGB = {
    .primaries = { { 0.640, 0.330 }, { 0.300, 0.600 }, { 0.150, 0.060 }, D65 },
    .transfer = XpColorTransferSRGB, .refWhite = 80, .minLum = 0.2, .maxLum = 80,
};
const XpColorSpace xpColorSpaceDisplayP3 = {
    .primaries = { { 0.680, 0.320 }, { 0.265, 0.690 }, { 0.150, 0.060 }, D65 },
    .transfer = XpColorTransferSRGB, .refWhite = 80, .minLum = 0.2, .maxLum = 80,
};
const XpColorSpace xpColorSpaceBT2100PQ = {
    .primaries = { { 0.708, 0.292 }, { 0.170, 0.797 }, { 0.131, 0.046 }, D65 },
    .transfer = XpColorTransferPQ, .refWhite = 203, .minLum = 0.005, .maxLum = 10000,
};
const XpColorSpace xpColorSpaceBT2100HLG = {
    .primaries = { { 0.708, 0.292 }, { 0.170, 0.797 }, { 0.131, 0.046 }, D65 },
    .transfer = XpColorTransferHLG, .refWhite = 203, .minLum = 0.005, .maxLum = 1000,
};

/* ---- matrices ----------------------------------------------------------- */

void
xpColorMatrixMultiply(const XpColorMatrix a, const XpColorMatrix b, XpColorMatrix out)
{
    XpColorMatrix r;

    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            r[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
    memcpy(out, r, sizeof(r));
}

bool
xpColorMatrixInvert(const XpColorMatrix m, XpColorMatrix out)
{
    double c00 = m[1][1] * m[2][2] - m[1][2] * m[2][1];
    double c01 = m[1][2] * m[2][0] - m[1][0] * m[2][2];
    double c02 = m[1][0] * m[2][1] - m[1][1] * m[2][0];
    double det = m[0][0] * c00 + m[0][1] * c01 + m[0][2] * c02;
    XpColorMatrix r;

    if (fabs(det) < 1e-12)
        return false;
    r[0][0] = c00 / det;
    r[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / det;
    r[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det;
    r[1][0] = c01 / det;
    r[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det;
    r[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det;
    r[2][0] = c02 / det;
    r[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det;
    r[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det;
    memcpy(out, r, sizeof(r));
    return true;
}

void
xpColorMatrixApply(const XpColorMatrix m, const double in[3], double out[3])
{
    double r[3];

    for (int i = 0; i < 3; i++)
        r[i] = m[i][0] * in[0] + m[i][1] * in[1] + m[i][2] * in[2];
    memcpy(out, r, sizeof(r));
}

static void
xyToXYZ(XpColorXY c, double out[3])
{
    out[0] = c.x / c.y;
    out[1] = 1.0;
    out[2] = (1.0 - c.x - c.y) / c.y;
}

bool
xpColorRGBToXYZ(const XpColorPrimaries *p, XpColorMatrix m)
{
    double r[3], g[3], b[3], w[3], s[3];
    XpColorMatrix prim, inv;

    if (p->r.y <= 0 || p->g.y <= 0 || p->b.y <= 0 || p->white.y <= 0)
        return false;
    xyToXYZ(p->r, r);
    xyToXYZ(p->g, g);
    xyToXYZ(p->b, b);
    xyToXYZ(p->white, w);
    for (int i = 0; i < 3; i++) {
        prim[i][0] = r[i];
        prim[i][1] = g[i];
        prim[i][2] = b[i];
    }
    /* scale the primaries so that RGB 1,1,1 is the white point */
    if (!xpColorMatrixInvert(prim, inv))
        return false;
    xpColorMatrixApply(inv, w, s);
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            m[i][j] = prim[i][j] * s[j];
    return true;
}

static const XpColorMatrix bradford = {
    { 0.8951, 0.2664, -0.1614 },
    { -0.7502, 1.7135, 0.0367 },
    { 0.0389, -0.0685, 1.0296 },
};

bool
xpColorConversion(const XpColorPrimaries *src, const XpColorPrimaries *dst,
                  XpColorMatrix m)
{
    XpColorMatrix toXYZ, fromXYZ, dstToXYZ;

    if (!xpColorRGBToXYZ(src, toXYZ) || !xpColorRGBToXYZ(dst, dstToXYZ) ||
        !xpColorMatrixInvert(dstToXYZ, fromXYZ))
        return false;

    if (src->white.x != dst->white.x || src->white.y != dst->white.y) {
        double ws[3], wd[3], cs[3], cd[3];
        XpColorMatrix scale = { { 0 } }, invBradford, adapt;

        xyToXYZ(src->white, ws);
        xyToXYZ(dst->white, wd);
        xpColorMatrixApply(bradford, ws, cs);
        xpColorMatrixApply(bradford, wd, cd);
        for (int i = 0; i < 3; i++)
            scale[i][i] = cd[i] / cs[i];
        if (!xpColorMatrixInvert(bradford, invBradford))
            return false;
        xpColorMatrixMultiply(scale, bradford, adapt);
        xpColorMatrixMultiply(invBradford, adapt, adapt);
        xpColorMatrixMultiply(adapt, toXYZ, toXYZ);
    }
    xpColorMatrixMultiply(fromXYZ, toXYZ, m);
    return true;
}

/* ---- transfer functions ------------------------------------------------ */

#define PQ_M1 (2610.0 / 16384)
#define PQ_M2 (2523.0 / 4096 * 128)
#define PQ_C1 (3424.0 / 4096)
#define PQ_C2 (2413.0 / 4096 * 32)
#define PQ_C3 (2392.0 / 4096 * 32)

double
xpColorPQToNits(double v)
{
    double p, num;

    if (v <= 0)
        return 0;
    if (v > 1)
        v = 1;
    p = pow(v, 1.0 / PQ_M2);
    num = fmax(p - PQ_C1, 0);
    return 10000 * pow(num / (PQ_C2 - PQ_C3 * p), 1.0 / PQ_M1);
}

double
xpColorNitsToPQ(double nits)
{
    double y, p;

    if (nits <= 0)
        return 0;
    y = fmin(nits / 10000, 1);
    p = pow(y, PQ_M1);
    return pow((PQ_C1 + PQ_C2 * p) / (1 + PQ_C3 * p), PQ_M2);
}

#define HLG_A 0.17883277
#define HLG_B (1 - 4 * HLG_A)
#define HLG_C (0.5 - HLG_A * log(4 * HLG_A))

/* HLG inverse OETF: signal to normalised scene light in [0, 1] */
static double
hlgToScene(double v)
{
    if (v <= 0)
        return 0;
    if (v <= 0.5)
        return v * v / 3;
    return (exp((fmin(v, 1) - HLG_C) / HLG_A) + HLG_B) / 12;
}

static double
hlgFromScene(double e)
{
    if (e <= 0)
        return 0;
    if (e <= 1.0 / 12)
        return sqrt(3 * e);
    return fmin(HLG_A * log(12 * e - HLG_B) + HLG_C, 1);
}

/* BT.2100 system gamma for a display of peak luminance lw */
static double
hlgGamma(double lw)
{
    return 1.2 + 0.42 * log10(lw / 1000);
}

/* luminance weights of the space's primaries */
static void
lumaWeights(const XpColorSpace *cs, double w[3])
{
    XpColorMatrix m;

    if (!xpColorRGBToXYZ(&cs->primaries, m)) {
        w[0] = 0.2627; w[1] = 0.6780; w[2] = 0.0593;    /* BT.2020 */
        return;
    }
    for (int i = 0; i < 3; i++)
        w[i] = m[1][i];
}

static double
srgbDecode(double v)
{
    if (v <= 0.04045)
        return v / 12.92;
    return pow((v + 0.055) / 1.055, 2.4);
}

static double
srgbEncode(double l)
{
    if (l <= 0.0031308)
        return l * 12.92;
    return 1.055 * pow(l, 1 / 2.4) - 0.055;
}

static double
clamp01(double v)
{
    return v < 0 ? 0 : v > 1 ? 1 : v;
}

void
xpColorDecodeRGB(const XpColorSpace *cs, const double in[3], double out[3])
{
    if (cs->transfer == XpColorTransferHLG) {
        double e[3], w[3], lw = cs->maxLum, g = hlgGamma(lw);

        lumaWeights(cs, w);
        for (int i = 0; i < 3; i++)
            e[i] = hlgToScene(clamp01(in[i]));
        double ys = w[0] * e[0] + w[1] * e[1] + w[2] * e[2];
        double k = ys > 0 ? lw * pow(ys, g - 1) : 0;
        for (int i = 0; i < 3; i++)
            out[i] = k * e[i] / cs->refWhite;
        return;
    }
    for (int i = 0; i < 3; i++)
        out[i] = xpColorDecode(cs, in[i]);
}

void
xpColorEncodeRGB(const XpColorSpace *cs, const double in[3], double out[3])
{
    if (cs->transfer == XpColorTransferHLG) {
        double w[3], lw = cs->maxLum, g = hlgGamma(lw), f[3];

        lumaWeights(cs, w);
        for (int i = 0; i < 3; i++)
            f[i] = fmax(in[i], 0) * cs->refWhite;           /* cd/m^2 */
        double yd = w[0] * f[0] + w[1] * f[1] + w[2] * f[2];
        /* invert Fd = lw * Ys^(g - 1) * E, with Yd = lw * Ys^g */
        double ys = yd > 0 ? pow(fmin(yd / lw, 1), 1 / g) : 0;
        for (int i = 0; i < 3; i++) {
            double e = ys > 0 ? f[i] / (lw * pow(ys, g - 1)) : 0;
            out[i] = hlgFromScene(e);     /* clamps the signal */
        }
        return;
    }
    for (int i = 0; i < 3; i++)
        out[i] = xpColorEncode(cs, in[i]);
}

double
xpColorDecode(const XpColorSpace *cs, double v)
{
    switch (cs->transfer) {
    case XpColorTransferSRGB:
        return srgbDecode(clamp01(v));
    case XpColorTransferGamma22:
        return pow(clamp01(v), 2.2);
    case XpColorTransferLinear:
        return clamp01(v);
    case XpColorTransferPQ:
        return xpColorPQToNits(v) / cs->refWhite;
    case XpColorTransferHLG: {
        double in[3] = { v, v, v }, out[3];
        xpColorDecodeRGB(cs, in, out);
        return out[0];
    }
    }
    return v;
}

double
xpColorEncode(const XpColorSpace *cs, double l)
{
    switch (cs->transfer) {
    case XpColorTransferSRGB:
        return srgbEncode(clamp01(l));
    case XpColorTransferGamma22:
        return pow(clamp01(l), 1 / 2.2);
    case XpColorTransferLinear:
        return clamp01(l);
    case XpColorTransferPQ:
        return xpColorNitsToPQ(l * cs->refWhite);
    case XpColorTransferHLG: {
        double in[3] = { l, l, l }, out[3];
        xpColorEncodeRGB(cs, in, out);
        return out[0];
    }
    }
    return l;
}

/* ---- tone mapping ------------------------------------------------------- */

double
xpColorToneMap(double nits, double srcMax, double dstMax)
{
    if (nits <= 0)
        return 0;
    if (dstMax >= srcMax)
        return nits;
    nits = fmin(nits, srcMax);

    /* BT.2390 EETF in the PQ domain, black level 0 */
    double srcPQ = xpColorNitsToPQ(srcMax);
    double e1 = xpColorNitsToPQ(nits) / srcPQ;
    double maxLum = xpColorNitsToPQ(dstMax) / srcPQ;
    double ks = fmax(1.5 * maxLum - 0.5, 0);
    double e2 = e1;

    if (e1 > ks) {
        double t = (e1 - ks) / (1 - ks), t2 = t * t, t3 = t2 * t;
        e2 = (2 * t3 - 3 * t2 + 1) * ks + (t3 - 2 * t2 + t) * (1 - ks) +
            (-2 * t3 + 3 * t2) * maxLum;
    }
    return xpColorPQToNits(e2 * srcPQ);
}
