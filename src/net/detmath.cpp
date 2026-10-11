// Trigonometry the game simulation can rely on to give the same bits on every
// machine.
//
// The C runtime's sinf/cosf/atan2f are not that: on Windows they come from the
// system's ucrtbase.dll, which picks an FMA3 code path at run time on CPUs that
// have it (and many Pentium and Celeron parts do not), and whose code can change
// between Windows versions. A last-bit difference in a physics rotation is
// enough to desync a network match. These are musl's float kernels -- plain
// IEEE arithmetic, so the same binary computes the same result anywhere.
//
// Derived from musl libc (MIT licence), itself derived from FreeBSD's msun:
//   Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
//   Developed at SunPro, a Sun Microsystems, Inc. business. Permission to use,
//   copy, modify, and distribute this software is freely granted, provided
//   that this notice is preserved.

#include <cmath>
#include <cstdint>
#include <cstring>

namespace {

uint32_t bitsOf(float x) {
    uint32_t u;
    std::memcpy(&u, &x, sizeof(u));
    return u;
}

// |sin(x)| on [-pi/4, pi/4], in double.
float sindf(double x) {
    const double S1 = -0x15555554cbac77.0p-55, S2 = 0x111110896efbb2.0p-59, S3 = -0x1a00f9e2cae774.0p-65,
                 S4 = 0x16cd878c3b46a7.0p-71;
    const double z = x * x, w = z * z, r = S3 + z * S4, s = z * x;
    return float((x + s * (S1 + z * S2)) + s * w * r);
}

float cosdf(double x) {
    const double C0 = -0x1ffffffd0c5e81.0p-54, C1 = 0x155553e1053a42.0p-57, C2 = -0x16c087e80f1e27.0p-62,
                 C3 = 0x199342e0ee5069.0p-68;
    const double z = x * x, w = z * z, r = C2 + z * C3;
    return float(((1.0 + z * C0) + w * C1) + (w * z) * r);
}

float tandf(double x, int odd) {
    static const double T[] = {0x15554d3418c99f.0p-54, 0x1112fd38999f72.0p-55, 0x1b54c91d865afe.0p-57,
                               0x191df3908c33ce.0p-58, 0x185dadfcecf44e.0p-61, 0x1362b9bf971bcd.0p-59};
    const double z = x * x;
    double r = T[4] + z * T[5];
    const double t = T[2] + z * T[3], w = z * z, s = z * x, u = T[0] + z * T[1];
    r = (x + s * u) + (s * w) * (t + w * r);
    return float(odd ? -1.0 / r : r);
}

// x reduced by multiples of pi/2. musl falls back to a multi-precision
// reduction above 2^28; the game never takes the sine of anything that large,
// so the medium-range reduction is kept for every finite x (still exact enough
// below 2^28, and deterministic everywhere).
int remPio2f(float x, double* y) {
    const double toint = 1.5 / 2.220446049250313080847e-16, pio4 = 0x1.921fb6p-1,
                 invpio2 = 6.36619772367581382433e-01, pio2_1 = 1.57079631090164184570e+00,
                 pio2_1t = 1.58932547735281966916e-08;
    double fn = double(x) * invpio2 + toint - toint;
    int n = int(int32_t(fn));
    *y = x - fn * pio2_1 - fn * pio2_1t;
    if (*y < -pio4) {
        --n;
        fn -= 1;
        *y = x - fn * pio2_1 - fn * pio2_1t;
    } else if (*y > pio4) {
        ++n;
        fn += 1;
        *y = x - fn * pio2_1 - fn * pio2_1t;
    }
    return n;
}

const double kPio2 = 1.57079632679489661923;

float detAtanf(float x) {
    static const float atanhi[] = {4.6364760399e-01f, 7.8539812565e-01f, 9.8279368877e-01f, 1.5707962513e+00f};
    static const float atanlo[] = {5.0121582440e-09f, 3.7748947079e-08f, 3.4473217170e-08f, 7.5497894159e-08f};
    static const float aT[] = {3.3333328366e-01f, -1.9999158382e-01f, 1.4253635705e-01f, -1.0648017377e-01f,
                               6.1687607318e-02f};
    uint32_t ix = bitsOf(x);
    const uint32_t sign = ix >> 31;
    ix &= 0x7fffffff;
    int id;
    if (ix >= 0x4c800000) { // |x| >= 2^26
        if (std::isnan(x)) return x;
        const float z = atanhi[3] + 0x1p-120f;
        return sign ? -z : z;
    }
    if (ix < 0x3ee00000) { // |x| < 0.4375
        if (ix < 0x39800000) return x;
        id = -1;
    } else {
        x = std::fabs(x);
        if (ix < 0x3f980000) { // |x| < 1.1875
            if (ix < 0x3f300000) {
                id = 0;
                x = (2.0f * x - 1.0f) / (2.0f + x);
            } else {
                id = 1;
                x = (x - 1.0f) / (x + 1.0f);
            }
        } else if (ix < 0x401c0000) { // |x| < 2.4375
            id = 2;
            x = (x - 1.5f) / (1.0f + 1.5f * x);
        } else {
            id = 3;
            x = -1.0f / x;
        }
    }
    float z = x * x;
    const float w = z * z;
    const float s1 = z * (aT[0] + w * (aT[2] + w * aT[4]));
    const float s2 = w * (aT[1] + w * aT[3]);
    if (id < 0) return x - x * (s1 + s2);
    z = atanhi[id] - ((x * (s1 + s2) - atanlo[id]) - x);
    return sign ? -z : z;
}

} // namespace

