#pragma once

// Java language arithmetic the OSP filters rely on. Internal to motrack_filters.

#include "science/filters/filter_image.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace motrack::science::java
{

/// Java's (int) narrowing of a double: truncation toward zero, NaN -> 0, saturating at the int range.
inline std::int32_t toInt(double value) noexcept
{
    if (std::isnan(value))
    {
        return 0;
    }
    if (value >= 2147483647.0)
    {
        return std::numeric_limits<std::int32_t>::max();
    }
    if (value <= -2147483648.0)
    {
        return std::numeric_limits<std::int32_t>::min();
    }
    return static_cast<std::int32_t>(value);
}

/// Java's (int) narrowing of a float.
inline std::int32_t toInt(float value) noexcept
{
    return toInt(static_cast<double>(value));
}

/// Java's (long) narrowing of a double.
inline std::int64_t toLong(double value) noexcept
{
    if (std::isnan(value))
    {
        return 0;
    }
    if (value >= 9223372036854775807.0)
    {
        return std::numeric_limits<std::int64_t>::max();
    }
    if (value <= -9223372036854775808.0)
    {
        return std::numeric_limits<std::int64_t>::min();
    }
    return static_cast<std::int64_t>(value);
}

/// Math.round(double): the closest long, ties toward positive infinity, NaN -> 0.
inline std::int64_t round(double value) noexcept
{
    if (std::isnan(value))
    {
        return 0;
    }
    const double floor = std::floor(value);
    // value - floor is exact; floor + 1 is exact below 2^53 and value is already integral above it.
    return toLong(value - floor >= 0.5 ? floor + 1.0 : floor);
}

/// Java's (int) narrowing of a long keeps its low 32 bits.
inline std::int32_t lowBits(std::int64_t value) noexcept
{
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(static_cast<std::uint64_t>(value)));
}

/// Math.min/Math.max for doubles: NaN if either operand is NaN.
inline double min(double a, double b) noexcept
{
    return std::isnan(a) || std::isnan(b) ? std::numeric_limits<double>::quiet_NaN() : (a <= b ? a : b);
}

inline double max(double a, double b) noexcept
{
    return std::isnan(a) || std::isnan(b) ? std::numeric_limits<double>::quiet_NaN() : (a >= b ? a : b);
}

/// Java's `(r << 16) | (g << 8) | b` on ints (any values, two's-complement bits), made opaque.
inline std::uint32_t packBits(std::int32_t red, std::int32_t green, std::int32_t blue) noexcept
{
    const auto r = static_cast<std::uint32_t>(red);
    const auto g = static_cast<std::uint32_t>(green);
    const auto b = static_cast<std::uint32_t>(blue);
    return OPAQUE_ALPHA | (r << 16U) | (g << 8U) | b;
}

/// Java's `(v << 16) | (v << 8) | v` for a grey value, made opaque.
inline std::uint32_t packGray(std::int32_t value) noexcept
{
    return packBits(value, value, value);
}

/// Wrapping 32-bit addition, as Java's int `+=` (never undefined behaviour here).
inline std::int32_t addWrapping(std::int32_t a, std::int32_t b) noexcept
{
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}

/**
 * Correctly rounded sin and cos (double-double evaluation), for |theta| <= 2^20; larger or non-finite
 * arguments use the platform's std::sin/std::cos. HotSpot's x86-64 Math.sin/cos intrinsics returned the
 * correctly rounded value at every argument the filter oracle probes, whereas some C runtimes (the
 * Windows UCRT among them) are off by one ulp, which brightness truncation makes visible.
 */
std::pair<double, double> sinCos(double theta) noexcept;

/// Wrapping 32-bit multiplication, as Java's int `*`.
inline std::int32_t multiplyWrapping(std::int32_t a, std::int32_t b) noexcept
{
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) * static_cast<std::uint32_t>(b));
}

} // namespace motrack::science::java
