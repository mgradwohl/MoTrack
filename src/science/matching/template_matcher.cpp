#include "science/matching/template_matcher.hpp"

#include "science/matching/matching_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace motrack::science
{
namespace
{

using matching_detail::fitParabola;

constexpr double NOT_A_NUMBER = std::numeric_limits<double>::quiet_NaN();
constexpr ArgbPixel OPAQUE_BLACK = 0xff000000U;
constexpr ArgbPixel RGB_MASK = 0x00ffffffU;
constexpr int OPAQUE = 0xff;
// TemplateMatcher.largeNumber: larger than any squared difference sum.
constexpr double LARGE_DIFFERENCE = 1.0E20;
constexpr std::size_t LINE_POINTS_PER_CANCEL_CHECK = 64;

std::size_t pixelCount(int width, int height)
{
    if (width < 0 || height < 0 || static_cast<std::int64_t>(width) * static_cast<std::int64_t>(height) > std::numeric_limits<int>::max())
    {
        throw std::invalid_argument("ArgbImage dimensions must be non-negative and hold at most INT_MAX pixels");
    }
    return static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
}

bool cancelled(const MatchOptions& options)
{
    return options.cancelled && options.cancelled();
}

// AutoTracker.newVideoImage(rect) followed by ensureType(TYPE_INT_ARGB): the frame's RGB drawn into a
// black TYPE_INT_RGB image of the rect's size, then made opaque.
ArgbImage opaqueCrop(const ArgbImage& frame, PixelRect rect)
{
    ArgbImage crop(rect.width, rect.height, OPAQUE_BLACK);
    for (int row = 0; row < rect.height; ++row)
    {
        const std::int64_t y = static_cast<std::int64_t>(rect.y) + row;
        if (y < 0 || y >= frame.height())
        {
            continue;
        }
        for (int column = 0; column < rect.width; ++column)
        {
            const std::int64_t x = static_cast<std::int64_t>(rect.x) + column;
            if (x >= 0 && x < frame.width())
            {
                crop.setPixel(column, row, OPAQUE_BLACK | (frame.pixel(static_cast<int>(x), static_cast<int>(y)) & RGB_MASK));
            }
        }
    }
    return crop;
}

// Graphics2D.drawImage(source, 0, 0) onto destination with AlphaComposite(SRC_OVER, alpha / 255).
void composite(ArgbImage& destination, const ArgbImage& source, int alpha)
{
    const int extraAlpha = matching_detail::compositeExtraAlpha(alpha);
    auto target = destination.pixels();
    const auto overlay = source.pixels();
    for (std::size_t index = 0; index < target.size(); ++index)
    {
        target[index] = matching_detail::srcOver(overlay[index], target[index], extraAlpha);
    }
}

int red(ArgbPixel pixel)
{
    return static_cast<int>((pixel >> 16U) & 0xffU);
}

int green(ArgbPixel pixel)
{
    return static_cast<int>((pixel >> 8U) & 0xffU);
}

int blue(ArgbPixel pixel)
{
    return static_cast<int>(pixel & 0xffU);
}

// TemplateMatcher.getRGBDiffSquaredAtTestPoint, evaluated on the frame itself: the sum over included
// template pixels of the squared red, green and blue differences with the trimmed template's top-left
// pixel at (left, top); NaN when the template does not fit inside the frame there.
double difference(const MatchTemplate& matchTemplate, const ArgbImage& frame, std::int64_t left, std::int64_t top)
{
    const int width = matchTemplate.image.width();
    const int height = matchTemplate.image.height();
    if (left < 0 || top < 0 || left + width > frame.width() || top + height > frame.height())
    {
        return NOT_A_NUMBER;
    }
    const auto templatePixels = matchTemplate.image.pixels();
    const auto framePixels = frame.pixels();
    std::int64_t total = 0;
    for (int row = 0; row < height; ++row)
    {
        const auto frameRow =
            framePixels.subspan(static_cast<std::size_t>((top + row) * frame.width() + left), static_cast<std::size_t>(width));
        const auto templateRow =
            templatePixels.subspan(static_cast<std::size_t>(row) * static_cast<std::size_t>(width), static_cast<std::size_t>(width));
        for (int column = 0; column < width; ++column)
        {
            const ArgbPixel model = templateRow[static_cast<std::size_t>(column)];
            if ((model >> 24U) == 0)
            {
                continue; // isPixelTransparent
            }
            const ArgbPixel pixel = frameRow[static_cast<std::size_t>(column)];
            const int dr = red(model) - red(pixel);
            const int dg = green(model) - green(pixel);
            const int db = blue(model) - blue(pixel);
            total += dr * dr + dg * dg + db * db;
        }
    }
    // Java sums the same integers in double; every partial sum is exact below 2^53.
    return static_cast<double>(total);
}

// The first lines of getMatchLocation: insets that keep the template inside the frame, then the search
// rectangle moved (not clipped) to respect them and cut at the right and bottom.
struct SearchGeometry
{
    PixelRect area;
    int left = 0;
    int top = 0;
};

SearchGeometry searchGeometry(const MatchTemplate& matchTemplate, const ArgbImage& frame, PixelRect searchArea)
{
    const int templateWidth = matchTemplate.image.width();
    const int templateHeight = matchTemplate.image.height();
    const int left = templateWidth / 2;
    const int top = templateHeight / 2;
    const int right = left + (templateWidth % 2);
    const int bottom = top + (templateHeight % 2);
    const int x = std::max(left, std::min(frame.width() - right, searchArea.x));
    const int y = std::max(top, std::min(frame.height() - bottom, searchArea.y));
    const int width = std::min(frame.width() - x - right, searchArea.width);
    const int height = std::min(frame.height() - y - bottom, searchArea.height);
    return {.area = {.x = x, .y = y, .width = width, .height = height}, .left = left, .top = top};
}

MatchResult unmatched(MatchStatus status, PixelRect area)
{
    MatchResult result;
    result.status = status;
    result.searchArea = area;
    return result;
}

struct PeakFit
{
    double dx = 0.0;
    double dy = 0.0;
    double width = NOT_A_NUMBER;
};

// The vertex offset -c1 / (2 c2) of a parabola fitted through a position and its two neighbors. Java
// only replaces NaN by 0. MoTrack also uses 0 unless the parabola opens upward and its vertex lies
// within the neighbors: otherwise the "peak" is a maximum or an extrapolation beyond the sampled
// positions, which Tracker reports unbounded (even infinite) at the edge of a search area. For a best
// position that is a local minimum both conditions always hold, so interior matches equal Tracker's.
double vertexOffset(const matching_detail::Parabola& parabola)
{
    const double offset = -parabola.c1 / (2 * parabola.c2);
    return std::isnan(offset) || !(parabola.c2 > 0) || std::abs(offset) > 1 ? 0.0 : offset;
}

// The two-dimensional branch of getMatchLocation's subpixel fit around the best test position, whose
// trimmed-template top-left pixel is (left, top). Java reads neighbors from a copy of the search
// rectangle that has no column left of it or row above it: at the first column its left neighbor wraps
// to the end of the previous row, and at the first row its upper neighbor is NaN. MoTrack scores all
// four neighbors in the frame (NaN only where the template leaves the frame).
PeakFit fitPeak2d(const MatchTemplate& matchTemplate, const ArgbImage& frame, std::int64_t left, std::int64_t top, double minimum)
{
    const double west = difference(matchTemplate, frame, left - 1, top);
    const double east = difference(matchTemplate, frame, left + 1, top);
    const double north = difference(matchTemplate, frame, left, top - 1);
    const double south = difference(matchTemplate, frame, left, top + 1);
    PeakFit fit;
    const auto horizontal = fitParabola(west, minimum, east);
    fit.dx = vertexOffset(horizontal);
    fit.width = std::sqrt(2 * horizontal.c0 / horizontal.c2);
    const auto vertical = fitParabola(north, minimum, south);
    fit.dy = vertexOffset(vertical);
    fit.width = 0.5 * (fit.width + std::sqrt(2 * vertical.c0 / vertical.c2));
    return fit;
}

MatchResult finish(MatchResult result,
                   const MatchTemplate& matchTemplate,
                   PixelPoint origin,
                   PixelPoint best,
                   double peakHeight,
                   const PeakFit& fit,
                   bool atSearchEdge)
{
    result.atSearchEdge = atSearchEdge;
    // Tracker divides the score of a wide (poorly localized) peak by its width.
    if (!std::isnan(fit.width) && fit.width > 1)
    {
        peakHeight /= fit.width;
    }
    result.status = MatchStatus::Matched;
    result.pixel = {.x = origin.x + best.x - matchTemplate.trimLeft, .y = origin.y + best.y - matchTemplate.trimTop};
    result.x = static_cast<double>(result.pixel.x) + fit.dx;
    result.y = static_cast<double>(result.pixel.y) + fit.dy;
    result.peakHeight = peakHeight;
    result.peakWidth = fit.width;
    return result;
}

} // namespace

ArgbImage::ArgbImage(int width, int height, ArgbPixel fill) : m_Width(width), m_Height(height), m_Pixels(pixelCount(width, height), fill)
{}

ArgbImage::ArgbImage(int width, int height, std::vector<ArgbPixel> pixels) : m_Width(width), m_Height(height), m_Pixels(std::move(pixels))
{
    if (m_Pixels.size() != pixelCount(width, height))
    {
        throw std::invalid_argument("ArgbImage pixel count must equal width * height");
    }
}

int ArgbImage::width() const noexcept
{
    return m_Width;
}

int ArgbImage::height() const noexcept
{
    return m_Height;
}

bool ArgbImage::empty() const noexcept
{
    return m_Pixels.empty();
}

ArgbPixel ArgbImage::pixel(int x, int y) const noexcept
{
    return m_Pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(m_Width) + static_cast<std::size_t>(x)];
}

