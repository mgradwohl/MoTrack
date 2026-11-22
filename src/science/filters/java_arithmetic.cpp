#include "science/filters/java_arithmetic.hpp"

#include <cmath>
#include <utility>

namespace motrack::science::filters::detail
{

namespace
{

/// An unevaluated sum hi + lo with |lo| <= ulp(hi) / 2 (double-double arithmetic).
struct DoubleDouble
{
    double hi;
    double lo;
};

DoubleDouble quickTwoSum(double a, double b) noexcept
{
    const double s = a + b;
    return {.hi = s, .lo = b - (s - a)};
}

DoubleDouble twoSum(double a, double b) noexcept
{
    const double s = a + b;
    const double bb = s - a;
    return {.hi = s, .lo = (a - (s - bb)) + (b - bb)};
}

DoubleDouble twoProduct(double a, double b) noexcept
{
    const double p = a * b;
    return {.hi = p, .lo = std::fma(a, b, -p)};
}

DoubleDouble add(DoubleDouble x, DoubleDouble y) noexcept
{
    DoubleDouble s = twoSum(x.hi, y.hi);
    const DoubleDouble t = twoSum(x.lo, y.lo);
    s.lo += t.hi;
    s = quickTwoSum(s.hi, s.lo);
    s.lo += t.lo;
    return quickTwoSum(s.hi, s.lo);
}

DoubleDouble multiply(DoubleDouble x, DoubleDouble y) noexcept
{
    DoubleDouble p = twoProduct(x.hi, y.hi);
    p.lo += (x.hi * y.lo) + (x.lo * y.hi);
    return quickTwoSum(p.hi, p.lo);
}

DoubleDouble divide(DoubleDouble x, double y) noexcept
{
    const double q = x.hi / y;
    const DoubleDouble p = twoProduct(q, y);
    const double remainder = ((x.hi - p.hi) - p.lo) + x.lo;
    return quickTwoSum(q, remainder / y);
}

DoubleDouble negate(DoubleDouble x) noexcept
{
    return {.hi = -x.hi, .lo = -x.lo};
}

// pi / 2 = PIO2_1 + PIO2_2 + PIO2_3 to about 160 bits.
constexpr double PIO2_1 = 0x1.921fb54442d18p+0;
constexpr double PIO2_2 = 0x1.1a62633145c07p-54;
constexpr double PIO2_3 = -0x1.f1976b7ed8fbcp-110;
constexpr double TWO_OVER_PI = 0.6366197723675814;
// Beyond this, the three-part reduction loses accuracy; such arguments never come from Tracker data.
constexpr double REDUCTION_LIMIT = 0x1p20;
constexpr int TAYLOR_TERMS = 14;

} // namespace

std::pair<double, double> sinCos(double theta) noexcept
{
    if (theta == 0.0 || !std::isfinite(theta) || std::abs(theta) > REDUCTION_LIMIT)
    {
        return {std::sin(theta), std::cos(theta)};
    }
    const double k = std::nearbyint(theta * TWO_OVER_PI);
    // r = theta - k pi / 2 in double-double; every product is exact (FMA).
    DoubleDouble r = add({.hi = theta, .lo = 0.0}, negate(twoProduct(k, PIO2_1)));
    r = add(r, negate(twoProduct(k, PIO2_2)));
    r = add(r, {.hi = -(k * PIO2_3), .lo = 0.0});
    const DoubleDouble r2 = multiply(r, r);
    // Taylor series for |r| <= pi / 4 + tiny: 14 terms leave a relative error below 1e-36.
    DoubleDouble sine = r;
    DoubleDouble cosine{.hi = 1.0, .lo = 0.0};
    DoubleDouble sineTerm = r;
    DoubleDouble cosineTerm{.hi = 1.0, .lo = 0.0};
    for (int n = 1; n <= TAYLOR_TERMS; ++n)
    {
        sineTerm = negate(divide(multiply(sineTerm, r2), static_cast<double>((2 * n) * ((2 * n) + 1))));
        cosineTerm = negate(divide(multiply(cosineTerm, r2), static_cast<double>(((2 * n) - 1) * (2 * n))));
        sine = add(sine, sineTerm);
        cosine = add(cosine, cosineTerm);
    }
    // hi of a normalized double-double is the double nearest its value.
    switch (static_cast<int>(std::fmod(k, 4.0) + 4.0) % 4)
    {
    case 0:
        return {sine.hi, cosine.hi};
    case 1:
        return {cosine.hi, -sine.hi};
    case 2:
        return {-sine.hi, -cosine.hi};
    default:
        return {-cosine.hi, sine.hi};
    }
}

} // namespace motrack::science::filters::detail
