// Geometric filters: resize (Java2D's scaled blit), rotate/reflect, perspective and radial distortion.

#include "science/filters/java_arithmetic.hpp"
#include "science/filters/video_filters.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <ranges>

namespace motrack::science
{

// The filters' Java arithmetic (science/filters/java_arithmetic.hpp).
namespace detail = filters::detail;

namespace
{

constexpr double PI = std::numbers::pi;

std::size_t index(int x, int y, int width) noexcept
{
    return (static_cast<std::size_t>(y) * static_cast<std::size_t>(width)) + static_cast<std::size_t>(x);
}

// ---------------------------------------------------------------------------------------------
// Java2D ScaledBlit.c (OpenJDK 17) for an IntRgb -> IntRgb nearest-neighbour scale from (0, 0).

/// TILESTART(id, io, ts)
std::int32_t tileStart(std::int32_t id, std::int32_t origin, std::int32_t tileSize) noexcept
{
    return origin + ((id - origin) & ~(tileSize - 1));
}

/// SRCLOC(id, fo, sf)
double sourceLocation(std::int32_t id, double origin, double scale) noexcept
{
    return std::ceil((((id + 0.5) - origin) * scale) - 0.5);
}

std::int32_t pow2TileSize(std::int32_t shift, std::int32_t xIncrement, std::int32_t yIncrement) noexcept
{
    const std::int32_t smallest = std::min(xIncrement, yIncrement);
    if (smallest == 0)
    {
        return 1;
    }
    while ((1 << shift) > smallest)
    {
        --shift;
    }
    shift = shift >= 16 ? shift - 8 : shift / 2;
    return 1 << shift;
}

std::int32_t refine(std::int32_t origin,
                    double doubleOrigin,
                    std::int32_t tileSize,
                    double scale,
                    std::int32_t sourceTarget,
                    std::int32_t sourceIncrement) noexcept
{
    std::int32_t destination = detail::toInt(std::ceil(doubleOrigin + (sourceTarget / scale) - 0.5));
    bool wasNegative = false;
    bool wasPositive = false;
    while (true)
    {
        const std::int32_t start = tileStart(destination, origin, tileSize);
        auto location = static_cast<std::int64_t>(sourceLocation(start, doubleOrigin, scale));
        if (destination > start)
        {
            location += static_cast<std::int64_t>(sourceIncrement) * (static_cast<std::int64_t>(destination) - start);
        }
        if (location >= sourceTarget)
        {
            if (wasNegative)
            {
                break;
            }
            --destination;
            wasPositive = true;
        }
        else
        {
            ++destination;
            if (wasPositive)
            {
                break;
            }
            wasNegative = true;
        }
    }
    return destination;
}

struct ScaledBlit
{
    const FilterImage& source;
    FilterImage& destination;
    std::int32_t shift;
    std::int32_t xIncrement;
    std::int32_t yIncrement;