void ArgbImage::setPixel(int x, int y, ArgbPixel value) noexcept
{
    m_Pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(m_Width) + static_cast<std::size_t>(x)] = value;
}

std::span<const ArgbPixel> ArgbImage::pixels() const noexcept
{
    return m_Pixels;
}

std::span<ArgbPixel> ArgbImage::pixels() noexcept
{
    return m_Pixels;
}

std::optional<TemplateMatcher> TemplateMatcher::create(const ArgbImage& frame, const MatchMask& mask)
{
    const auto bounds = maskBounds(mask);
    if (!bounds || bounds->width <= 0 || bounds->height <= 0 ||
        static_cast<std::int64_t>(bounds->width) * bounds->height > MAX_TEMPLATE_PIXELS)
    {
        return std::nullopt;
    }
    auto included = maskPixels(mask);
    // Tracker trims the template to the opaque pixels. Included pixels stay opaque through every build
    // (SrcOver onto an opaque pixel is opaque) and excluded ones are cleared, so the trim is the bounding
    // box of the included pixels. Java fails for a mask that includes nothing; MoTrack refuses it.
    int minX = bounds->width;
    int minY = bounds->height;
    int maxX = -1;
    int maxY = -1;
    for (int y = 0; y < bounds->height; ++y)
    {
        for (int x = 0; x < bounds->width; ++x)
        {
            if (included[static_cast<std::size_t>(y) * static_cast<std::size_t>(bounds->width) + static_cast<std::size_t>(x)] != 0)
            {
                minX = std::min(minX, x);
                minY = std::min(minY, y);
                maxX = std::max(maxX, x);
                maxY = std::max(maxY, y);
            }
        }
    }
    if (maxX < 0)
    {
        return std::nullopt;
    }
    TemplateMatcher matcher;
    matcher.m_Mask = mask;
    matcher.m_Bounds = *bounds;
    matcher.m_Trimmed = {.x = minX, .y = minY, .width = maxX - minX + 1, .height = maxY - minY + 1};
    matcher.m_Included = std::move(included);
    matcher.m_Original = opaqueCrop(frame, *bounds);
    // The constructor's buildTemplate(original, 255, 0) onto a new, transparent working image.
    matcher.m_Working = ArgbImage(bounds->width, bounds->height, 0);
    composite(matcher.m_Working, matcher.m_Original, OPAQUE);
    matcher.rebuild();
    return matcher;
}

