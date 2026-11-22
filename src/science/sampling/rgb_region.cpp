#include "science/sampling/rgb_region.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace motrack::science
{

namespace
{

// java.awt.geom.EllipseIterator: four cubic Beziers approximating a circle of radius 0.5 centred
// at (0.5, 0.5); CtrlVal = ArcIterator.btan(PI / 2).
constexpr double ELLIPSE_CONTROL = 0.5522847498307933;
constexpr double PCV = 0.5 + (ELLIPSE_CONTROL * 0.5);
constexpr double NCV = 0.5 - (ELLIPSE_CONTROL * 0.5);
constexpr std::array<std::array<double, 6>, 4> ELLIPSE_CONTROL_POINTS = {{
    {1.0, PCV, PCV, 1.0, 0.5, 1.0},
    {NCV, 1.0, 0.0, PCV, 0.0, 0.5},
    {0.0, NCV, NCV, 0.0, 0.5, 0.0},
    {PCV, 0.0, 1.0, NCV, 1.0, 0.5},
}};

constexpr std::int64_t JAVA_INT_MAX = std::numeric_limits<std::int32_t>::max();

bool finite(ImagePoint point) noexcept
{
    return std::isfinite(point.x) && std::isfinite(point.y);
}

// new Ellipse2D.Double(-w / 2, -h / 2, w, h) through EllipseIterator, before translation.
void appendEllipse(detail::JavaPath& path, int width, int height)
{
    if (width < 0 || height < 0)
    {
        return; // the iterator starts "done": an empty path
    }
    const int left = -width / 2; // Java int division, as RGBStep.createRGBShape
    const int top = -height / 2;
    const auto x = static_cast<double>(left);
    const auto y = static_cast<double>(top);
    const auto w = static_cast<double>(width);
    const auto h = static_cast<double>(height);
    const auto& start = ELLIPSE_CONTROL_POINTS[3];
    path.moveTo(x + (start[4] * w), y + (start[5] * h));
    for (const auto& c : ELLIPSE_CONTROL_POINTS)
    {
        path.curveTo(x + (c[0] * w), y + (c[1] * h), x + (c[2] * w), y + (c[3] * h), x + (c[4] * w), y + (c[5] * h));
    }
}

// new Rectangle2D.Double(-w / 2, -h / 2, w, h) through RectIterator, before translation.
void appendRectangle(detail::JavaPath& path, int width, int height)
{
    if (width < 0 || height < 0)
    {
        return;
    }
    const int left = -width / 2;
    const int top = -height / 2;
    const auto x = static_cast<double>(left);
    const auto y = static_cast<double>(top);
    const auto w = static_cast<double>(width);
    const auto h = static_cast<double>(height);
    path.moveTo(x, y);
    path.lineTo(x + w, y);
    path.lineTo(x + w, y + h);
    path.lineTo(x, y + h);
    path.lineTo(x, y);
}

double sampleDeviation(std::int64_t sum, std::int64_t squareSum, double mean, std::int64_t count) noexcept
{
    if (count == 1)
    {
        return std::numeric_limits<double>::quiet_NaN();
    }
    // Java: Math.sqrt((r2 - r * rMean) / (n - 1)), all in double.
    return std::sqrt((static_cast<double>(squareSum) - (static_cast<double>(sum) * mean)) / static_cast<double>(count - 1));
}

} // namespace

RgbShapeKind rgbShapeKindFromSaved(std::optional<int> shapeType) noexcept
{
    if (!shapeType || *shapeType == static_cast<int>(RgbShapeKind::Ellipse))
    {
        return RgbShapeKind::Ellipse;
    }
    return *shapeType == static_cast<int>(RgbShapeKind::Polygon) ? RgbShapeKind::Polygon : RgbShapeKind::Rectangle;
}

int rgbMaxEdgeLength(int videoWidth, int videoHeight) noexcept
{
    return std::min(videoHeight, videoWidth) - 1;
}

int clampRgbEdgeLength(int requested, int maxEdgeLength) noexcept
{
    return std::min(std::max(requested, 0), maxEdgeLength);
}

RgbPolygon::RgbPolygon(std::vector<ImagePoint> vertices) noexcept : m_Vertices(std::move(vertices))
{}

std::span<const ImagePoint> RgbPolygon::vertices() const noexcept
{
    return m_Vertices;
}

bool RgbPolygon::empty() const noexcept
{
    return m_Vertices.empty();
}

bool RgbPolygon::isClosed() const noexcept
{
    if (m_Vertices.size() < 3)
    {
        return false;
    }
    const ImagePoint last = m_Vertices.back();
    return last.x == 0 && last.y == 0;
}

void RgbPolygon::append(ImagePoint relative)
{
    m_Vertices.push_back(relative);
}

bool RgbPolygon::close()
{
    if (isClosed() || m_Vertices.size() < 3)
    {
        return false;
    }
    m_Vertices.push_back({.x = 0.0, .y = 0.0});
    return true;
}

bool RgbPolygon::reopen()
{
    if (!isClosed())
    {
        return false;
    }
    m_Vertices.pop_back();
    return true;
}

std::size_t RgbPolygon::handleCount() const noexcept
{
    const std::size_t fixed = isClosed() ? 2 : 1;
    return m_Vertices.size() > fixed ? m_Vertices.size() - fixed : 0;
}

bool RgbPolygon::moveHandleVertex(int handle, ImagePoint relative) noexcept
{
    if (handle < 0 || static_cast<std::size_t>(handle) + 2 > m_Vertices.size())
    {
        return false;
    }
    m_Vertices[static_cast<std::size_t>(handle) + 1] = relative;
    return true;
}

bool RgbPolygon::removeHandleVertex(int handle)
{
    if (m_Vertices.size() < 2 || handle < 0 || m_Vertices.size() < static_cast<std::size_t>(handle) + 2)
    {
        return false;
    }
    m_Vertices.erase(m_Vertices.begin() + handle + 1);
    return true;
}

RgbRegionOutline::RgbRegionOutline(const RgbRegionShape& shape, ImagePoint position)
{
    m_Finite = finite(position);
    if (shape.kind == RgbShapeKind::Polygon)
    {
        const auto vertices = shape.polygon.vertices();
        m_Finite = m_Finite && std::ranges::all_of(vertices, finite);
        // Polygon2D.add: the first vertex is a moveTo, every later one a lineTo.
        for (std::size_t i = 0; i < vertices.size(); ++i)
        {
            if (i == 0)
            {
                m_Path.moveTo(vertices[i].x, vertices[i].y);
            }
            else
            {
                m_Path.lineTo(vertices[i].x, vertices[i].y);
            }
        }
    }
    else if (shape.kind == RgbShapeKind::Rectangle)
    {
        appendRectangle(m_Path, shape.width, shape.height);
    }
    else
    {
        appendEllipse(m_Path, shape.width, shape.height);
    }
    if (m_Finite)
    {
        m_Path.translate(position.x, position.y);
    }
}

bool RgbRegionOutline::isFinite() const noexcept
{
    return m_Finite;
}

std::optional<PixelRect> RgbRegionOutline::bounds() const noexcept
{
    if (!m_Finite)
    {
        return std::nullopt;
    }
    // Rectangle2D.Double(minX, minY, maxX - minX, maxY - minY).getBounds()
    const auto box = m_Path.bounds2D();
    const double width = box.maxX - box.minX;
    const double height = box.maxY - box.minY;
    const double x1 = std::floor(box.minX);
    const double y1 = std::floor(box.minY);
    const double x2 = std::ceil(box.minX + width);
    const double y2 = std::ceil(box.minY + height);
    constexpr auto INT_LOW = static_cast<double>(std::numeric_limits<int>::min());
    constexpr auto INT_HIGH = static_cast<double>(std::numeric_limits<int>::max());
    // Written so that NaN (for example inf - inf from coordinates that overflowed) fails as well.
    const auto representable = [](double value)
    {
        return value >= INT_LOW && value <= INT_HIGH;
    };
    if (!(representable(x1) && representable(y1) && representable(x2) && representable(y2) && x2 - x1 <= INT_HIGH && y2 - y1 <= INT_HIGH))
    {
        return std::nullopt;
    }
    return PixelRect{
        .x = static_cast<int>(x1), .y = static_cast<int>(y1), .width = static_cast<int>(x2 - x1), .height = static_cast<int>(y2 - y1)};
}

bool RgbRegionOutline::contains(double x, double y) const noexcept
{
    return m_Finite && m_Path.contains(x, y);
}

bool RgbRegionOutline::includesPixel(int x, int y) const noexcept
{
    return contains(static_cast<double>(x) + 0.5, static_cast<double>(y) + 0.5);
}

RgbRegionMeasurement measureRgbRegion(const ArgbImage& image, const RgbRegionShape& shape, ImagePoint position)
{
    RgbRegionMeasurement result;
    const RgbRegionOutline outline(shape, position);
    const auto bounds = outline.bounds();
    if (!bounds)
    {
        result.status = outline.isFinite() ? RgbRegionStatus::OutsideImage : RgbRegionStatus::InvalidGeometry;
        return result;
    }
    result.bounds = *bounds;
    // Raster.getDataElements(x, y, w, h) throws for any part outside the image; Tracker returns null.
    const auto right = static_cast<std::int64_t>(bounds->x) + bounds->width;
    const auto bottom = static_cast<std::int64_t>(bounds->y) + bounds->height;
    if (bounds->x < 0 || bounds->y < 0 || right > image.width() || bottom > image.height())
    {
        result.status = RgbRegionStatus::OutsideImage;
        return result;
    }
    RgbStatistics& statistics = result.statistics;
    for (int y = bounds->y; y < bottom; ++y)
    {
        const auto row = image.row(y);
        for (int x = bounds->x; x < right; ++x)
        {
            if (!outline.includesPixel(x, y))
            {
                continue;
            }
            const std::uint32_t pixel = row[static_cast<std::size_t>(x)];
            const std::array<std::int64_t, 3> channels = {redOf(pixel), greenOf(pixel), blueOf(pixel)};
            ++statistics.pixels;
            for (std::size_t c = 0; c < channels.size(); ++c)
            {
                statistics.sums[c] += channels[c];
                statistics.squareSums[c] += channels[c] * channels[c];
            }
        }
    }
    if (statistics.pixels == 0)
    {
        result.status = RgbRegionStatus::NoPixels;
        return result;
    }
    const auto count = static_cast<double>(statistics.pixels);
    std::array<double, 3> means{};
    std::array<double, 3> deviations{};
    for (std::size_t c = 0; c < means.size(); ++c)
    {
        means[c] = static_cast<double>(statistics.sums[c]) / count;
        deviations[c] = sampleDeviation(statistics.sums[c], statistics.squareSums[c], means[c], statistics.pixels);
        result.trackerSumOverflow =
            result.trackerSumOverflow || statistics.squareSums[c] > JAVA_INT_MAX || statistics.sums[c] > JAVA_INT_MAX;
    }
    statistics.red = means[0];
    statistics.green = means[1];
    statistics.blue = means[2];
    statistics.luma = trackerLuma(means[0], means[1], means[2]);
    statistics.redDeviation = deviations[0];
    statistics.greenDeviation = deviations[1];
    statistics.blueDeviation = deviations[2];
    result.status = RgbRegionStatus::Measured;
    return result;
}

} // namespace motrack::science
