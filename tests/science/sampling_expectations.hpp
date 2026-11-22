#pragma once

// Independent expectations for the sampling kernels. Nothing here calls the kernel's area code:
// probe/pixel overlaps are integrated slice by slice (an exact method unrelated to the kernel's
// polygon clipping), and frames are generated from closed-form pixel formulas.

#include "science/sampling/argb_image.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace motrack::science::expectations
{

/// Builds an image from `pixel(x, y)` returning 0x00RRGGBB (alpha bytes are set to 0xff).
template <typename Function> ArgbImage makeImage(int width, int height, Function pixel)
{
    auto created = ArgbImage::filled(width, height, 0xff000000U);
    if (!created)
    {
        return {};
    }
    ArgbImage image = *std::move(created);
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            image.setPixel(x, y, 0xff000000U | pixel(x, y));
        }
    }
    return image;
}

constexpr std::uint32_t rgb(int red, int green, int blue) noexcept
{
    return (static_cast<std::uint32_t>(red) << 16U) | (static_cast<std::uint32_t>(green) << 8U) | static_cast<std::uint32_t>(blue);
}

/// The corners of a probe one pixel long (along `theta`) and `width` pixels wide around `centre`.
inline std::array<ImagePoint, 4> probeCorners(ImagePoint centre, double theta, int width)
{
    const double ux = std::cos(theta) / 2;
    const double uy = std::sin(theta) / 2;
    const double nx = -std::sin(theta) * width / 2;
    const double ny = std::cos(theta) * width / 2;
    return {ImagePoint{.x = centre.x - ux + nx, .y = centre.y - uy + ny},
            ImagePoint{.x = centre.x - ux - nx, .y = centre.y - uy - ny},
            ImagePoint{.x = centre.x + ux - nx, .y = centre.y + uy - ny},
            ImagePoint{.x = centre.x + ux + nx, .y = centre.y + uy + ny}};
}

/// Exact area of a convex quadrilateral inside the pixel [column, column + 1] x [row, row + 1].
///
/// The vertical extent of the quadrilateral at x, clamped to the pixel row, is piecewise linear in
/// x. Its breakpoints are corner abscissae and the abscissae where an edge crosses y = row or
/// y = row + 1, so the midpoint rule is exact on every interval between consecutive breakpoints.
inline double overlapBySlices(const std::array<ImagePoint, 4>& quad, int column, int row)
{
    const double top = row;
    const double bottom = row + 1.0;
    std::vector<double> breaks = {static_cast<double>(column), column + 1.0};
    for (std::size_t i = 0; i < quad.size(); ++i)
    {
        const ImagePoint a = quad[i];
        const ImagePoint b = quad[(i + 1) % quad.size()];
        breaks.push_back(a.x);
        if (a.x != b.x)
        {
            for (double level : {top, bottom})
            {
                if ((a.y - level) * (b.y - level) < 0)
                {
                    breaks.push_back(a.x + ((level - a.y) * (b.x - a.x) / (b.y - a.y)));
                }
            }
        }
    }
    std::ranges::sort(breaks);
    double area = 0.0;
    for (std::size_t k = 0; k + 1 < breaks.size(); ++k)
    {
        const double left = std::max(breaks[k], static_cast<double>(column));
        const double right = std::min(breaks[k + 1], column + 1.0);
        if (right <= left)
        {
            continue;
        }
        const double x = (left + right) / 2;
        double low = std::numeric_limits<double>::infinity();
        double high = -std::numeric_limits<double>::infinity();
        for (std::size_t i = 0; i < quad.size(); ++i)
        {
            const ImagePoint a = quad[i];
            const ImagePoint b = quad[(i + 1) % quad.size()];
            if (a.x == b.x || x < std::min(a.x, b.x) || x > std::max(a.x, b.x))
            {
                continue;
            }
            const double y = a.y + ((x - a.x) * (b.y - a.y) / (b.x - a.x));
            low = std::min(low, y);
            high = std::max(high, y);
        }
        if (low <= high)
        {
            area += (right - left) * std::max(0.0, std::min(high, bottom) - std::max(low, top));
        }
    }
    return area;
}

/// Area-weighted channel means of `image` under a convex quadrilateral (by slice integration).
inline std::array<double, 3> exactMean(const ArgbImage& image, const std::array<ImagePoint, 4>& quad)
{
    double xMin = quad[0].x;
    double xMax = quad[0].x;
    double yMin = quad[0].y;
    double yMax = quad[0].y;
    for (const ImagePoint& p : quad)
    {
        xMin = std::min(xMin, p.x);
        xMax = std::max(xMax, p.x);
        yMin = std::min(yMin, p.y);
        yMax = std::max(yMax, p.y);
    }
    std::array<double, 3> sums{};
    double total = 0.0;
    for (int row = static_cast<int>(std::floor(yMin)); row < static_cast<int>(std::ceil(yMax)); ++row)
    {
        for (int column = static_cast<int>(std::floor(xMin)); column < static_cast<int>(std::ceil(xMax)); ++column)
        {
            if (!image.contains(column, row))
            {
                continue;
            }
            const double a = overlapBySlices(quad, column, row);
            const std::uint32_t pixel = image.pixel(column, row);
            sums[0] += a * redOf(pixel);
            sums[1] += a * greenOf(pixel);
            sums[2] += a * blueOf(pixel);
            total += a;
        }
    }
    return {sums[0] / total, sums[1] / total, sums[2] / total};
}

/// Midpoint-lattice estimate of the same mean, `n` x `n` points in the probe's own frame. It
/// checks overlapBySlices itself; each pixel edge crossing the probe misclassifies at most about n
/// of the n^2 points, and in practice the estimate agrees to a few hundredths for n = 512.
inline std::array<double, 3> latticeMean(const ArgbImage& image, const std::array<ImagePoint, 4>& quad, int n)
{
    std::array<double, 3> sums{};
    for (int i = 0; i < n; ++i)
    {
        for (int j = 0; j < n; ++j)
        {
            const double s = (i + 0.5) / n;
            const double t = (j + 0.5) / n;
            const double x = quad[0].x + (s * (quad[3].x - quad[0].x)) + (t * (quad[1].x - quad[0].x));
            const double y = quad[0].y + (s * (quad[3].y - quad[0].y)) + (t * (quad[1].y - quad[0].y));
            const std::uint32_t pixel = image.pixel(static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y)));
            sums[0] += redOf(pixel);
            sums[1] += greenOf(pixel);
            sums[2] += blueOf(pixel);
        }
    }
    const double count = static_cast<double>(n) * n;
    return {sums[0] / count, sums[1] / count, sums[2] / count};
}

} // namespace motrack::science::expectations
