#include "science/matching/matching_internal.hpp"
#include "science/matching/template_matcher.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <vector>

namespace motrack::science
{
namespace
{

// java.awt.geom.EllipseIterator: four cubics approximating a circle of radius 0.5 centered at
// (0.5, 0.5), scaled to the frame. CtrlVal is ArcIterator.btan(Math.PI / 2).
constexpr double CTRL_VAL = 0.5522847498307933;
constexpr double PCV = 0.5 + CTRL_VAL * 0.5;
constexpr double NCV = 0.5 - CTRL_VAL * 0.5;
constexpr std::array<std::array<double, 6>, 4> ELLIPSE_CONTROLS = {{
    {1.0, PCV, PCV, 1.0, 0.5, 1.0},
    {NCV, 1.0, 0.0, PCV, 0.0, 0.5},
    {0.0, NCV, NCV, 0.0, 0.5, 0.0},
    {PCV, 0.0, 1.0, NCV, 1.0, 0.5},
}};

// Curve.pointCrossingsForLine.
int lineCrossings(double px, double py, double x0, double y0, double x1, double y1) noexcept
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
        return (y0 < y1) ? 1 : -1;
    }
    const double xIntercept = x0 + (py - y0) * (x1 - x0) / (y1 - y0);
    if (px >= xIntercept)
    {
        return 0;
    }
    return (y0 < y1) ? 1 : -1;
}

struct Cubic
{
    double x0;
    double y0;
    double xc0;
    double yc0;
    double xc1;
    double yc1;
    double x1;
    double y1;
};

// Curve.pointCrossingsForCubic: recursive subdivision until the curve's hull clears the point.
// NOLINTNEXTLINE(misc-no-recursion): bounded by Java's 52-level limit.
int cubicCrossings(double px, double py, const Cubic& c, int level) noexcept
{
    if (py < c.y0 && py < c.yc0 && py < c.yc1 && py < c.y1)
    {
        return 0;
    }
    if (py >= c.y0 && py >= c.yc0 && py >= c.yc1 && py >= c.y1)
    {
        return 0;
    }
    if (px >= c.x0 && px >= c.xc0 && px >= c.xc1 && px >= c.x1)
    {
        return 0;
    }
    if (px < c.x0 && px < c.xc0 && px < c.xc1 && px < c.x1)
    {
        if (py >= c.y0)
        {
            if (py < c.y1)
            {
                return 1;
            }
        }
        else if (py >= c.y1)
        {
            return -1;
        }
        return 0;
    }
    constexpr int MAX_LEVEL = 52;
    if (level > MAX_LEVEL)
    {
        return lineCrossings(px, py, c.x0, c.y0, c.x1, c.y1);
    }
    double xMid = (c.xc0 + c.xc1) / 2;
    double yMid = (c.yc0 + c.yc1) / 2;
    const double xc0 = (c.x0 + c.xc0) / 2;
    const double yc0 = (c.y0 + c.yc0) / 2;
    const double xc1 = (c.xc1 + c.x1) / 2;
    const double yc1 = (c.yc1 + c.y1) / 2;
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
    const Cubic first{.x0 = c.x0, .y0 = c.y0, .xc0 = xc0, .yc0 = yc0, .xc1 = xc0m, .yc1 = yc0m, .x1 = xMid, .y1 = yMid};
    const Cubic second{.x0 = xMid, .y0 = yMid, .xc0 = xmc1, .yc0 = ymc1, .xc1 = xc1, .yc1 = yc1, .x1 = c.x1, .y1 = c.y1};
    return cubicCrossings(px, py, first, level + 1) + cubicCrossings(px, py, second, level + 1);
}