const MatchMask& TemplateMatcher::mask() const noexcept
{
    return m_Mask;
}

PixelRect TemplateMatcher::bounds() const noexcept
{
    return m_Bounds;
}

const MatchTemplate& TemplateMatcher::matchTemplate() const noexcept
{
    return m_Template;
}

const ArgbImage& TemplateMatcher::original() const noexcept
{
    return m_Original;
}

const ArgbImage& TemplateMatcher::working() const noexcept
{
    return m_Working;
}

std::array<int, 2> TemplateMatcher::alphas() const noexcept
{
    return m_Alphas;
}

bool TemplateMatcher::evolve(const ArgbImage& frame, PixelPoint origin, int alphaInput, int alphaOriginal)
{
    if (alphaInput == 0 && alphaOriginal == 0)
    {
        return false;
    }
    m_Alphas = {alphaInput, alphaOriginal};
    const auto input = opaqueCrop(frame, {.x = origin.x, .y = origin.y, .width = m_Bounds.width, .height = m_Bounds.height});
    if (const int alpha = std::clamp(alphaInput, 0, OPAQUE); alpha > 0)
    {
        composite(m_Working, input, alpha);
    }
    if (const int alpha = std::clamp(alphaOriginal, 0, OPAQUE); alpha > 0)
    {
        composite(m_Working, m_Original, alpha);
    }
    rebuild();
    return true;
}

