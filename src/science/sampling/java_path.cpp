#include "science/sampling/java_path.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace motrack::science::detail
{

namespace
{

// Java's recursion limit: "double precision only has 52 bits of mantissa".
constexpr int MAX_CUBIC_LEVEL = 52;

} // namespace

int javaIntCast(double value) noexcept
{
    if (value >= static_cast<double>(std::numeric_limits<int>::max()))
    {
        return std::numeric_limits<int>::max();
    }
    if (!(value > static_cast<double>(std::numeric_limits<int>::min())))
    {
        return std::isnan(value) ? 0 : std::numeric_limits<int>::min();
    }
    return static_cast<int>(value);
}

int pointCrossingsForLine(double px, double py, double x0, double y0, double x1, double y1) noexcept
{
    if (py < y0 && py < y1)
    {
        return 0;
    }
    if (py >= y0 && py >= y1)
    {
        return 0;
    }
    if (px >= x0 && px >= x1)
    {
        return 0;
    }
    if (px < x0 && px < x1)
    {
        return y0 < y1 ? 1 : -1;
    }
    const double xIntercept = x0 + ((py - y0) * (x1 - x0) / (y1 - y0));
    if (px >= xIntercept)
    {
        return 0;
    }
    return y0 < y1 ? 1 : -1;
}

int pointCrossingsForCubic(
    double px, double py, double x0, double y0, double xc0, double yc0, double xc1, double yc1, double x1, double y1, int level) noexcept
{
    if (py < y0 && py < yc0 && py < yc1 && py < y1)
    {
        return 0;
    }
    if (py >= y0 && py >= yc0 && py >= yc1 && py >= y1)
    {
        return 0;
    }
    if (px >= x0 && px >= xc0 && px >= xc1 && px >= x1)
    {
        return 0;
    }
    if (px < x0 && px < xc0 && px < xc1 && px < x1)
    {
        if (py >= y0)
        {
            if (py < y1)
            {
                return 1;
            }
        }
        else if (py >= y1)
        {
            return -1;
        }
        return 0;
    }
    if (level > MAX_CUBIC_LEVEL)
    {
        return pointCrossingsForLine(px, py, x0, y0, x1, y1);
    }
    double xMid = (xc0 + xc1) / 2;
    double yMid = (yc0 + yc1) / 2;
    xc0 = (x0 + xc0) / 2;
    yc0 = (y0 + yc0) / 2;
    xc1 = (xc1 + x1) / 2;
    yc1 = (yc1 + y1) / 2;
    const double xc0m = (xc0 + xMid) / 2;
    const double yc0m = (yc0 + yMid) / 2;
    const double xmc1 = (xMid + xc1) / 2;
    const double ymc1 = (yMid + yc1) / 2;
    xMid = (xc0m + xmc1) / 2;
    yMid = (yc0m + ymc1) / 2;
    if (std::isnan(xMid) || std::isnan(yMid))
    {
        return 0;
    }
    return pointCrossingsForCubic(px, py, x0, y0, xc0, yc0, xc0m, yc0m, xMid, yMid, level + 1) +
           pointCrossingsForCubic(px, py, xMid, yMid, xmc1, ymc1, xc1, yc1, x1, y1, level + 1);
}

void JavaPath::moveTo(double x, double y)
{
    m_Coordinates.push_back(x);
    m_Coordinates.push_back(y);
}

void JavaPath::lineTo(double x, double y)
{
    m_Segments.push_back(Segment::LineTo);
    m_Coordinates.push_back(x);
    m_Coordinates.push_back(y);
}

void JavaPath::curveTo(double x1, double y1, double x2, double y2, double x3, double y3)
{
    m_Segments.push_back(Segment::CubicTo);
    m_Coordinates.insert(m_Coordinates.end(), {x1, y1, x2, y2, x3, y3});
}

void JavaPath::translate(double tx, double ty) noexcept
{
    if (tx == 0.0 && ty == 0.0)
    {
        return;
    }
    for (std::size_t i = 0; i < m_Coordinates.size(); i += 2)
    {
        m_Coordinates[i] += tx;
        m_Coordinates[i + 1] += ty;
    }
}

bool JavaPath::contains(double x, double y) const noexcept
{
    // Java's finiteness test: N * 0.0 is 0.0 only for finite N.
    if ((x * 0.0) + (y * 0.0) != 0.0)
    {
        return false;
    }
    // Path2D: fewer than two segment types (at most the initial moveTo) contain nothing.
    if (m_Segments.empty())
    {
        return false;
    }
    return pointCrossings(x, y) != 0;
}

int JavaPath::pointCrossings(double px, double py) const noexcept
{
    // Precondition (from contains): a moveTo and at least one segment.
    const std::vector<double>& coords = m_Coordinates;
    double movX = coords[0];
    double movY = coords[1];
    double curX = movX;
    double curY = movY;
    int crossings = 0;
    std::size_t ci = 2;
    for (const Segment segment : m_Segments)
    {
        if (segment == Segment::LineTo)
        {
            const double endX = coords[ci];
            const double endY = coords[ci + 1];
            ci += 2;
            crossings += pointCrossingsForLine(px, py, curX, curY, endX, endY);
            curX = endX;
            curY = endY;
        }
        else
        {
            const double endX = coords[ci + 4];
            const double endY = coords[ci + 5];
            crossings +=
                pointCrossingsForCubic(px, py, curX, curY, coords[ci], coords[ci + 1], coords[ci + 2], coords[ci + 3], endX, endY, 0);
            ci += 6;
            curX = endX;
            curY = endY;
        }
    }
    // The implicit closure (also exactly what an explicit SEG_CLOSE would add).
    if (curY != movY)
    {
        crossings += pointCrossingsForLine(px, py, curX, curY, movX, movY);
    }
    return crossings;
}

JavaPath::Bounds JavaPath::bounds2D() const noexcept
{
    if (m_Coordinates.empty())
    {
        return {};
    }
    Bounds bounds{.minX = m_Coordinates[0], .minY = m_Coordinates[1], .maxX = m_Coordinates[0], .maxY = m_Coordinates[1]};
    for (std::size_t i = 2; i < m_Coordinates.size(); i += 2)
    {
        bounds.minX = std::min(bounds.minX, m_Coordinates[i]);
        bounds.maxX = std::max(bounds.maxX, m_Coordinates[i]);
        bounds.minY = std::min(bounds.minY, m_Coordinates[i + 1]);
        bounds.maxY = std::max(bounds.maxY, m_Coordinates[i + 1]);
    }
    return bounds;
}

} // namespace motrack::science::detail