int ellipseCrossings(const MatchMask& mask, double tx, double ty, double px, double py) noexcept
{
    // EllipseIterator.currentSegment(double[]) followed by AffineTransform's translation.
    const auto pointX = [&](double control)
    {
        return (mask.x + control * mask.width) + tx;
    };
    const auto pointY = [&](double control)
    {
        return (mask.y + control * mask.height) + ty;
    };
    const auto& last = ELLIPSE_CONTROLS[3];
    const double moveX = pointX(last[4]);
    const double moveY = pointY(last[5]);
    double currentX = moveX;
    double currentY = moveY;
    int crossings = 0;
    for (const auto& control : ELLIPSE_CONTROLS)
    {
        const Cubic cubic{.x0 = currentX,
                          .y0 = currentY,
                          .xc0 = pointX(control[0]),
                          .yc0 = pointY(control[1]),
                          .xc1 = pointX(control[2]),
                          .yc1 = pointY(control[3]),
                          .x1 = pointX(control[4]),
                          .y1 = pointY(control[5])};
        crossings += cubicCrossings(px, py, cubic, 0);
        currentX = cubic.x1;
        currentY = cubic.y1;
    }
    // Path2D's closing segment adds nothing: the last cubic ends at the moveTo point, computed by the
    // same expression, so cury == movy.
    return crossings;
}

int rectangleCrossings(const MatchMask& mask, double tx, double ty, double px, double py) noexcept
{
    // RectIterator: move to (x, y), lines to (x + w, y), (x + w, y + h), (x, y + h) and (x, y).
    const double left = mask.x + tx;
    const double top = mask.y + ty;
    const double right = (mask.x + mask.width) + tx;
    const double bottom = (mask.y + mask.height) + ty;
    const std::array<std::array<double, 2>, 5> points = {{{left, top}, {right, top}, {right, bottom}, {left, bottom}, {left, top}}};
    int crossings = 0;
    for (std::size_t index = 1; index < points.size(); ++index)
    {
        crossings += lineCrossings(px, py, points[index - 1][0], points[index - 1][1], points[index][0], points[index][1]);
    }
    return crossings;
}

bool fitsInt(double value) noexcept
{
    return value >= static_cast<double>(std::numeric_limits<int>::min()) && value <= static_cast<double>(std::numeric_limits<int>::max());
}

// Line2D.Double and TemplateMatcher.getDistanceAndPointAt{X,Y}: {u, x, y} along P = P1 + u (P2 - P1).
struct Line
{
    double x1;
    double y1;
    double x2;
    double y2;
};

struct Intersection
{
    double u;
    double x;
    double y;
};

std::optional<Intersection> pointAtX(const Line& line, double x) noexcept
{
    const double dx = line.x2 - line.x1;
    if (dx == 0)
    {
        return std::nullopt;
    }
    const double u = (x - line.x1) / dx;
    return Intersection{.u = u, .x = x, .y = line.y1 + u * (line.y2 - line.y1)};
}

std::optional<Intersection> pointAtY(const Line& line, double y) noexcept
{
    const double dy = line.y2 - line.y1;
    if (dy == 0)
    {
        return std::nullopt;
    }
    const double u = (y - line.y1) / dy;
    return Intersection{.u = u, .x = line.x1 + u * (line.x2 - line.x1), .y = y};
}

struct JavaDoubleOrder
{
    bool operator()(double first, double second) const noexcept
    {
        return matching_detail::javaDoubleLess(first, second);
    }
};

} // namespace

namespace matching_detail
{

bool maskContains(const MatchMask& mask, PixelPoint boundsOrigin, double x, double y) noexcept
{
    // Path2D.contains: non-finite points are outside; an empty frame yields no path.
    if (!(x * 0.0 + y * 0.0 == 0.0) || mask.width < 0 || mask.height < 0)
    {
        return false;
    }
    const double tx = -static_cast<double>(boundsOrigin.x);
    const double ty = -static_cast<double>(boundsOrigin.y);
    const int crossings = mask.shape == MaskShape::Ellipse ? ellipseCrossings(mask, tx, ty, x, y) : rectangleCrossings(mask, tx, ty, x, y);
    return crossings != 0; // WIND_NON_ZERO
}

int javaRoundToInt(double value) noexcept
{
    if (std::isnan(value))
    {
        return 0;
    }
    // Math.round is floor(value + 1/2) evaluated exactly; value - floor(value) is exact for doubles.
    const double floor = std::floor(value);
    const double rounded = (value - floor >= 0.5) ? floor + 1.0 : floor;
    if (rounded <= static_cast<double>(std::numeric_limits<int>::min()))
    {
        return std::numeric_limits<int>::min();
    }
    if (rounded >= static_cast<double>(std::numeric_limits<int>::max()))
    {
        return std::numeric_limits<int>::max();
    }
    return static_cast<int>(rounded);
}

bool javaDoubleLess(double first, double second) noexcept
{
    if (first < second)
    {
        return true;
    }
    if (first > second)
    {
        return false;
    }
    // Double.doubleToLongBits: one canonical NaN, compared as signed 64-bit integers.
    const auto bits = [](double value)
    {
        return std::isnan(value) ? std::int64_t{0x7ff8000000000000} : std::bit_cast<std::int64_t>(value);
    };
    return bits(first) < bits(second);
}

} // namespace matching_detail

