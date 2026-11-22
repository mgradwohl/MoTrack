#pragma once

#include "science/matching/template_matcher.hpp"

#include <cstdint>

namespace motrack::science::matching_detail
{

// Java and Java2D primitives behind TemplateMatcher, ported operation by operation from OpenJDK 17
// (java.awt.geom, sun.awt.geom.Curve, libawt's AlphaMath.c/AlphaMacros.h) and OSP d56207b0. They are
// exposed here for focused tests; the library is compiled with -ffp-contract=off so that every
// expression rounds as Java's does.

/// `Shape.contains(x, y)` for AutoTracker's mask after `createTransformedShape` translates it by
/// (-bounds.x, -bounds.y): a Path2D.Double of four cubics (ellipse) or four lines (rectangle).
[[nodiscard]] bool maskContains(const MatchMask& mask, PixelPoint boundsOrigin, double x, double y) noexcept;

/// libawt's `mul8table[a][b]` and `div8table[b][a]` (MUL8(a, b) and DIV8(a, b)).
[[nodiscard]] int mul8(int a, int b) noexcept;
[[nodiscard]] int div8(int a, int b) noexcept;

/// `(jint) (extraAlpha * 255.0 + 0.5)` for `AlphaComposite.getInstance(SRC_OVER, 1.0f * alpha / 255)`.
[[nodiscard]] int compositeExtraAlpha(int alpha) noexcept;

/// The IntArgb-to-IntArgb SrcOver MaskBlit of one pixel without a coverage mask (extraA from
/// compositeExtraAlpha). Both pixels are non-premultiplied.
[[nodiscard]] ArgbPixel srcOver(ArgbPixel source, ArgbPixel destination, int extraAlpha) noexcept;

/// Coefficients {c0, c1, c2} of `KnownPolynomial.fitData({-1, 0, 1}, values)` for degree 2
/// (PolynomialLeastSquareFit's normal equations solved by OSP's LUPDecomposition).
struct Parabola
{
    double c0 = 0.0;
    double c1 = 0.0;
    double c2 = 0.0;
};
[[nodiscard]] Parabola fitParabola(double previous, double center, double next) noexcept;

/// Math.sin, Math.cos and Math.tan, identical on every platform: correctly rounded (double-double
/// reduction and series, then one rounding). Java only bounds these to 1 ulp; HotSpot x86-64, the
/// oracle's JVM, is correctly rounded for all but about 0.1-0.3% of angles (recorded in
/// tests/science/match-reference.json), whereas fdlibm (StrictMath) differs for about 3% and platform
/// libms vary. Arguments beyond 2^20 radians, infinities and NaN use <cmath>. Private to matching until
/// the shared Java Math library consolidates them (#263).
[[nodiscard]] double javaSin(double radians) noexcept;
[[nodiscard]] double javaCos(double radians) noexcept;
[[nodiscard]] double javaTan(double radians) noexcept;

/// `(int) Math.round(value)`: nearest integer with ties toward +infinity, NaN to 0, saturated to int.
[[nodiscard]] int javaRoundToInt(double value) noexcept;

/// Java's `Double.compare` ordering (-0.0 before +0.0, NaN after everything), as TreeMap<Double> uses.
[[nodiscard]] bool javaDoubleLess(double first, double second) noexcept;

} // namespace motrack::science::matching_detail
