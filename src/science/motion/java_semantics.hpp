#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

// Java language and library semantics that the motion kernels depend on. Internal to motrack_motion.

namespace motrack::science::motion_detail
{

/// Java's (int) cast of a double: truncation toward zero, NaN to 0, saturation at the int range.
[[nodiscard]] inline int javaDoubleToInt(double value) noexcept
{
    constexpr auto LOWEST = static_cast<double>(std::numeric_limits<int>::min());
    constexpr auto HIGHEST = static_cast<double>(std::numeric_limits<int>::max());
    return std::isnan(value) ? 0 : static_cast<int>(std::clamp(value, LOWEST, HIGHEST));
}

/// Double.compare: a total order with -0.0 below +0.0 and every NaN equal and above +infinity.
[[nodiscard]] inline int javaDoubleCompare(double first, double second) noexcept
{
    if (first < second)
    {
        return -1;
    }
    if (first > second)
    {
        return 1;
    }
    // Equal, signed zeros or NaN: compare doubleToLongBits, which maps every NaN to one canonical value.
    const auto bits = [](double value)
    {
        return std::isnan(value) ? std::int64_t{0x7ff8000000000000} : std::bit_cast<std::int64_t>(value);
    };
    const auto firstBits = bits(first);
    const auto secondBits = bits(second);
    return firstBits == secondBits ? 0 : (firstBits < secondBits ? -1 : 1);
}

/// Whether Java's Math.round(value) is 0: true for NaN and for -0.5 <= value < 0.5 (ties round up).
[[nodiscard]] inline bool javaRoundsToZero(double value) noexcept
{
    return std::isnan(value) || (value >= -0.5 && value < 0.5);
}

} // namespace motrack::science::motion_detail
