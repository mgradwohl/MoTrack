#pragma once

#include <cmath>
#include <limits>

// Java's Math.max/Math.min, which the ported OSP code uses: NaN if either argument is NaN, and
// -0.0 orders below +0.0. std::max/std::min and std::fmax/std::fmin differ on one or both.
namespace motrack::science::detail
{

[[nodiscard]] inline double javaMax(double a, double b) noexcept
{
    if (std::isnan(a) || std::isnan(b))
    {
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (a == 0.0 && b == 0.0)
    {
        return std::signbit(a) ? b : a;
    }
    return a >= b ? a : b;
}

[[nodiscard]] inline double javaMin(double a, double b) noexcept
{
    if (std::isnan(a) || std::isnan(b))
    {
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (a == 0.0 && b == 0.0)
    {
        return std::signbit(a) ? a : b;
    }
    return a <= b ? a : b;
}

} // namespace motrack::science::detail
