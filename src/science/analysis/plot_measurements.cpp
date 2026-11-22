#include "science/analysis/data_analysis.hpp"
#include "science/java_math.hpp"

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
        firstX = java::min(firstX, point.x);
        lastX = java::max(lastX, point.x);
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
    const double clamped = java::min(visibleMax, java::max(visibleMin, x));
    // OSP takes the nearest point overall and then moves an off-screen choice by one index or to the
    // last index, assuming ascending x, which can land far outside the view (#267). MoTrack
    // takes the nearest visible point and falls back to the nearest point when none is visible.
    std::optional<std::size_t> nearestVisible;
    std::optional<std::size_t> nearestOverall;
    double visibleDistance = std::numeric_limits<double>::max();
    double overallDistance = std::numeric_limits<double>::max();
    for (std::size_t i = 0; i < points.size(); ++i)
    {
        if (std::isnan(points[i].y))
        {
            continue;
        }
        const double distance = std::abs(clamped - points[i].x);
        if (distance < overallDistance)
        {
            overallDistance = distance;
            nearestOverall = i;
        }
        if (points[i].x >= visibleMin && points[i].x <= visibleMax && distance < visibleDistance)
        {
            visibleDistance = distance;
            nearestVisible = i;
        }
    }
    return nearestVisible ? nearestVisible : nearestOverall;
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
    double lower = java::min(firstLimit, secondLimit);
    double upper = java::max(firstLimit, secondLimit);
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
    AreaMeasurement result = trapezoidArea(used);
    if (lower == upper && std::isfinite(lower))
    {
        result.area = 0.0; // a zero-width integral is exactly zero, as OSP shows
    }
    return result;
}

AreaMeasurement functionArea(const ScalarFunction& function, double firstLimit, double secondLimit, std::size_t intervals)
{
    const double lower = java::min(firstLimit, secondLimit);
    const double upper = java::max(firstLimit, secondLimit);
    if (!std::isfinite(lower) || !std::isfinite(upper))
    {
        return {};
    }
    if (lower == upper)
    {
        AreaMeasurement zeroWidth;
        zeroWidth.area = 0.0; // a zero-width integral is exactly zero, as for data
        return zeroWidth;
    }
    if (intervals == 0)
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
