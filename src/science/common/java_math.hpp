#pragma once

namespace motrack::science::java
{

// Deterministic replacements for Java's Math.sin, Math.cos and Math.tan, identical on every platform.
//
// Java specifies these only to within 1 ulp. HotSpot on x86-64 (the reference platform for Tracker's
// oracles) uses intrinsics that are correctly rounded for almost every argument: over 40,001 angles
// spanning [-pi, pi] they differ from the correctly rounded result for 36 sines, 54 cosines and 108
// tangents, whereas fdlibm (StrictMath) differs for about 1,300 of each. These functions are therefore
// correctly rounded (double-double reduction and series, then one rounding), so they equal HotSpot
// x86-64 except where its intrinsic is not correctly rounded; tests/science/match-reference.json lists
// those arguments. Platform libm results differ more often (Windows UCRT tan in about 4% of them).
//
// Arguments beyond 2^20 radians, infinities and NaN use <cmath>; no Tracker angle comes near that.

[[nodiscard]] double sin(double radians) noexcept;
[[nodiscard]] double cos(double radians) noexcept;
[[nodiscard]] double tan(double radians) noexcept;

} // namespace motrack::science::java