MatchMask MatchMask::fromCenter(MaskShape shape, double centerX, double centerY, double cornerX, double cornerY) noexcept
{
    const double halfWidth = std::abs(cornerX - centerX);
    const double halfHeight = std::abs(cornerY - centerY);
    return {.shape = shape, .x = centerX - halfWidth, .y = centerY - halfHeight, .width = halfWidth * 2.0, .height = halfHeight * 2.0};
}

std::optional<PixelRect> frameBounds(double x, double y, double width, double height) noexcept
{
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(width) || !std::isfinite(height))
    {
        return std::nullopt;
    }
    if (width < 0 || height < 0)
    {
        return PixelRect{};
    }
    const double x1 = std::floor(x);
    const double y1 = std::floor(y);
    const double x2 = std::ceil(x + width);
    const double y2 = std::ceil(y + height);
    if (!fitsInt(x1) || !fitsInt(y1) || !fitsInt(x2 - x1) || !fitsInt(y2 - y1))
    {
        return std::nullopt;
    }
    return PixelRect{
        .x = static_cast<int>(x1), .y = static_cast<int>(y1), .width = static_cast<int>(x2 - x1), .height = static_cast<int>(y2 - y1)};
}

std::optional<PixelRect> maskBounds(const MatchMask& mask) noexcept
{
    return frameBounds(mask.x, mask.y, mask.width, mask.height);
}

std::vector<std::uint8_t> maskPixels(const MatchMask& mask)
{
    const auto bounds = maskBounds(mask);
    if (!bounds || static_cast<std::int64_t>(bounds->width) * bounds->height > TemplateMatcher::MAX_TEMPLATE_PIXELS)
    {
        return {};
    }
    const PixelPoint origin{.x = bounds->x, .y = bounds->y};
    std::vector<std::uint8_t> included(static_cast<std::size_t>(bounds->width) * static_cast<std::size_t>(bounds->height));
    std::size_t index = 0;
    for (int y = 0; y < bounds->height; ++y)
    {
        for (int x = 0; x < bounds->width; ++x)
        {
            bool inside = true;
            for (int j = 0; j < 2; ++j)
            {
                for (int k = 0; k < 2; ++k)
                {
                    inside = inside && matching_detail::maskContains(mask, origin, x + j, y + k);
                }
            }
            included[index++] = inside ? 1 : 0;
        }
    }
    return included;
}

