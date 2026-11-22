#include "science/sampling/line_profile.hpp"

#include "science/sampling/java_path.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace motrack::science
{

namespace
{

bool finite(ImagePoint point) noexcept
{
    return std::isfinite(point.x) && std::isfinite(point.y);
}

LineProfileData withStatus(LineProfileStatus status)
{
    LineProfileData data;
    data.status = status;
    return data;
}

LineProfileSample sample(int index, ImagePoint position, std::array<double, 3> means, double pixels) noexcept
{
    return {.index = index,
            .position = position,
            .red = means[0],
            .green = means[1],
            .blue = means[2],
            .luma = trackerLuma(means[0], means[1], means[2]),
            .pixels = pixels};
}

// ---------------------------------------------------------------------------------------------
// Horizontal profiles: LineProfileStep.getHorizontalProfileData.

LineProfileData sampleHorizontal(const ArgbImage& image, ImagePoint end0, ImagePoint end1, int spread)
{
    const int first = detail::javaIntCast(end0.x);
    const int second = detail::javaIntCast(end1.x);
    const std::int64_t x0 = std::max(std::min(first, second), 0);
    const std::int64_t x1 = std::min<std::int64_t>(std::max(first, second), image.width());
    const std::int64_t length = x1 - x0;
    if (length <= 0)
    {
        return withStatus(first == second ? LineProfileStatus::TooShort : LineProfileStatus::OutsideImage);
    }
    const int width = 1 + (2 * spread);
    const int y = detail::javaIntCast(end0.y);
    const std::int64_t y0 = static_cast<std::int64_t>(y) - spread;
    // Raster.getDataElements(x0, y0, length, width) throws outside the image; Tracker returns null.
    if (y0 < 0 || y0 + width > image.height())
    {
        return withStatus(LineProfileStatus::OutsideImage);
    }
    LineProfileData data;
    data.status = LineProfileStatus::Measured;
    data.trackerOmitsBlackStrip = true;
    data.samples.reserve(static_cast<std::size_t>(length));
    for (std::int64_t i = 0; i < length; ++i)
    {
        const auto column = static_cast<std::size_t>(x0 + i);
        std::array<double, 3> sums{};
        double total = 0.0;
        for (int j = 0; j < width; ++j)
        {
            const std::uint32_t pixel = image.row(static_cast<int>(y0 + j))[column];
            data.trackerOmitsBlackStrip = data.trackerOmitsBlackStrip && (pixel & 0xffffffU) == 0;
            sums[0] += redOf(pixel);
            sums[1] += greenOf(pixel);
            sums[2] += blueOf(pixel);
            total += 1.0;
        }
        const ImagePoint centre{.x = static_cast<double>(x0 + i) + 0.5, .y = static_cast<double>(y) + 0.5};
        data.samples.push_back(sample(static_cast<int>(i), centre, {sums[0] / total, sums[1] / total, sums[2] / total}, total));
    }
    return data;
}

// ---------------------------------------------------------------------------------------------
// Tilted profiles: LineProfileStep.getTiltedProfileData stepping, with exact area weights.

/// A convex polygon small enough for a quadrilateral clipped by four axis-aligned half-planes.
struct ClipPolygon
{
    std::array<ImagePoint, 12> points{};
    std::size_t size = 0;
};

/// Sutherland-Hodgman against `value <= bound` (below) or `value >= bound` on one axis.
ClipPolygon clip(const ClipPolygon& input, bool xAxis, double bound, bool keepBelow) noexcept
{
    ClipPolygon output;
    const auto coordinate = [xAxis](ImagePoint point)
    {
        return xAxis ? point.x : point.y;
    };
    const auto inside = [&](ImagePoint point)
    {
        return keepBelow ? coordinate(point) <= bound : coordinate(point) >= bound;
    };
    for (std::size_t i = 0; i < input.size; ++i)
    {
        const ImagePoint current = input.points[i];
        const ImagePoint next = input.points[(i + 1) % input.size];
        const bool currentInside = inside(current);
        if (currentInside)
        {
            output.points[output.size++] = current;
        }
        if (currentInside != inside(next))
        {
            const double t = (bound - coordinate(current)) / (coordinate(next) - coordinate(current));
            ImagePoint crossing{.x = current.x + (t * (next.x - current.x)), .y = current.y + (t * (next.y - current.y))};
            (xAxis ? crossing.x : crossing.y) = bound;
            output.points[output.size++] = crossing;
        }
    }
    return output;
}

double area(const ClipPolygon& polygon) noexcept
{
    double twice = 0.0;
    for (std::size_t i = 0; i < polygon.size; ++i)
    {
        const ImagePoint current = polygon.points[i];
        const ImagePoint next = polygon.points[(i + 1) % polygon.size];
        twice += (current.x * next.y) - (next.x * current.y);
    }
    return std::abs(twice) / 2.0;
}

bool insideImage(const ArgbImage& image, ImagePoint point) noexcept
{
    // new Rectangle(width, height).contains(point): x in [0, width), y in [0, height).
    return point.x >= 0.0 && point.y >= 0.0 && point.x < image.width() && point.y < image.height();
}

void advance(std::array<ImagePoint, 2>& corners, double cos, double sin) noexcept
{
    for (ImagePoint& corner : corners)
    {
        corner.x += cos;
        corner.y += sin;
    }
}

void retreat(std::array<ImagePoint, 2>& corners, double cos, double sin) noexcept
{
    for (ImagePoint& corner : corners)
    {
        corner.x -= cos;
        corner.y -= sin;
    }
}

LineProfileData sampleTilted(const ArgbImage& image, ImagePoint end0, ImagePoint end1, int spread)
{
    // Point2D.distance, TPoint.angle (Math.atan2) and Math.cos/sin.
    const double px = end1.x - end0.x;
    const double py = end1.y - end0.y;
    const double length = std::sqrt((px * px) + (py * py));
    if (length < 1)
    {
        return withStatus(LineProfileStatus::TooShort);
    }
    if (length > LINE_PROFILE_MAX_LENGTH)
    {
        return withStatus(LineProfileStatus::InvalidGeometry);
    }
    const int width = 1 + (2 * spread);
    auto count = static_cast<std::int64_t>(std::floor(length));
    const double theta = std::atan2(end1.y - end0.y, end1.x - end0.x);
    const double cos = std::cos(theta);
    const double sin = std::sin(theta);
    const double dx = static_cast<double>(width) * sin / 2.0;
    const double dy = static_cast<double>(width) * cos / 2.0;
    // Step the start corners forward, then the far corners back, until both lie inside the image.
    std::array<ImagePoint, 2> start = {ImagePoint{.x = end0.x - dx, .y = end0.y + dy}, ImagePoint{.x = end0.x + dx, .y = end0.y - dy}};
    while (count > 0 && (!insideImage(image, start[0]) || !insideImage(image, start[1])))
    {
        --count;
        advance(start, cos, sin);
    }
    std::array<ImagePoint, 2> far = {ImagePoint{.x = end1.x - dx, .y = end1.y + dy}, ImagePoint{.x = end1.x + dx, .y = end1.y - dy}};
    while (count > 0 && (!insideImage(image, far[0]) || !insideImage(image, far[1])))
    {
        --count;
        retreat(far, cos, sin);
    }
    if (count < 1)
    {
        return withStatus(LineProfileStatus::OutsideImage);
    }
    LineProfileData data;
    data.status = LineProfileStatus::Measured;
    data.samples.reserve(static_cast<std::size_t>(count));
    std::array<ImagePoint, 2> trailing = start;
    for (std::int64_t i = 0; i < count; ++i)
    {
        std::array<ImagePoint, 2> leading = trailing;
        advance(leading, cos, sin);
        const std::array<ImagePoint, 4> quad = {trailing[0], trailing[1], leading[1], leading[0]};
        double xMin = quad[0].x;
        double xMax = quad[0].x;
        double yMin = quad[0].y;
        double yMax = quad[0].y;
        for (const ImagePoint& corner : quad)
        {
            xMin = std::min(xMin, corner.x);
            xMax = std::max(xMax, corner.x);
            yMin = std::min(yMin, corner.y);
            yMax = std::max(yMax, corner.y);
        }
        // Tracker reports the centre of the corners' bounding box.
        const ImagePoint centre{.x = (xMax + xMin) / 2, .y = (yMax + yMin) / 2};
        // Clip in coordinates relative to the first cell; subtracting the integer is exact.
        const double column0 = std::floor(xMin);
        const double row0 = std::floor(yMin);
        ClipPolygon probe;
        for (const ImagePoint& corner : quad)
        {
            probe.points[probe.size++] = {.x = corner.x - column0, .y = corner.y - row0};
        }
        // The stepping keeps the corners inside the image; accumulated rounding can still leave a
        // sliver past an edge, which has no pixel and therefore no weight.
        const int firstColumn = std::max(static_cast<int>(column0), 0);
        const int endColumn = std::min(static_cast<int>(std::ceil(xMax)), image.width());
        const int firstRow = std::max(static_cast<int>(row0), 0);
        const int endRow = std::min(static_cast<int>(std::ceil(yMax)), image.height());
        double weight = 0.0;
        std::array<double, 3> sums{};
        for (int row = firstRow; row < endRow; ++row)
        {
            const double r = row - row0;
            const ClipPolygon band = clip(clip(probe, false, r, false), false, r + 1, true);
            const auto pixels = image.row(row);
            for (int column = firstColumn; column < endColumn; ++column)
            {
                const double c = column - column0;
                const double a = area(clip(clip(band, true, c, false), true, c + 1, true));
                const std::uint32_t pixel = pixels[static_cast<std::size_t>(column)];
                sums[0] += a * redOf(pixel);
                sums[1] += a * greenOf(pixel);
                sums[2] += a * blueOf(pixel);
                weight += a;
            }
        }
        // The stepping keeps every probe inside the image, so its weight is its area, about `width`.
        data.samples.push_back(sample(static_cast<int>(i), centre, {sums[0] / weight, sums[1] / weight, sums[2] / weight}, width));
        trailing = leading;
    }
    return data;
}

} // namespace

int clampLineProfileSpread(int spread) noexcept
{
    return std::min(std::max(spread, 0), LINE_PROFILE_MAX_SPREAD);
}

LineProfileMode lineProfileMode(bool alwaysHorizontal, double axisAngle) noexcept
{
    return alwaysHorizontal || std::abs(std::sin(axisAngle)) < LINE_PROFILE_HORIZONTAL_SINE ? LineProfileMode::Horizontal
                                                                                            : LineProfileMode::Tilted;
}

LineProfileData sampleLineProfile(const ArgbImage& image, ImagePoint end0, ImagePoint end1, int spread, LineProfileMode mode)
{
    if (!finite(end0) || !finite(end1) || spread < 0 || spread > LINE_PROFILE_MAX_SPREAD)
    {
        return withStatus(LineProfileStatus::InvalidGeometry);
    }
    return mode == LineProfileMode::Horizontal ? sampleHorizontal(image, end0, end1, spread) : sampleTilted(image, end0, end1, spread);
}

} // namespace motrack::science
