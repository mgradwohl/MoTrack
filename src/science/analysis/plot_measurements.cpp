#include "science/analysis/data_analysis.hpp"
#include "science/analysis/java_order.hpp"

#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <vector>

// Ports of the Data Tool measuring tools in DataToolTab (OSP d56207b0): findNearestXIndex,
// DataToolPlotter.refreshMeasurements (Coordinates and Slope) and DataToolPlotter.refreshArea.

namespace motrack::science
{
namespace
{

/// refreshArea's trapezoidal sum over the points it keeps, in their order.
AreaMeasurement trapezoidArea(std::span<const PlotPoint> used)
{
    AreaMeasurement result;
    result.pointCount = used.size();
    if (used.empty())
    {
        return result;
    }
    // The limit lines move to the x range of the area dataset, (x0, 0), the points, (x_{n-1}, 0).
    double firstX = used.front().x;
    double lastX = used.front().x;
    for (const PlotPoint& point : used)
    {
        firstX = detail::javaMin(firstX, point.x);
        lastX = detail::javaMax(lastX, point.x);
    }
    result.firstX = firstX;
    result.lastX = lastX;
    const std::size_t n = used.size();
    if (n < 2)
    {
        return result;
    }
    double area = used[0].y * (used[1].x - used[0].x);
    area += used[n - 1].y * (used[n - 1].x - used[n - 2].x);
    for (std::size_t i = 1; i + 1 < n; ++i)
    {
        area += used[i].y * (used[i + 1].x - used[i - 1].x);
    }
    area /= 2;
    if (!std::isnan(area))
    {
        result.area = area;
    }
    return result;
}

} // namespace

std::optional<std::size_t> nearestPointIndex(std::span<const PlotPoint> points, double x, double visibleMin, double visibleMax)
{
    const double clamped = detail::javaMin(visibleMax, detail::javaMax(visibleMin, x));
    std::optional<std::size_t> nearest;
    double nearestDistance = std::numeric_limits<double>::max();
    for (std::size_t i = 0; i < points.size(); ++i)
    {
        if (std::isnan(points[i].y))
        {
            continue;
        }
        const double distance = std::abs(clamped - points[i].x);
        if (distance < nearestDistance)
        {
            nearestDistance = distance;
            nearest = i;
        }
    }
    if (!nearest)
    {
        return std::nullopt;
    }
    std::size_t index = *nearest;
    if (points[index].x < visibleMin)
    {
        ++index;
    }
    if (index == points.size() || points[index].x > visibleMax)
    {
        index = points.size() - 1;
    }
    return index;
}

std::optional<PlotPoint> pointCoordinates(std::span<const PlotPoint> points, std::size_t index)
{
    if (index >= points.size() || std::isnan(points[index].y))
    {
        return std::nullopt;
    }
    return points[index];
}

std::optional<double> pointSlope(std::span<const PlotPoint> points, std::size_t index)
{
    if (index == 0 || index + 1 >= points.size() || std::isnan(points[index].y))
    {
        return std::nullopt;
    }
    const double slope = (points[index + 1].y - points[index - 1].y) / (points[index + 1].x - points[index - 1].x);
    if (std::isnan(slope))
    {
        return std::nullopt;
    }
    return slope;
}

std::optional<double> functionSlope(const ScalarFunction& function, double x, double dx)
{
    const double slope = (function(x + dx) - function(x - dx)) / (2 * dx);
    if (std::isnan(slope))
    {
        return std::nullopt;
    }
    return slope;
}

AreaMeasurement dataArea(std::span<const PlotPoint> points, double firstLimit, double secondLimit)
{
    double lower = detail::javaMin(firstLimit, secondLimit);
    double upper = detail::javaMax(firstLimit, secondLimit);
    const double margin = (upper - lower) / 200000;
    if (margin > 0)
    {
        lower -= margin;
        upper += margin;
    }
    std::vector<PlotPoint> used;
    for (const PlotPoint& point : points)
    {
        if (point.x >= lower && point.x <= upper && !std::isnan(point.y))
        {
            used.push_back(point);
        }
    }
    return trapezoidArea(used);
}

AreaMeasurement functionArea(const ScalarFunction& function, double firstLimit, double secondLimit, std::size_t intervals)
{
    const double lower = detail::javaMin(firstLimit, secondLimit);
    const double upper = detail::javaMax(firstLimit, secondLimit);
    if (intervals == 0 || !std::isfinite(lower) || !std::isfinite(upper))
    {
        return {};
    }
    const double step = (upper - lower) / static_cast<double>(intervals);
    std::vector<PlotPoint> samples;
    samples.reserve(intervals + 1);
    for (std::size_t i = 0; i <= intervals; ++i)
    {
        const double x = i == intervals ? upper : lower + static_cast<double>(i) * step;
        const double y = function(x);
        if (!std::isnan(y))
        {
            samples.push_back({.x = x, .y = y});
        }
    }
    return trapezoidArea(samples);
}

} // namespace motrack::science