    /// DEFINE_ISOSCALE_BLIT over destination columns [x1, x2) and rows [y1, y2).
    void blit(int x1, int y1, int x2, int y2, std::int64_t xLocation, std::int64_t yLocation) const
    {
        const auto in = source.pixels();
        auto out = destination.pixels();
        for (int y = y1; y < y2; ++y, yLocation += yIncrement)
        {
            const auto row = static_cast<int>(yLocation >> shift);
            std::int64_t x = xLocation;
            for (int column = x1; column < x2; ++column, x += xIncrement)
            {
                out[index(column, y, destination.width())] =
                    OPAQUE_ALPHA | (in[index(static_cast<int>(x >> shift), row, source.width())] & RGB_MASK);
            }
        }
    }
};

void scaleImage(const FilterImage& source, FilterImage& destination, double destinationWidth, double destinationHeight)
{
    const std::int32_t sourceWidth = source.width();
    const std::int32_t sourceHeight = source.height();
    // The most fixed-point precision for which (w | h) << shift stays positive.
    std::int32_t shift = 0;
    for (auto bits = static_cast<std::uint32_t>(sourceWidth | sourceHeight); static_cast<std::int32_t>(bits <<= 1U) > 0;)
    {
        ++shift;
    }
    // Callers guarantee destinationWidth/Height >= 1 (a smaller output has a zero dimension), so the
    // ScaledBlit underflow branches never apply.
    const double scaleX = (static_cast<double>(sourceWidth) / destinationWidth) * (1 << shift);
    const double scaleY = (static_cast<double>(sourceHeight) / destinationHeight) * (1 << shift);
    const std::int32_t xIncrement = detail::toInt(scaleX);
    const std::int32_t yIncrement = detail::toInt(scaleY);
    const std::int32_t tileSize = pow2TileSize(shift, xIncrement, yIncrement);
    // ceil(0 - 0.5) is 0: the destination origin is pixel (0, 0).
    const int x2 = std::min(refine(0, 0.0, tileSize, scaleX, sourceWidth << shift, xIncrement), destination.width());
    const int y2 = std::min(refine(0, 0.0, tileSize, scaleY, sourceHeight << shift, yIncrement), destination.height());
    if (x2 <= 0 || y2 <= 0)
    {
        return;
    }
    const ScaledBlit blit{.source = source, .destination = destination, .shift = shift, .xIncrement = xIncrement, .yIncrement = yIncrement};
    if (tileSize >= destinationWidth && tileSize >= destinationHeight)
    {
        blit.blit(0, 0, x2, y2, detail::toInt(sourceLocation(0, 0.0, scaleX)), detail::toInt(sourceLocation(0, 0.0, scaleY)));
        return;
    }
    for (std::int32_t tileY = 0; tileY < y2; tileY += tileSize)
    {
        const std::int32_t yLocation = detail::toInt(sourceLocation(tileY, 0.0, scaleY));
        for (std::int32_t tileX = 0; tileX < x2; tileX += tileSize)
        {
            const std::int32_t xLocation = detail::toInt(sourceLocation(tileX, 0.0, scaleX));
            blit.blit(tileX, tileY, std::min(tileX + tileSize, x2), std::min(tileY + tileSize, y2), xLocation, yLocation);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Perspective matrices (PerspectiveFilter), indexed [row][column].

using Matrix = std::array<std::array<double, 3>, 3>;

Matrix squareToQuad(const PerspectiveQuad& quad) noexcept
{
    const auto [x0, y0] = quad[0];
    const auto [x1, y1] = quad[1];
    const auto [x2, y2] = quad[2];
    const auto [x3, y3] = quad[3];
    const double dx3 = x0 - x1 + x2 - x3;
    const double dy3 = y0 - y1 + y2 - y3;
    Matrix m{};
    m[2][2] = 1.0;
    if (dx3 == 0.0 && dy3 == 0.0)
    {
        m[0][0] = x1 - x0;
        m[0][1] = x2 - x1;
        m[0][2] = x0;
        m[1][0] = y1 - y0;
        m[1][1] = y2 - y1;
        m[1][2] = y0;
        m[2][0] = 0.0;
        m[2][1] = 0.0;
        return m;
    }
    const double dx1 = x1 - x2;
    const double dy1 = y1 - y2;
    const double dx2 = x3 - x2;
    const double dy2 = y3 - y2;
    const double inverseDeterminant = 1.0 / ((dx1 * dy2) - (dx2 * dy1));
    m[2][0] = ((dx3 * dy2) - (dx2 * dy3)) * inverseDeterminant;
    m[2][1] = ((dx1 * dy3) - (dx3 * dy1)) * inverseDeterminant;
    m[0][0] = x1 - x0 + (m[2][0] * x1);
    m[0][1] = x3 - x0 + (m[2][1] * x3);
    m[0][2] = x0;
    m[1][0] = y1 - y0 + (m[2][0] * y1);
    m[1][1] = y3 - y0 + (m[2][1] * y3);
    m[1][2] = y0;
    return m;
}

/// The adjoint of square-to-quad, as OSP's getQuadToSquare (an unnormalized inverse).
Matrix quadToSquare(const PerspectiveQuad& quad) noexcept
{
    const Matrix s = squareToQuad(quad);
    Matrix m{};
    m[0][0] = (s[1][1] * s[2][2]) - (s[1][2] * s[2][1]);
    m[1][0] = (s[1][2] * s[2][0]) - (s[1][0] * s[2][2]);
    m[2][0] = (s[1][0] * s[2][1]) - (s[1][1] * s[2][0]);
    m[0][1] = (s[0][2] * s[2][1]) - (s[0][1] * s[2][2]);
    m[1][1] = (s[0][0] * s[2][2]) - (s[0][2] * s[2][0]);
    m[2][1] = (s[0][1] * s[2][0]) - (s[0][0] * s[2][1]);
    m[0][2] = (s[0][1] * s[1][2]) - (s[0][2] * s[1][1]);
    m[1][2] = (s[0][2] * s[1][0]) - (s[0][0] * s[1][2]);
    m[2][2] = (s[0][0] * s[1][1]) - (s[0][1] * s[1][0]);
    return m;
}

/// OSP's concatenate(m1, m2): m2 after m1.
ProjectiveTransform concatenate(const Matrix& m1, const Matrix& m2) noexcept
{
    ProjectiveTransform t;
    auto& m = t.m;
    for (std::size_t row = 0; row < 3; ++row)
    {
        for (std::size_t column = 0; column < 3; ++column)
        {
            m[(row * 3) + column] = (m1[0][column] * m2[row][0]) + (m1[1][column] * m2[row][1]) + (m1[2][column] * m2[row][2]);
        }
    }
    return t;
}

/// OSP getColor's base pixel and fractions for a sample position; nullopt outside the image. `edge` is set
/// for the last row and column, which OSP copies without interpolating.
struct SampleCell
{
    std::int32_t column;
    std::int32_t row;
    double u;
    double v;
    bool edge;
};

std::optional<SampleCell> sampleCell(double x, double y, int w, int h) noexcept
{
    const std::int32_t column = detail::toInt(std::floor(x));
    const std::int32_t row = detail::toInt(std::floor(y));
    if (column < 0 || column >= w || row < 0 || row >= h)
    {
        return std::nullopt;
    }
    // OSP's fractional parts: x % col equals x - col for 0 <= x - col < col.
    return SampleCell{.column = column,
                      .row = row,
                      .u = column == 0 ? x : std::fmod(x, column),
                      .v = row == 0 ? y : std::fmod(y, row),
                      .edge = column + 1 == w || row + 1 == h};
}

/// PerspectiveFilter getColor with interpolation 2 (bilinear); outside the image is black.
std::uint32_t sampleBilinear(double x, double y, const FilterImage& image, bool transposed) noexcept
{
    const int w = image.width();
    const auto pixels = image.pixels();
    const auto cell = sampleCell(x, y, w, image.height());
    if (!cell)
    {
        return OPAQUE_ALPHA;
    }
    const auto [column, row, u, v, edge] = *cell;
    if (edge)
    {
        return OPAQUE_ALPHA | (pixels[index(column, row, w)] & RGB_MASK);
    }
    const std::array<std::uint32_t, 4> values{pixels[index(column, row, w)],
                                              pixels[index(column + 1, row, w)],
                                              pixels[index(column, row + 1, w)],
                                              pixels[index(column + 1, row + 1, w)]};
    // OSP evaluates (1 - v)((1 - u) a0 + u a2) + v((1 - u) a1 + u a3) with a = (c, r), (c + 1, r),
    // (c, r + 1), (c + 1, r + 1), weighting the wrong neighbours. Swapping a1 and a2 corrects it.
    const std::size_t right = transposed ? 2 : 1;
    const std::size_t below = transposed ? 1 : 2;
    const double fu = u;
    const double fv = v;
    const auto interpolate = [&](int (*channel)(std::uint32_t) noexcept)
    {
        return detail::toInt(((1 - fv) * (((1 - fu) * channel(values[0])) + (fu * channel(values[right])))) +
                             (fv * (((1 - fu) * channel(values[below])) + (fu * channel(values[3])))));
    };
    return detail::packBits(interpolate(redOf), interpolate(greenOf), interpolate(blueOf));
}

/// RadialDistortionFilter getColor with interpolation 1: the input pixel index OSP's nearest-of-2x2 rule
/// picks, or -1 (black) outside the image.
std::int32_t nearestSource(double x, double y, int w, int h) noexcept
{
    const auto cell = sampleCell(x, y, w, h);
    if (!cell)
    {
        return -1;
    }
    const int column = cell->column + (cell->edge || cell->u < 0.5 ? 0 : 1);
    const int row = cell->row + (cell->edge || cell->v < 0.5 ? 0 : 1);
    return static_cast<std::int32_t>(index(column, row, w));
}

// ---------------------------------------------------------------------------------------------
// Radial projections (RadialDistortionFilter)

constexpr double MIN_RADIUS = 0.20;
constexpr double MAX_RADIUS = 1.0;
constexpr double MIN_FOV = PI / 18;
constexpr double MAX_FOV = PI - .001;

double angleAtRadius(double r, RadialProjection projection, double fov) noexcept
{
    switch (projection)
    {
    case RadialProjection::Rectilinear: // r = L tan(theta)
        return std::atan(r * std::tan(fov / 2));
    case RadialProjection::Equidistant: // r = L theta
        return r * fov / 2;
    case RadialProjection::Equisolid: // r = 2L sin(theta / 2)
        return 2 * std::asin(r * std::sin(fov / 4));
    case RadialProjection::Stereographic: // r = 2L tan(theta / 2)
        return 2 * std::atan(r * std::tan(fov / 4));
    case RadialProjection::Orthographic: // r = L sin(theta)
        return std::asin(r * std::sin(fov / 2));
    }
    return r * fov / 2;
}

/// d/dr of atan(r tan) and asin(r sin), given tan or sin of the projection's half angle.
double tangentDerivative(double r, double tan) noexcept
{
    return tan / (1 + (r * r * tan * tan));
}

double sineDerivative(double r, double sin) noexcept
{
    return sin / std::sqrt(1 - (r * r * sin * sin));
}

double derivativeAtRadius(double r, RadialProjection projection, double fov) noexcept
{
    switch (projection)
    {
    case RadialProjection::Rectilinear:
        return tangentDerivative(r, std::tan(fov / 2));
    case RadialProjection::Equidistant:
        return fov / 2;
    case RadialProjection::Equisolid:
        return 2 * sineDerivative(r, std::sin(fov / 4));
    case RadialProjection::Stereographic:
        return 2 * tangentDerivative(r, std::tan(fov / 4));
    case RadialProjection::Orthographic:
        return sineDerivative(r, std::sin(fov / 2));
    }
    return fov / 2;
}

double pixelsToCorner(int width, int height) noexcept
{
    // Math.sqrt(w * w + h * h) / 2 with Java int arithmetic.
    return std::sqrt(
               static_cast<double>(detail::addWrapping(detail::multiplyWrapping(width, width), detail::multiplyWrapping(height, height)))) /
           2;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Resize

void ResizeFilter::setWidthFactor(double factor) noexcept
{
    invalidateSource();
    m_WidthFactor = detail::max(detail::min(std::abs(factor), 10), 0.01);
}

void ResizeFilter::setHeightFactor(double factor) noexcept
{
    invalidateSource();
    m_HeightFactor = detail::max(detail::min(std::abs(factor), 10), 0.01);
}

FilterPoint ResizeFilter::inputToOutput(FilterPoint point) const noexcept
{
    return {point.x * m_WidthFactor, point.y * m_HeightFactor};
}

FilterPoint ResizeFilter::outputToInput(FilterPoint point) const noexcept
{
    return {point.x / m_WidthFactor, point.y / m_HeightFactor};
}

std::pair<int, int> ResizeFilter::outputSize(int width, int height)
{
    // Filter.autoScale720x480: DV frames are shown with square pixels.
    if (width == 720 && height == 480 && m_WidthFactor == 1.0 && m_HeightFactor == 1.0)
    {
        m_WidthFactor = 0.889;
    }
    return {detail::toInt(width * m_WidthFactor), detail::toInt(height * m_HeightFactor)};
}

void ResizeFilter::render(const FilterImage& input, FilterImage& output, int /*videoFrame*/)
{
    // Graphics2D.drawImage with AffineTransform.getScaleInstance(wf, hf) and the default
    // nearest-neighbour interpolation (sun.java2d.pipe.DrawImage.transformImage).
    const double width = input.width() * m_WidthFactor;
    const double height = input.height() * m_HeightFactor;
    if (std::abs(width - input.width()) < 0.0001 && std::abs(height - input.height()) < 0.0001)
    {
        // tryCopyOrScale: within 1e-4 of the source size the image is copied, not scaled.
        const int w = std::min(input.width(), output.width());
        const int h = std::min(input.height(), output.height());
        for (int y = 0; y < h; ++y)
        {
            for (int x = 0; x < w; ++x)
            {
                output.pixels()[index(x, y, output.width())] = OPAQUE_ALPHA | (input.pixels()[index(x, y, input.width())] & RGB_MASK);
            }
        }
        return;
    }
    scaleImage(input, output, width, height);
}

// ---------------------------------------------------------------------------------------------
// Rotate

std::string_view filterRotationName(FilterRotation rotation) noexcept
{
    switch (rotation)
    {
    case FilterRotation::None:
        return "None";
    case FilterRotation::CounterClockwise90:
        return "CCW";
    case FilterRotation::Clockwise90:
        return "CW";
    case FilterRotation::Rotate180:
        return "180";
    }
    return {};
}

std::optional<FilterRotation> filterRotationFromName(std::string_view name) noexcept
{
    for (const auto rotation :
         {FilterRotation::None, FilterRotation::CounterClockwise90, FilterRotation::Clockwise90, FilterRotation::Rotate180})
    {
        if (filterRotationName(rotation) == name)
        {
            return rotation;
        }
    }
    return std::nullopt;
}

void RotateFilter::setRotation(FilterRotation rotation) noexcept
{
    if (rotation != m_Rotation)
    {
        m_Rotation = rotation;
        invalidateSource();
    }
}

std::pair<int, int> RotateFilter::outputSize(int width, int height)
{
    const bool quarter = m_Rotation == FilterRotation::Clockwise90 || m_Rotation == FilterRotation::CounterClockwise90;
    return quarter ? std::pair{height, width} : std::pair{width, height};
}

FilterPoint RotateFilter::inputToOutput(FilterPoint point, int width, int height) const noexcept
{
    const double w = width;
    const double h = height;
    const FilterPoint p = m_Reversed ? FilterPoint{w - point.x, point.y} : point;
    switch (m_Rotation)
    {
    case FilterRotation::Clockwise90:
        return {h - p.y, p.x};
    case FilterRotation::CounterClockwise90:
        return {p.y, w - p.x};
    case FilterRotation::Rotate180:
        return {w - p.x, h - p.y};
    case FilterRotation::None:
        break;
    }
    return p;
}

FilterPoint RotateFilter::outputToInput(FilterPoint point, int width, int height) const noexcept
{
    const double w = width;
    const double h = height;
    FilterPoint p = point;
    switch (m_Rotation)
    {
    case FilterRotation::Clockwise90:
        p = {point.y, h - point.x};
        break;
    case FilterRotation::CounterClockwise90:
        p = {w - point.y, point.x};
        break;
    case FilterRotation::Rotate180:
        p = {w - point.x, h - point.y};
        break;
    case FilterRotation::None:
        break;
    }
    return m_Reversed ? FilterPoint{w - p.x, p.y} : p;
}

void RotateFilter::render(const FilterImage& input, FilterImage& output, int /*videoFrame*/)
{
    const auto in = input.pixels();
    auto out = output.pixels();
    const int w = input.width();
    const int h = input.height();
    const auto store = [&](std::size_t destination, std::size_t source)
    {
        out[destination] = OPAQUE_ALPHA | (in[source] & RGB_MASK);
    };
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            const std::size_t i = index(x, y, w);
            const int mirrored = m_Reversed ? w - x - 1 : x;
            switch (m_Rotation)
            {
            case FilterRotation::None:
                store(index(mirrored, y, w), i);
                break;
            case FilterRotation::Clockwise90:
                store(index(h - y - 1, mirrored, h), i);
                break;
            case FilterRotation::CounterClockwise90:
                store(index(y, w - mirrored - 1, h), i);
                break;
            case FilterRotation::Rotate180:
                store(index(w - mirrored - 1, h - y - 1, w), i);
                break;
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Perspective

FilterPoint ProjectiveTransform::map(FilterPoint point) const noexcept
{
    const auto [x, y] = point;
    const double w = (m[6] * x) + (m[7] * y) + m[8];
    if (w == 0)
    {
        return point;
    }
    return {((m[0] * x) + (m[1] * y) + m[2]) / w, ((m[3] * x) + (m[4] * y) + m[5]) / w};
}

void PerspectiveFilter::loadKeyFrame(PerspectivePlane plane, int videoFrame, const PerspectiveQuad& quad)
{
    keys(plane)[videoFrame] = quad;
}

void PerspectiveFilter::setCorners(PerspectivePlane plane, int videoFrame, const PerspectiveQuad& quad)
{
    keys(plane)[isFixed(plane) ? 0 : videoFrame] = quad;
}

bool PerspectiveFilter::removeKeyFrame(PerspectivePlane plane, int videoFrame)
{
    auto& map = keys(plane);
    if (isFixed(plane))
    {
        return false;
    }
    const auto next = map.upper_bound(videoFrame);
    if (next == map.begin() || std::prev(next)->first == 0)
    {
        return false;
    }
    map.erase(std::prev(next));
    return true;
}

void PerspectiveFilter::setFixed(PerspectivePlane plane, bool fixed, int currentVideoFrame)
{
    if (fixed == isFixed(plane))
    {
        return;
    }
    const auto shown = corners(plane, currentVideoFrame);
    (plane == PerspectivePlane::Input ? m_FixedInput : m_FixedOutput) = fixed;
    if (fixed && shown)
    {
        auto& map = keys(plane);
        map.clear();
        map[0] = *shown;
    }
}

std::vector<int> PerspectiveFilter::keyFrames(PerspectivePlane plane) const
{
    const auto frames = keys(plane) | std::views::keys;
    return {frames.begin(), frames.end()};
}

std::optional<PerspectiveQuad> PerspectiveFilter::corners(PerspectivePlane plane, int videoFrame) const
{
    const auto& map = keys(plane);
    if (map.empty())
    {
        return std::nullopt;
    }
    // PerspectiveFilter.getKeyFrame: fixedKey (0) when fixed, else the last key at or before the frame,
    // else 0. A missing key 0 (only possible in edited XML) falls back to the first key.
    auto key = map.end();
    if (!isFixed(plane))
    {
        const auto next = map.upper_bound(videoFrame);
        if (next != map.begin())
        {
            key = std::prev(next);
        }
    }
    if (key == map.end())
    {
        key = map.find(0);
    }
    return key == map.end() ? map.begin()->second : key->second;
}

std::optional<ProjectiveTransform> PerspectiveFilter::outputToInput(int videoFrame) const
{
    const auto in = corners(PerspectivePlane::Input, videoFrame);
    const auto out = corners(PerspectivePlane::Output, videoFrame);
    if (!in || !out)
    {
        return std::nullopt;
    }
    return concatenate(quadToSquare(*out), squareToQuad(*in));
}

std::optional<ProjectiveTransform> PerspectiveFilter::inputToOutput(int videoFrame) const
{
    const auto in = corners(PerspectivePlane::Input, videoFrame);
    const auto out = corners(PerspectivePlane::Output, videoFrame);
    if (!in || !out)
    {
        return std::nullopt;
    }
    return concatenate(quadToSquare(*in), squareToQuad(*out));
}

void PerspectiveFilter::initialize(const FilterImage& input)
{
    if (!m_InputKeys.empty())
    {
        return;
    }
    // Java's int arithmetic: w / 4 and 3 * w / 4 truncate before becoming corner coordinates.
    const int left = input.width() / 4;
    const int right = 3 * input.width() / 4;
    const int top = input.height() / 4;
    const int bottom = 3 * input.height() / 4;
    const PerspectiveQuad defaults{FilterPoint(left, top), FilterPoint(right, top), FilterPoint(right, bottom), FilterPoint(left, bottom)};
    m_InputKeys[0] = defaults;
    m_OutputKeys[0] = defaults;
}

void PerspectiveFilter::render(const FilterImage& input, FilterImage& output, int videoFrame)
{
    // Both planes have keys after initialize(); outputToInput only fails without them.
    const ProjectiveTransform transform = outputToInput(videoFrame).value_or(ProjectiveTransform{});
    const bool transposed = m_Arithmetic == FilterArithmetic::Tracker635;
    auto out = output.pixels();
    for (int j = 0; j < input.height(); ++j)
    {
        for (int i = 0; i < input.width(); ++i)
        {
            const FilterPoint sample = transform.map({static_cast<double>(i), static_cast<double>(j)});
            out[index(i, j, input.width())] = sampleBilinear(sample.x, sample.y, input, transposed);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Radial distortion

std::string_view radialProjectionName(RadialProjection projection) noexcept
{
    switch (projection)
    {
    case RadialProjection::Rectilinear:
        return "Rectilinear";
    case RadialProjection::Equisolid:
        return "Equisolid";
    case RadialProjection::Equidistant:
        return "Equidistant";
    case RadialProjection::Stereographic:
        return "Stereographic";
    case RadialProjection::Orthographic:
        return "Orthographic";
    }
    return {};
}

std::optional<RadialProjection> radialProjectionFromName(std::string_view name) noexcept
{
    for (const auto projection : {RadialProjection::Rectilinear,
                                  RadialProjection::Equisolid,
                                  RadialProjection::Equidistant,
                                  RadialProjection::Stereographic,
                                  RadialProjection::Orthographic})
    {
        if (radialProjectionName(projection) == name)
        {
            return projection;
        }
    }
    return std::nullopt;
}

void RadialDistortionFilter::setFixedRadius(double fraction) noexcept
{
    if (std::isnan(fraction))
    {
        return;
    }
    fraction = std::max(std::min(std::abs(fraction), MAX_RADIUS), MIN_RADIUS);
    if (fraction != m_FixedRadius)
    {
        m_FixedRadius = fraction;
        m_ValidTransform = false;
    }
}

void RadialDistortionFilter::setSourceFov(double radians) noexcept
{
    if (std::isnan(radians))
    {
        return;
    }
    radians = std::max(std::min(std::abs(radians), PI - .0005), MIN_FOV);
    if (radians != m_SourceFov)
    {
        m_SourceFov = radians;
        m_ValidTransform = false;
    }
}

void RadialDistortionFilter::setSourceProjection(RadialProjection projection) noexcept
{
    if (projection != m_SourceProjection)
    {
        m_SourceProjection = projection;
        m_HasLowerLimit = angleAtRadius(0.5, m_OutputProjection, PI / 2) < angleAtRadius(0.5, m_SourceProjection, PI / 2);
        m_ValidTransform = false;
    }
}

void RadialDistortionFilter::setOutputProjection(RadialProjection projection) noexcept
{
    if (projection != m_OutputProjection)
    {
        m_OutputProjection = projection;
        m_HasLowerLimit = angleAtRadius(0.5, m_OutputProjection, PI / 2) < angleAtRadius(0.5, m_SourceProjection, PI / 2);
        m_ValidTransform = false;
    }
}

RadialDistortionFilter::Scale RadialDistortionFilter::refreshScale(Scale scale) const
{
    // RadialDistortionFilter.refreshScale, whose recursion runs at most twice.
    while (true)
    {
        const double theta = angleAtRadius(scale.fixedRadius, m_SourceProjection, m_SourceFov);
        switch (m_OutputProjection)
        {
        case RadialProjection::Rectilinear: // d = L tan(theta)
            scale.outputFov = 2 * std::atan(std::tan(theta) / scale.fixedRadius);
            break;
        case RadialProjection::Equidistant: // d = L theta
            scale.outputFov = 2 * theta / scale.fixedRadius;
            break;
        case RadialProjection::Equisolid: // d = 2L sin(theta / 2)
            scale.outputFov = 4 * std::asin(std::sin(theta / 2) / scale.fixedRadius);
            break;
        case RadialProjection::Stereographic: // d = 2L tan(theta / 2)
            scale.outputFov = 4 * std::atan(std::tan(theta / 2) / scale.fixedRadius);
            break;
        case RadialProjection::Orthographic: // d = L sin(theta)
            scale.outputFov = 2 * std::asin(std::sin(theta) / scale.fixedRadius);
            break;
        }
        if (std::isnan(scale.outputFov))
        {
            scale.outputFov = MAX_FOV;
        }
        if (!m_HasLowerLimit)
        {
            scale.lowerRadiusLimit = detail::toInt(100 * MIN_RADIUS);
            return scale;
        }
        // findRadiusLimit: Newton's method for the radius whose source angle equals a 180-degree output.
        double limit = 0.8;
        double f = angleAtRadius(limit, m_SourceProjection, m_SourceFov) - angleAtRadius(limit, m_OutputProjection, PI);
        double slope = derivativeAtRadius(limit, m_SourceProjection, m_SourceFov) - derivativeAtRadius(limit, m_OutputProjection, PI);
        int iteration = 0;
        for (; std::abs(f) > 0.000001 && iteration < 10; ++iteration)
        {
            limit = limit - (f / slope);
            f = angleAtRadius(limit, m_SourceProjection, m_SourceFov) - angleAtRadius(limit, m_OutputProjection, PI);
            slope = derivativeAtRadius(limit, m_SourceProjection, m_SourceFov) - derivativeAtRadius(limit, m_OutputProjection, PI);
        }
        if (iteration < 10 && !std::isnan(limit) && limit > .005 && limit < 1.01)
        {
            scale.lowerRadiusLimit = std::max(detail::toInt(std::floor(100 * limit)), detail::toInt(100 * MIN_RADIUS));
        }
        if (scale.outputFov > MAX_FOV && scale.fixedRadius != scale.lowerRadiusLimit / 100.0)
        {
            scale.fixedRadius = scale.lowerRadiusLimit / 100.0;
            continue;
        }
        return scale;
    }
}

double RadialDistortionFilter::stretch(double radius, double outputFov, double toCorner) const
{
    if (radius == 0)
    {
        return 1;
    }
    const double theta = angleAtRadius(radius / toCorner, m_OutputProjection, outputFov);
    double source = radius;
    switch (m_SourceProjection)
    {
    case RadialProjection::Rectilinear:
        source = toCorner / std::tan(m_SourceFov / 2) * std::tan(theta);
        break;
    case RadialProjection::Equidistant:
        source = 2 * toCorner / m_SourceFov * theta;
        break;
    case RadialProjection::Equisolid:
        source = toCorner / std::sin(m_SourceFov / 4) * std::sin(theta / 2);
        break;
    case RadialProjection::Stereographic:
        source = toCorner / std::tan(m_SourceFov / 4) * std::tan(theta / 2);
        break;
    case RadialProjection::Orthographic:
        source = toCorner / std::sin(m_SourceFov / 2) * std::sin(theta);
        break;
    }
    return source / radius;
}

FilterPoint RadialDistortionFilter::outputToInput(FilterPoint point, int width, int height) const
{
    const Scale scale = refreshScale({.fixedRadius = m_FixedRadius, .outputFov = m_OutputFov, .lowerRadiusLimit = m_LowerRadiusLimit});
    const double xCenter = width / 2.0;
    const double yCenter = height / 2.0;
    const double dx = point.x - xCenter;
    const double dy = point.y - yCenter;
    const double s = stretch(std::sqrt((dx * dx) + (dy * dy)), scale.outputFov, pixelsToCorner(width, height));
    return {xCenter + (s * dx), yCenter + (s * dy)};
}

void RadialDistortionFilter::initialize(const FilterImage& /*input*/)
{
    m_ValidTransform = false;
}

void RadialDistortionFilter::render(const FilterImage& input, FilterImage& output, int /*videoFrame*/)
{
    const int w = input.width();
    const int h = input.height();
    if (!m_ValidTransform || w != m_TransformWidth || h != m_TransformHeight)
    {
        const Scale scale = refreshScale({.fixedRadius = m_FixedRadius, .outputFov = m_OutputFov, .lowerRadiusLimit = m_LowerRadiusLimit});
        m_FixedRadius = scale.fixedRadius;
        m_OutputFov = scale.outputFov;
        m_LowerRadiusLimit = scale.lowerRadiusLimit;
        const double toCorner = pixelsToCorner(w, h);
        const double xCenter = w / 2.0;
        const double yCenter = h / 2.0;
        constexpr double NUDGE = 0.0001;
        m_Sources.resize(input.pixels().size());
        for (int j = 0; j < h; ++j)
        {
            for (int i = 0; i < w; ++i)
            {
                const double dx = i - xCenter;
                const double dy = j - yCenter;
                const double s = stretch(std::sqrt((dx * dx) + (dy * dy)), m_OutputFov, toCorner);
                m_Sources[index(i, j, w)] = nearestSource(xCenter + (s * dx) + NUDGE, yCenter + (s * dy) + NUDGE, w, h);
            }
        }
        m_ValidTransform = true;
        m_TransformWidth = w;
        m_TransformHeight = h;
    }
    const auto in = input.pixels();
    std::ranges::transform(m_Sources,
                           output.pixels().begin(),
                           [&](std::int32_t source)
                           { return source < 0 ? OPAQUE_ALPHA : OPAQUE_ALPHA | (in[static_cast<std::size_t>(source)] & RGB_MASK); });
}

// ---------------------------------------------------------------------------------------------
// clone()

std::unique_ptr<VideoFilter> ResizeFilter::clone() const
{
    return std::make_unique<ResizeFilter>(*this);
}

std::unique_ptr<VideoFilter> RotateFilter::clone() const
{
    return std::make_unique<RotateFilter>(*this);
}

std::unique_ptr<VideoFilter> PerspectiveFilter::clone() const
{
    return std::make_unique<PerspectiveFilter>(*this);
}

std::unique_ptr<VideoFilter> RadialDistortionFilter::clone() const
{
    return std::make_unique<RadialDistortionFilter>(*this);
}

} // namespace motrack::science
