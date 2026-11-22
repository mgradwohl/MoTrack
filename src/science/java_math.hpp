#pragma once

namespace motrack::science::java
{

// Java `Math` operations whose special cases differ from <cmath> or have no direct C++ equivalent.
// Kernels that port OSP/Tracker numerics use these so their results follow Java's documented semantics.
// Functions that <cmath> already matches exactly (abs, ceil, floor, sqrt) and transcendental functions,
// which Java only specifies to within 1 ulp, are used directly from <cmath>.

/// Math.pow: like std::pow, except that a NaN exponent, or an infinite exponent with |base| == 1, is NaN.
[[nodiscard]] double pow(double base, double exponent) noexcept;

/// Math.round(double) converted back to double: nearest integer with ties toward positive infinity,
/// NaN to 0, and values outside the long range saturated to +/-2^63. Signed zeros become +0.
[[nodiscard]] double round(double value) noexcept;

/// Math.rint: nearest integer, ties to even, keeping the sign of zero (current rounding mode: nearest).
[[nodiscard]] double rint(double value) noexcept;

/// Math.signum: -1, +1, or the argument itself for signed zeros and NaN.
[[nodiscard]] double signum(double value) noexcept;

/// Math.min: NaN if either argument is NaN, and -0 is smaller than +0.
[[nodiscard]] double min(double first, double second) noexcept;

/// Math.max: NaN if either argument is NaN, and +0 is larger than -0.
[[nodiscard]] double max(double first, double second) noexcept;

/// Math.IEEEremainder: first - second * n with n the integer nearest first / second, ties to even.
[[nodiscard]] double ieeeRemainder(double dividend, double divisor) noexcept;

} // namespace motrack::science::java
