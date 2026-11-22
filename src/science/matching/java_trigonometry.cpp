#include "science/matching/matching_internal.hpp"

#include <cmath>

namespace motrack::science::matching_detail
{
namespace
{

/// An unevaluated sum hi + lo with |lo| <= ulp(hi) / 2 (double-double arithmetic).
struct DoubleDouble
{
    double hi = 0.0;
    double lo = 0.0;
};

DoubleDouble quickTwoSum(double a, double b) noexcept
{
    const double sum = a + b;
    return {.hi = sum, .lo = b - (sum - a)};
}

DoubleDouble twoSum(double a, double b) noexcept
{
    const double sum = a + b;
    const double bb = sum - a;
    return {.hi = sum, .lo = (a - (sum - bb)) + (b - bb)};
}

DoubleDouble twoProduct(double a, double b) noexcept
{
    const double product = a * b;
    return {.hi = product, .lo = std::fma(a, b, -product)};
}

DoubleDouble add(DoubleDouble x, DoubleDouble y) noexcept
{
    DoubleDouble sum = twoSum(x.hi, y.hi);
    const DoubleDouble low = twoSum(x.lo, y.lo);
    sum.lo += low.hi;
    sum = quickTwoSum(sum.hi, sum.lo);
    sum.lo += low.lo;
    return quickTwoSum(sum.hi, sum.lo);
}

DoubleDouble negate(DoubleDouble x) noexcept
{
    return {.hi = -x.hi, .lo = -x.lo};
}

DoubleDouble multiply(DoubleDouble x, DoubleDouble y) noexcept
{
    DoubleDouble product = twoProduct(x.hi, y.hi);
    product.lo += (x.hi * y.lo) + (x.lo * y.hi);
    return quickTwoSum(product.hi, product.lo);
}

DoubleDouble multiply(DoubleDouble x, double y) noexcept
{
    DoubleDouble product = twoProduct(x.hi, y);
    product.lo += x.lo * y;
    return quickTwoSum(product.hi, product.lo);
}

DoubleDouble divide(DoubleDouble x, double y) noexcept
{
    const double quotient = x.hi / y;
    const DoubleDouble product = twoProduct(quotient, y);
    const double remainder = ((x.hi - product.hi) - product.lo) + x.lo;
    return quickTwoSum(quotient, remainder / y);
}

DoubleDouble divide(DoubleDouble x, DoubleDouble y) noexcept
{
    // Three quotient digits, each from the remaining dividend.
    const double first = x.hi / y.hi;
    DoubleDouble remainder = add(x, negate(multiply(y, first)));
    const double second = remainder.hi / y.hi;
    remainder = add(remainder, negate(multiply(y, second)));
    const double third = remainder.hi / y.hi;
    return add(quickTwoSum(first, second), {.hi = third, .lo = 0.0});
}

// pi / 2 = PIO2_1 + PIO2_2 + PIO2_3 to about 160 bits.
constexpr double PIO2_1 = 0x1.921fb54442d18p+0;
constexpr double PIO2_2 = 0x1.1a62633145c07p-54;
constexpr double PIO2_3 = -0x1.f1976b7ed8fbcp-110;
constexpr double TWO_OVER_PI = 0.6366197723675814;
// Beyond this, the three-part reduction loses accuracy; no Tracker angle comes near it.
constexpr double REDUCTION_LIMIT = 0x1p20;
// The Taylor series converges below a relative error of 1e-36 for |r| <= pi / 4 + tiny.
constexpr int TAYLOR_TERMS = 14;

struct SineCosine
{
    DoubleDouble sine;
    DoubleDouble cosine;
};

/// Precondition: radians is finite and 0 < |radians| <= REDUCTION_LIMIT.
SineCosine sineCosine(double radians) noexcept
{
    const double k = std::nearbyint(radians * TWO_OVER_PI);
    // r = radians - k pi / 2 in double-double; the first two products are exact (FMA).
    DoubleDouble r = add({.hi = radians, .lo = 0.0}, negate(twoProduct(k, PIO2_1)));
    r = add(r, negate(twoProduct(k, PIO2_2)));
    r = add(r, {.hi = -(k * PIO2_3), .lo = 0.0});
    const DoubleDouble r2 = multiply(r, r);
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
    switch (static_cast<int>(std::fmod(k, 4.0) + 4.0) % 4)
    {
    case 0:
        return {.sine = sine, .cosine = cosine};
    case 1:
        return {.sine = cosine, .cosine = negate(sine)};
    case 2:
        return {.sine = negate(sine), .cosine = negate(cosine)};
    default:
        return {.sine = negate(cosine), .cosine = sine};
    }
}

bool reducible(double radians) noexcept
{
    return radians != 0.0 && std::isfinite(radians) && std::abs(radians) <= REDUCTION_LIMIT;
}

} // namespace

// The high part of a normalized double-double is the double nearest its value, so each result is
// rounded once.

double javaSin(double radians) noexcept
{
    return reducible(radians) ? sineCosine(radians).sine.hi : std::sin(radians);
}

double javaCos(double radians) noexcept
{
    return reducible(radians) ? sineCosine(radians).cosine.hi : std::cos(radians);
}

double javaTan(double radians) noexcept
{
    if (!reducible(radians))
    {
        return std::tan(radians);
    }
    const auto values = sineCosine(radians);
    return divide(values.sine, values.cosine).hi;
}

} // namespace motrack::science::matching_detail