extern "C" float osDetSinf(float x) {
    uint32_t ix = bitsOf(x);
    const int sign = int(ix >> 31);
    ix &= 0x7fffffff;
    if (ix <= 0x3f490fda) { // |x| ~<= pi/4
        if (ix < 0x39800000) return x;
        return sindf(x);
    }
    if (ix <= 0x407b53d1) { // |x| ~<= 5pi/4
        if (ix <= 0x4016cbe3) return sign ? -cosdf(x + kPio2) : cosdf(x - kPio2);
        return sindf(sign ? -(x + 2 * kPio2) : -(x - 2 * kPio2));
    }
    if (ix <= 0x40e231d5) { // |x| ~<= 9pi/4
        if (ix <= 0x40afeddf) return sign ? cosdf(x + 3 * kPio2) : -cosdf(x - 3 * kPio2);
        return sindf(sign ? x + 4 * kPio2 : x - 4 * kPio2);
    }
    if (ix >= 0x7f800000) return x - x;
    double y;
    switch (remPio2f(x, &y) & 3) {
    case 0: return sindf(y);
    case 1: return cosdf(y);
    case 2: return sindf(-y);
    default: return -cosdf(y);
    }
}

extern "C" float osDetCosf(float x) {
    uint32_t ix = bitsOf(x);
    const int sign = int(ix >> 31);
    ix &= 0x7fffffff;
    if (ix <= 0x3f490fda) {
        if (ix < 0x39800000) return 1.0f;
        return cosdf(x);
    }
    if (ix <= 0x407b53d1) {
        if (ix > 0x4016cbe3) return -cosdf(sign ? x + 2 * kPio2 : x - 2 * kPio2);
        return sign ? sindf(x + kPio2) : sindf(kPio2 - x);
    }
    if (ix <= 0x40e231d5) {
        if (ix > 0x40afeddf) return cosdf(sign ? x + 4 * kPio2 : x - 4 * kPio2);
        return sign ? sindf(-x - 3 * kPio2) : sindf(x - 3 * kPio2);
    }
    if (ix >= 0x7f800000) return x - x;
    double y;
    switch (remPio2f(x, &y) & 3) {
    case 0: return cosdf(y);
    case 1: return sindf(-y);
    case 2: return -cosdf(y);
    default: return sindf(y);
    }
}

extern "C" float osDetTanf(float x) {
    uint32_t ix = bitsOf(x);
    const int sign = int(ix >> 31);
    ix &= 0x7fffffff;
    if (ix <= 0x3f490fda) {
        if (ix < 0x39800000) return x;
        return tandf(x, 0);
    }
    if (ix <= 0x407b53d1) {
        if (ix <= 0x4016cbe3) return tandf(sign ? x + kPio2 : x - kPio2, 1);
        return tandf(sign ? x + 2 * kPio2 : x - 2 * kPio2, 0);
    }
    if (ix <= 0x40e231d5) {
        if (ix <= 0x40afeddf) return tandf(sign ? x + 3 * kPio2 : x - 3 * kPio2, 1);
        return tandf(sign ? x + 4 * kPio2 : x - 4 * kPio2, 0);
    }
    if (ix >= 0x7f800000) return x - x;
    double y;
    const int n = remPio2f(x, &y);
    return tandf(y, n & 1);
}

extern "C" float osDetAtan2f(float y, float x) {
    const float pi = 3.1415927410e+00f, pi_lo = -8.7422776573e-08f;
    if (std::isnan(x) || std::isnan(y)) return x + y;
    uint32_t ix = bitsOf(x), iy = bitsOf(y);
    if (ix == 0x3f800000) return detAtanf(y); // x == 1.0
    const uint32_t m = ((iy >> 31) & 1) | ((ix >> 30) & 2); // 2*sign(x) + sign(y)
    ix &= 0x7fffffff;
    iy &= 0x7fffffff;
    if (iy == 0) {
        switch (m) {
        case 0:
        case 1: return y;
        case 2: return pi;
        default: return -pi;
        }
    }
    if (ix == 0) return (m & 1) ? -pi / 2 : pi / 2;
    if (ix == 0x7f800000) {
        if (iy == 0x7f800000) {
            switch (m) {
            case 0: return pi / 4;
            case 1: return -pi / 4;
            case 2: return 3 * pi / 4;
            default: return -3 * pi / 4;
            }
        }
        switch (m) {
        case 0: return 0.0f;
        case 1: return -0.0f;
        case 2: return pi;
        default: return -pi;
        }
    }
    if (ix + (26u << 23) < iy || iy == 0x7f800000) return (m & 1) ? -pi / 2 : pi / 2;
    float z;
    if ((m & 2) && iy + (26u << 23) < ix) z = 0.0f;
    else z = detAtanf(std::fabs(y / x));
    switch (m) {
    case 0: return z;
    case 1: return -z;
    case 2: return pi - (z - pi_lo);
    default: return (z - pi_lo) - pi;
    }
}

// x^n for whole n, by squaring: the only pow the simulation needs.
extern "C" double osDetPowi(double x, int n) {
    const bool invert = n < 0;
    unsigned int e = invert ? 0u - unsigned(n) : unsigned(n);
    double result = 1.0;
    while (e != 0) {
        if (e & 1) result *= x;
        x *= x;
        e >>= 1;
    }
    return invert ? 1.0 / result : result;
}