void TemplateMatcher::rebuild()
{
    // Clear excluded working pixels (alpha 0), then cut the trimmed template out of the working image.
    auto working = m_Working.pixels();
    for (std::size_t index = 0; index < working.size(); ++index)
    {
        if (m_Included[index] == 0)
        {
            working[index] = 0;
        }
    }
    ArgbImage image(m_Trimmed.width, m_Trimmed.height);
    for (int y = 0; y < m_Trimmed.height; ++y)
    {
        for (int x = 0; x < m_Trimmed.width; ++x)
        {
            image.setPixel(x, y, m_Working.pixel(m_Trimmed.x + x, m_Trimmed.y + y));
        }
    }
    m_Template = {.image = std::move(image), .trimLeft = m_Trimmed.x, .trimTop = m_Trimmed.y};
}

MatchResult findMatch(const MatchTemplate& matchTemplate, const ArgbImage& frame, PixelRect searchArea, const MatchOptions& options)
{
    const auto geometry = searchGeometry(matchTemplate, frame, searchArea);
    const auto& area = geometry.area;
    if (matchTemplate.image.empty() || area.width <= 0 || area.height <= 0)
    {
        return unmatched(MatchStatus::OutsideImage, area);
    }
    // Test position (x, y) puts the trimmed template's top-left pixel at origin + (x, y). Tracker visits
    // positions column by column and keeps the first strict minimum.
    const PixelPoint origin{.x = area.x - geometry.left, .y = area.y - geometry.top};
    double minimum = LARGE_DIFFERENCE;
    PixelPoint best;
    double sum = 0;
    for (int x = 0; x < area.width; ++x)
    {
        if (cancelled(options))
        {
            return unmatched(MatchStatus::Cancelled, area);
        }
        for (int y = 0; y < area.height; ++y)
        {
            const double value =
                difference(matchTemplate, frame, static_cast<std::int64_t>(origin.x) + x, static_cast<std::int64_t>(origin.y) + y);
            sum += value;
            if (value < minimum)
            {
                minimum = value;
                best = {.x = x, .y = y};
            }
        }
    }
    const double mean = sum / (static_cast<double>(area.width) * static_cast<double>(area.height));
    const double peakHeight = mean / minimum - 1;
    PeakFit fit;
    if (!std::isinf(peakHeight))
    {
        fit = fitPeak2d(
            matchTemplate, frame, static_cast<std::int64_t>(origin.x) + best.x, static_cast<std::int64_t>(origin.y) + best.y, minimum);
    }
    MatchResult result;
    result.searchArea = area;
    const bool edge = best.x == 0 || best.y == 0 || best.x + 1 == area.width || best.y + 1 == area.height;
    return finish(result, matchTemplate, origin, best, peakHeight, fit, edge);
}

