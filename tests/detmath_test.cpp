// The simulation's own trig (src/net/detmath.cpp) against the C library in
// double precision: every result within 2 float ulps over the ranges the game
// uses, which catches a mistyped constant or branch.

#include <cmath>
#include <cstdio>

extern "C" float osDetSinf(float);
extern "C" float osDetCosf(float);
extern "C" float osDetTanf(float);
extern "C" float osDetAtan2f(float, float);
extern "C" double osDetPowi(double, int);

namespace {

int g_failures = 0;

bool close(double expect, float got, double ulps) {
    const double tolerance = ulps * std::fabs(std::nextafter(float(expect), INFINITY) - float(expect)) + 1e-30;
    return std::fabs(double(got) - expect) <= tolerance;
}

void check(const char* what, double input, double expect, float got) {
    if (!close(expect, got, 2.0)) {
        if (++g_failures <= 10) std::fprintf(stderr, "%s(%.9g) = %.9g, expected %.9g\n", what, input, got, expect);
    }
}

} // namespace

int main() {
    for (int i = -400000; i <= 400000; ++i) {
        const float x = float(i) * 0.00025f; // -100 .. 100
        check("sin", x, std::sin(double(x)), osDetSinf(x));
        check("cos", x, std::cos(double(x)), osDetCosf(x));
        const double t = std::tan(double(x));
        if (std::fabs(t) < 1e4) check("tan", x, t, osDetTanf(x));
    }
    for (int i = -300; i <= 300; ++i) {
        for (int j = -300; j <= 300; ++j) {
            const float y = float(i) * 0.37f, x = float(j) * 0.41f;
            if (x == 0.0f && y == 0.0f) continue;
            check("atan2", y * 1000 + x, std::atan2(double(y), double(x)), osDetAtan2f(y, x));
        }
    }
    if (osDetAtan2f(0.0f, -1.0f) != float(3.14159265358979)) ++g_failures;
    if (osDetPowi(0.5, 10) != std::pow(0.5, 10) || osDetPowi(3.0, 0) != 1.0 || osDetPowi(2.0, -2) != 0.25) {
        ++g_failures;
        std::fprintf(stderr, "powi wrong\n");
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "%d mismatches\n", g_failures);
        return 1;
    }
    std::puts("detmath: all within 2 ulps");
    return 0;
}
