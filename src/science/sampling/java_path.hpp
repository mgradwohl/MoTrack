#pragma once

// Internal: the subset of java.awt.geom that Tracker's RGB regions depend on, ported operation by
// operation from OpenJDK 17 (Path2D.Double, sun.awt.geom.Curve, Rectangle2D/RectangularShape
// bounds) so that pixel inclusion matches the pinned reference bit for bit. Not part of the public API.

#include <cstdint>
#include <vector>

namespace motrack::science::detail
{

/// Java's `(int)` conversion of a double: NaN becomes 0, out-of-range values saturate, others truncate.
[[nodiscard]] int javaIntCast(double value) noexcept;

/// `Curve.pointCrossingsForLine`: crossings of the ray to the right of (px, py) with a line segment.
[[nodiscard]] int pointCrossingsForLine(double px, double py, double x0, double y0, double x1, double y1) noexcept;

/// `Curve.pointCrossingsForCubic`, subdividing to at most 52 levels as Java does.
[[nodiscard]] int pointCrossingsForCubic(
    double px, double py, double x0, double y0, double xc0, double yc0, double xc1, double yc1, double x1, double y1, int level) noexcept;

/// The double-precision path of `Path2D.Double` with its default non-zero winding rule, limited to
/// the single subpath every Tracker region outline has: one `moveTo` first, then segments.
///
/// `closePath` is not stored: for one subpath, Java's `SEG_CLOSE` adds the closing line exactly as
/// the implicit closure at the end of `pointCrossings` does, so both give the same crossings.
class JavaPath
{
  public:
    /// The segments after the initial `moveTo`.
    enum class Segment : std::uint8_t
    {
        LineTo,
        CubicTo,
    };

    /// Starts the subpath. Precondition: the path is empty.
    void moveTo(double x, double y);
    void lineTo(double x, double y);
    void curveTo(double x1, double y1, double x2, double y2, double x3, double y3);

    /// `AffineTransform.getTranslateInstance(tx, ty).createTransformedShape(path)`: adds the
    /// offset to every coordinate unless both components are zero (the identity state).
    void translate(double tx, double ty) noexcept;

    /// `Path2D.contains(x, y)`: false for non-finite points and for a lone `moveTo` (Java's
    /// "fewer than two segments"), otherwise a non-zero `Path2D.Double.pointCrossings` count.
    [[nodiscard]] bool contains(double x, double y) const noexcept;

    /// Control-point bounds `{minX, minY, maxX, maxY}` of `Path2D.Double.getBounds2D` (zeros when empty).
    struct Bounds
    {
        double minX = 0.0;
        double minY = 0.0;
        double maxX = 0.0;
        double maxY = 0.0;
    };
    [[nodiscard]] Bounds bounds2D() const noexcept;

  private:
    [[nodiscard]] int pointCrossings(double px, double py) const noexcept;

    std::vector<Segment> m_Segments;
    std::vector<double> m_Coordinates;
};

} // namespace motrack::science::detail