MatchResult findMatchAlongLine(
    const MatchTemplate& matchTemplate, const ArgbImage& frame, PixelRect searchArea, const SearchLine& line, const MatchOptions& options)
{
    const auto geometry = searchGeometry(matchTemplate, frame, searchArea);
    const auto& area = geometry.area;
    if (matchTemplate.image.empty() || area.width <= 0 || area.height <= 0)
    {
        return unmatched(MatchStatus::OutsideImage, area);
    }
    // Tracker computes the line pixels for the unadjusted rectangle but reads them relative to the
    // adjusted one; MoTrack computes them for the rectangle it searches.
    const auto points = lineSearchPoints(area, line);
    if (!points)
    {
        return unmatched(MatchStatus::NoSearchPoints, area);
    }
    const PixelPoint origin{.x = area.x - geometry.left, .y = area.y - geometry.top};
    const auto score = [&](PixelPoint point)
    {
        return difference(
            matchTemplate, frame, static_cast<std::int64_t>(origin.x) + point.x, static_cast<std::int64_t>(origin.y) + point.y);
    };
    double minimum = LARGE_DIFFERENCE;
    std::optional<std::size_t> best;
    std::optional<std::size_t> first;
    std::size_t last = 0;
    double sum = 0;
    std::int64_t searched = 0;
    for (std::size_t index = 0; index < points->size(); ++index)
    {
        if (index % LINE_POINTS_PER_CANCEL_CHECK == 0 && cancelled(options))
        {
            return unmatched(MatchStatus::Cancelled, area);
        }
        const auto point = (*points)[index];
        if (point.x >= area.width || point.y >= area.height || point.x < 0 || point.y < 0)
        {
            continue;
        }
        const double value = score(point);
        sum += value;
        ++searched;
        first = first.value_or(index);
        last = index;
        if (value < minimum)
        {
            minimum = value;
            best = index;
        }
    }
    // Tracker indexes past the array (and fails) when no line pixel lies in the area.
    if (!best)
    {
        return unmatched(MatchStatus::NoSearchPoints, area);
    }
    const double peakHeight = sum / static_cast<double>(searched) / minimum - 1;
    const auto bestPoint = (*points)[*best];
    PeakFit fit;
    if (!std::isinf(peakHeight))
    {
        // Tracker fits a parabola in point-index units through the neighboring line pixels and moves
        // by offset x (cos angle, sin angle). Along the line that is offset x sign(cos angle) x
        // cos(2 angle): reversed for axes pointing left or steeper than 45 degrees, zero at 45 degrees,
        // and NaN is kept. Index units are also uneven: consecutive line pixels of a tilted line lie
        // between sin and cos of the angle apart along it. MoTrack estimates the peak in two dimensions,
        // as Tracker does at either end of the line, and keeps its component along the line; for a
        // horizontal line that is Tracker's offset bit for bit.
        const auto planar = fitPeak2d(matchTemplate,
                                      frame,
                                      static_cast<std::int64_t>(origin.x) + bestPoint.x,
                                      static_cast<std::int64_t>(origin.y) + bestPoint.y,
                                      minimum);
        fit.width = planar.width;
        if (*best > 0 && *best + 1 < points->size())
        {
            // The score keeps Tracker's width of the fit along the line pixels.
            const auto parabola = fitParabola(score((*points)[*best - 1]), minimum, score((*points)[*best + 1]));
            fit.width = std::sqrt(2 * parabola.c0 / parabola.c2);
        }
        const double ux = std::cos(line.angle);
        const double uy = -std::sin(line.angle);
        const double along = planar.dx * ux + planar.dy * uy;
        fit.dx = along * ux;
        fit.dy = along * uy;
    }
    MatchResult result;
    result.searchArea = area;
    return finish(result, matchTemplate, origin, bestPoint, peakHeight, fit, *best == first || *best == last);
}

std::array<double, 2> projectOntoLine(const SearchLine& line, double x, double y) noexcept
{
    // AutoTracker.findMatchTarget: distance to the line through (x0, y0) and the point one unit along
    // the axis, then a perpendicular move by that distance.
    const double x0 = line.x0;
    const double y0 = line.y0;
    const double x1 = x0 + std::cos(line.angle);
    const double y1 = y0 - std::sin(line.angle);
    const double distance = ((y1 - y0) * x - (x1 - x0) * y + x1 * y0 - x0 * y1) / std::sqrt((y1 - y0) * (y1 - y0) + (x1 - x0) * (x1 - x0));
    return {x + distance * std::sin(line.angle), y + distance * std::cos(line.angle)};
}

MatchQuality classifyMatch(const MatchResult& result, int goodMatch, int possibleMatch) noexcept
{
    if (result.status != MatchStatus::Matched || result.peakHeight < possibleMatch)
    {
        return MatchQuality::None;
    }
    if (std::isinf(result.peakHeight) || result.peakHeight >= goodMatch)
    {
        return MatchQuality::Good;
    }
    return MatchQuality::Possible;
}

int alphaFromPercent(int percent) noexcept
{
    // AutoTracker.Wizard.getAlphaFromPercent: (int) (2.55 * percent), clamped.
    const double alpha = 2.55 * percent;
    if (alpha >= OPAQUE)
    {
        return OPAQUE;
    }
    if (alpha <= 0)
    {
        return 0;
    }
    return static_cast<int>(alpha);
}

PixelPoint evolutionOrigin(const MatchResult& result) noexcept
{
    return {.x = matching_detail::javaRoundToInt(result.x), .y = matching_detail::javaRoundToInt(result.y)};
}

} // namespace motrack::science
