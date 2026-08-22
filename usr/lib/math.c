#include <math.h>

/*
 * Minimal transcendentals. DOOM's rendering is entirely fixed-point
 * (m_fixed.c, tables.c hold precomputed sine tables), so nothing here is
 * on a hot path or feeds a pixel — these exist to satisfy the handful of
 * call sites in setup and menu code that reference them.
 *
 * Accuracy is "good enough to not look wrong", not IEEE-correct. If
 * something ever depends on these numerically, replace them rather than
 * trusting them.
 */

double fabs(double x)  { return x < 0 ? -x : x; }
double floor(double x) { double t = (double)(long long)x; return (x < 0 && t != x) ? t - 1 : t; }
double ceil(double x)  { double t = (double)(long long)x; return (x > 0 && t != x) ? t + 1 : t; }

double fmod(double a, double b) {
    if (b == 0) return 0;
    return a - b * (double)(long long)(a / b);
}

double sqrt(double x) {
    if (x <= 0) return 0;
    /* Newton-Raphson; converges to double precision well inside 40
     * iterations for any input this will see. */
    double r = x, prev = 0;
    for (int i = 0; i < 40 && r != prev; i++) {
        prev = r;
        r = 0.5 * (r + x / r);
    }
    return r;
}

/* Taylor series about 0 after range reduction into [-pi, pi]. */
double sin(double x) {
    x = fmod(x, 2 * M_PI);
    if (x >  M_PI) x -= 2 * M_PI;
    if (x < -M_PI) x += 2 * M_PI;
    double term = x, sum = x, x2 = x * x;
    for (int n = 1; n < 12; n++) {
        term *= -x2 / (double)((2 * n) * (2 * n + 1));
        sum += term;
    }
    return sum;
}

double cos(double x) { return sin(x + M_PI / 2); }

double tan(double x) {
    double c = cos(x);
    return c == 0 ? 0 : sin(x) / c;
}

double atan(double x) {
    /* Series converges only for |x| <= 1, so fold larger inputs through
     * the identity atan(x) = pi/2 - atan(1/x). */
    int inv = 0;
    if (x > 1)       { x = 1 / x; inv = 1; }
    else if (x < -1) { x = 1 / x; inv = -1; }

    double term = x, sum = x, x2 = x * x;
    for (int n = 1; n < 60; n++) {
        term *= -x2;
        sum += term / (double)(2 * n + 1);
    }
    if (inv > 0)  return M_PI / 2 - sum;
    if (inv < 0)  return -M_PI / 2 - sum;
    return sum;
}

double atan2(double y, double x) {
    if (x > 0)            return atan(y / x);
    if (x < 0 && y >= 0)  return atan(y / x) + M_PI;
    if (x < 0 && y < 0)   return atan(y / x) - M_PI;
    if (y > 0)            return M_PI / 2;
    if (y < 0)            return -M_PI / 2;
    return 0;
}

double exp(double x) {
    double term = 1, sum = 1;
    for (int n = 1; n < 30; n++) {
        term *= x / (double)n;
        sum += term;
    }
    return sum;
}

double log(double x) {
    if (x <= 0) return 0;
    /* Halley iteration on exp(r) = x — cheap and stable enough here. */
    double r = 0;
    for (int i = 0; i < 40; i++) {
        double e = exp(r);
        if (e == 0) break;
        r -= (e - x) / e;
    }
    return r;
}

double pow(double b, double e) {
    if (b == 0) return 0;
    if (e == 0) return 1;
    return exp(e * log(b));
}
