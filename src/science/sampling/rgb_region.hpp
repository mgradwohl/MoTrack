#pragma once

#include "science/sampling/argb_image.hpp"
#include "science/sampling/java_path.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace motrack::science
{

/// RGB-region shapes, with the values Tracker saves as `shape_type`.
enum class RgbShapeKind : std::uint8_t
{
    Ellipse = 0,
    Rectangle = 1,
    Polygon = 2,
};

/// How Tracker measures a saved `shape_type`: a missing value is an ellipse, and any value other
/// than 0 (ellipse) or 2 (polygon) falls through `RGBStep.createRGBShape` to a rectangle.
[[nodiscard]] RgbShapeKind rgbShapeKindFromSaved(std::optional<int> shapeType) noexcept;

/// `RGBRegion.defaultEdgeLength`: the width and height of a new ellipse or rectangle.
inline constexpr int RGB_DEFAULT_EDGE_LENGTH = 20;

/// `RGBRegion.defaultMaxEdgeLength`: the edit limit before a video has been opened.
inline constexpr int RGB_DEFAULT_MAX_EDGE_LENGTH = 200;

/// `RGBRegion.setMaxEdgeLength`: once a video is open, edits are limited to one pixel less than the
/// smaller image dimension, whether that is above or below the 200-pixel default.
[[nodiscard]] int rgbMaxEdgeLength(int videoWidth, int videoHeight) noexcept;

/// `RGBRegion.setShapeSize` clamps an edited width or height to [0, maxEdgeLength]. Sizes loaded
/// from a file are not clamped, so the sampling functions accept any size.
[[nodiscard]] int clampRgbEdgeLength(int requested, int maxEdgeLength) noexcept;

/// Tracker's polygon (`RGBStep.Polygon2D`): vertices relative to the step position, in image pixels.
///
/// Marking a polygon starts at the position itself, so the first vertex is normally (0, 0); a
/// polygon is closed when it has at least three vertices and its last vertex is exactly (0, 0)
/// again. An open polygon is still measured, implicitly closed back to its first vertex. Editing
/// handle `i` addresses vertex `i + 1`, because vertex 0 moves with the position.
class RgbPolygon
{
  public:
    RgbPolygon() = default;

    /// The vertices as saved in an RGB region's `shapes` array, in order.
    explicit RgbPolygon(std::vector<ImagePoint> vertices) noexcept;

    [[nodiscard]] std::span<const ImagePoint> vertices() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

    /// `isClosed`: three or more vertices and a last vertex of exactly (0, 0).
    [[nodiscard]] bool isClosed() const noexcept;

    /// Adds a vertex given relative to the position (Tracker subtracts the position from the mouse point).
    void append(ImagePoint relative);

    /// Closes the polygon by appending (0, 0); returns false (unchanged) if it is already closed or
    /// has fewer than three vertices, as `setClosed(true)`.
    bool close();

    /// Reopens a closed polygon for editing by removing its closing vertex; returns false
    /// (unchanged) for an open polygon. Tracker calls `setClosed(false)` only on closed polygons.
    bool reopen();

    /// Number of editable handles: every vertex after the first, excluding the closing vertex.
    [[nodiscard]] std::size_t handleCount() const noexcept;

    /// `VertexHandle.setXY`: moves vertex `handle + 1` to `relative`. Returns false for a handle
    /// outside [0, vertices - 2].
    bool moveHandleVertex(int handle, ImagePoint relative) noexcept;

    /// `Polygon2D.remove`: deletes vertex `handle + 1`. Uses the bounds check of the Tracker tip
    /// (upstream `50d39015`, U12): 6.3.5 accepted `handle == vertices - 1` and then failed after
    /// clearing its path. Returns false (unchanged) for fewer than two vertices or a handle outside
    /// [0, vertices - 2].
    bool removeHandleVertex(int handle);

  private:
    std::vector<ImagePoint> m_Vertices;
};

/// The geometry of one RGB-region step. Sizes are whole image pixels (`RGBStep.width/height`).
struct RgbRegionShape
{
    RgbShapeKind kind = RgbShapeKind::Ellipse;
    /// Ellipse and rectangle width and height. A negative size gives an empty region.
    int width = RGB_DEFAULT_EDGE_LENGTH;
    int height = RGB_DEFAULT_EDGE_LENGTH;
    /// Polygon vertices relative to the position (polygons ignore width and height).
    RgbPolygon polygon;
};

/// The region outline Tracker measures: `RGBStep.getRGBShape(position)`.
///
/// Ellipses and rectangles span [-(size / 2), size - size / 2] around the position with integer
/// division, so an odd size reaches half a pixel further right or down than left or up, as in
/// Tracker (preserved, MoTrack #236). Ellipses
/// are Java2D's four-cubic Bezier approximation (radial error below 0.03% of the radius), not the
/// exact conic. Containment is `Path2D.contains`: non-zero winding (self-intersecting polygon
/// lobes and multiply wound areas are inside), a ray-crossing test that includes left and top
/// edges and excludes right and bottom edges, and an implicit closing segment.
class RgbRegionOutline
{
  public:
    RgbRegionOutline(const RgbRegionShape& shape, ImagePoint position);

    /// False when the position or a polygon vertex is NaN or infinite; such outlines contain nothing.
    [[nodiscard]] bool isFinite() const noexcept;

    /// `Shape.getBounds()`: the floor/ceiling of the control-point bounds, in pixels. Nothing for a
    /// non-finite outline or bounds outside the `int` range.
    [[nodiscard]] std::optional<PixelRect> bounds() const noexcept;

    /// Whether the image point (x, y) is inside (`Path2D.contains`).
    [[nodiscard]] bool contains(double x, double y) const noexcept;

    /// Tracker's inclusion rule: a pixel is measured when its centre is inside the outline.
    [[nodiscard]] bool includesPixel(int x, int y) const noexcept;

  private:
    detail::JavaPath m_Path;
    bool m_Finite = true;
};

/// Why a region produced no statistics. Tracker returns no data for every case except `Measured`.
enum class RgbRegionStatus : std::uint8_t
{
    Measured,
    /// No pixel centre is inside the outline (empty, degenerate or zero-area shapes).
    NoPixels,
    /// The pixel bounds extend past the image. Tracker reads the bounding rectangle in one raster
    /// call and treats the resulting exception as "no data", even when no included pixel centre
    /// lies outside (for example a region less than half a pixel past the edge).
    OutsideImage,
    /// The position or a polygon vertex is NaN or infinite (MoTrack-only; Tracker never saves these).
    InvalidGeometry,
};

/// Column names and order of Tracker's RGB-region statistics (after t, x, y; then step and frame).
inline constexpr std::array<std::string_view, 8> RGB_REGION_COLUMNS = {"R", "G", "B", "luma", "pixels", "Rsd", "Gsd", "Bsd"};

/// One step's statistics. Channel values are 8-bit (0-255) means of the included pixels.
struct RgbStatistics
{
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    /// `trackerLuma(red, green, blue)` of the means.
    double luma = 0.0;
    /// Number of included pixels.
    std::int64_t pixels = 0;
    /// Sample standard deviations, `sqrt((sum(v^2) - sum(v) * mean) / (n - 1))`; NaN for one pixel.
    double redDeviation = std::numeric_limits<double>::quiet_NaN();
    double greenDeviation = std::numeric_limits<double>::quiet_NaN();
    double blueDeviation = std::numeric_limits<double>::quiet_NaN();
    /// Exact per-channel sums and sums of squares (R, G, B).
    std::array<std::int64_t, 3> sums{};
    std::array<std::int64_t, 3> squareSums{};
};

struct RgbRegionMeasurement
{
    RgbRegionStatus status = RgbRegionStatus::NoPixels;
    /// Valid when `status` is `Measured`.
    RgbStatistics statistics;
    /// Tracker's pixel bounds (`Shape.getBounds()`); empty when they are invalid or exceed `int`.
    PixelRect bounds;
    /// Tracker 6.3.5 accumulates these sums in Java `int`s. When a channel's sum of squares (or
    /// sum) exceeds 2^31 - 1 - more than 33,025 pixels of value 255 - its deviations wrap: Tracker
    /// shows NaN or a finite wrong value where MoTrack reports the correct deviation (MoTrack #232).
    bool trackerSumOverflow = false;
};

/// `RGBStep.getRGBData`: statistics of the pixels whose centres lie inside `shape` placed at
/// `position` (image pixels). The caller resolves Tracker's fixed-position/fixed-shape key frames
/// and passes the frame Tracker would measure; see docs/image-sampling.md.
[[nodiscard]] RgbRegionMeasurement measureRgbRegion(const ArgbImage& image, const RgbRegionShape& shape, ImagePoint position);

} // namespace motrack::science