std::optional<std::vector<PixelPoint>> lineSearchPoints(PixelRect searchArea, const SearchLine& searchLine)
{
    constexpr double LARGE_NUMBER = 1.0E10;
    const double sx = searchArea.x;
    const double sy = searchArea.y;
    const double right = static_cast<double>(searchArea.x) + searchArea.width;
    const double bottom = static_cast<double>(searchArea.y) + searchArea.height;
    const double x0 = searchLine.x0;
    const double y0 = searchLine.y0;
    const double slope = -std::tan(searchLine.angle);
    Line line{.x1 = x0, .y1 = y0, .x2 = x0 + 1, .y2 = y0 + slope};
    if (std::abs(slope) > LARGE_NUMBER)
    {
        line = {.x1 = x0, .y1 = y0, .x2 = x0, .y2 = y0 + 1};
    }
    else if (std::abs(slope) < 1 / LARGE_NUMBER)
    {
        line = {.x1 = x0, .y1 = y0, .x2 = x0 + 1, .y2 = y0};
    }
    // Find the two edge crossings exactly as getSearchPoints does, including how it reuses p1/p2.
    constexpr double NOT_A_NUMBER = std::numeric_limits<double>::quiet_NaN();
    std::array<double, 2> p1{0.0, 0.0};
    std::array<double, 2> p2{NOT_A_NUMBER, NOT_A_NUMBER};
    std::array<double, 2>* p = &p1;
    const auto differ = [&]
    {
        return p1[0] != p2[0] || p1[1] != p2[1];
    };
    if (const auto hit = pointAtX(line, sx))
    {
        *p = {hit->x, hit->y};
        if ((*p)[1] >= sy && (*p)[1] <= bottom)
        {
            p = &p2;
        }
    }
    if (const auto hit = pointAtX(line, right))
    {
        *p = {hit->x, hit->y};
        if ((*p)[1] >= sy && (*p)[1] <= bottom)
        {
            p = p == &p1 ? &p2 : nullptr;
        }
    }
    if (p != nullptr)
    {
        if (const auto hit = pointAtY(line, sy))
        {
            *p = {hit->x, hit->y};
            if ((*p)[0] >= sx && (*p)[0] <= right)
            {
                if (p == &p1)
                {
                    p = &p2;
                }
                else if (differ())
                {
                    p = nullptr;
                }
            }
        }
        if (p == &p2)
        {
            if (const auto hit = pointAtY(line, bottom))
            {
                *p = {hit->x, hit->y};
                if ((*p)[0] >= sx && (*p)[0] <= right && differ())
                {
                    p = nullptr;
                }
            }
        }
    }
    if (p != nullptr)
    {
        return std::nullopt;
    }
    // Both ends lie on the rectangle's edges, so these bounds are within its integer extent.
    line = p1[0] <= p2[0] ? Line{.x1 = p1[0], .y1 = p1[1], .x2 = p2[0], .y2 = p2[1]}
                          : Line{.x1 = p2[0], .y1 = p2[1], .x2 = p1[0], .y2 = p1[1]};
    const double xMin = std::ceil(std::min(p1[0], p2[0]));
    const double xMax = std::floor(std::max(p1[0], p2[0]));
    const double yMin = std::ceil(std::min(p1[1], p2[1]));
    const double yMax = std::floor(std::max(p1[1], p2[1]));
    // TreeMap<Double, double[]>: ordered by Double.compare, later equal keys replace the point.
    std::map<double, std::array<double, 2>, JavaDoubleOrder> intersections;
    for (auto x = static_cast<std::int64_t>(xMin); x <= static_cast<std::int64_t>(xMax); ++x)
    {
        if (const auto hit = pointAtX(line, static_cast<double>(x)))
        {
            intersections.insert_or_assign(hit->u, std::array{hit->x, hit->y});
        }
    }
    for (auto y = static_cast<std::int64_t>(yMin); y <= static_cast<std::int64_t>(yMax); ++y)
    {
        if (const auto hit = pointAtY(line, static_cast<double>(y)))
        {
            intersections.insert_or_assign(hit->u, std::array{hit->x, hit->y});
        }
    }
    // Midpoints of consecutive intersections, truncated toward zero like Java's (int) cast.
    std::vector<PixelPoint> points;
    const std::array<double, 2>* previous = nullptr;
    for (const auto& [u, next] : intersections)
    {
        if (previous != nullptr)
        {
            const double midX = std::trunc(((*previous)[0] + next[0]) / 2);
            const double midY = std::trunc(((*previous)[1] + next[1]) / 2);
            points.push_back({.x = static_cast<int>(midX) - searchArea.x, .y = static_cast<int>(midY) - searchArea.y});
        }
        previous = &next;
    }
    return points;
}

} // namespace motrack::science
