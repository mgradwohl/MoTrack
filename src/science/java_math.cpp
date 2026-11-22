#include "science/java_math.hpp"

#include <cmath>
#include <cstdint>
#include <limits>

namespace motrack::science::java
{
namespace
{
constexpr double LONG_RANGE = 9223372036854775808.0; // 2^63: (double) Long.MAX_VALUE and -Long.MIN_VALUE
} // namespace

double pow(double base, double exponent) noexcept
{
    if (std::isnan(exponent) || (std::fabs(base) == 1.0 && std::isinf(exponent)))
    {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return std::pow(base, exponent);
}

double round(double value) noexcept
{
    if (std::isnan(value))
    {
        return 0.0;
    }
    if (value >= LONG_RANGE)
    {
        return LONG_RANGE;
    }
    if (value <= -LONG_RANGE)
    {
        return -LONG_RANGE;
    }
    // Exact below 2^52; larger magnitudes are already integers, so the difference is zero.
    const double floor = std::floor(value);
    const double nearest = value - floor >= 0.5 ? floor + 1.0 : floor;
    return static_cast<double>(static_cast<std::int64_t>(nearest));
}

double rint(double value) noexcept
{
    return std::nearbyint(value);
}

double signum(double value) noexcept
{
    return value == 0.0 || std::isnan(value) ? value : std::copysign(1.0, value);
}

double min(double first, double second) noexcept
{
    if (std::isnan(first))
    {
        return first;
    }
    if (first == 0.0 && second == 0.0 && std::signbit(second))
    {
        return second;
    }
    return first <= second ? first : second;
}

double max(double first, double second) noexcept
{
    if (std::isnan(first))
    {
        return first;
    }
    if (first == 0.0 && second == 0.0 && std::signbit(first))
    {
        return second;
    }
    return first >= second ? first : second;
}

double ieeeRemainder(double dividend, double divisor) noexcept
{
    return std::remainder(dividend, divisor);
}

} // namespace motrack::science::java
