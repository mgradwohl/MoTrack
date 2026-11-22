#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace motrack::science
{

// Qt-free port of OSP's `org.opensourcephysics.media.core.TemplateMatcher` (OSP commit d56207b0, the
// source of the pinned Tracker 6.3.5 JAR), driven the way Tracker's `AutoTracker` drives it. See
// docs/template-matching.md for the algorithm, the oracle and the deliberate differences.
//
// Coordinates are image pixels: x to the right, y down, and pixel (i, j) covers [i, i + 1) x [j, j + 1).
// Every function is deterministic, keeps no global mutable state, and may be called concurrently on
// distinct or const objects.

/// A packed 0xAARRGGBB pixel, the layout of Java's `BufferedImage.TYPE_INT_ARGB`.
using ArgbPixel = std::uint32_t;

/// A whole-pixel position.
struct PixelPoint
{
    int x = 0;
    int y = 0;

    friend bool operator==(const PixelPoint&, const PixelPoint&) = default;
};

/// A whole-pixel rectangle like `java.awt.Rectangle`: columns [x, x + width) and rows [y, y + height).
struct PixelRect
{
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    friend bool operator==(const PixelRect&, const PixelRect&) = default;
};

/// A row-major image of packed pixels, owned by value. Video frames are treated as opaque RGB: the
/// matcher ignores their alpha byte, as Tracker does after converting frames to `TYPE_INT_RGB`.
class ArgbImage
{
  public:
    ArgbImage() = default;
    /// Throws std::invalid_argument when a dimension is negative or the pixel count exceeds Java's
    /// array limit (`INT_MAX`).
    ArgbImage(int width, int height, ArgbPixel fill = 0);
    /// Adopts `pixels` (row-major); throws std::invalid_argument unless it holds width * height pixels.
    ArgbImage(int width, int height, std::vector<ArgbPixel> pixels);

    [[nodiscard]] int width() const noexcept;
    [[nodiscard]] int height() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    /// Precondition: 0 <= x < width() and 0 <= y < height().
    [[nodiscard]] ArgbPixel pixel(int x, int y) const noexcept;
    /// Precondition: 0 <= x < width() and 0 <= y < height().
    void setPixel(int x, int y, ArgbPixel value) noexcept;
    [[nodiscard]] std::span<const ArgbPixel> pixels() const noexcept;
    [[nodiscard]] std::span<ArgbPixel> pixels() noexcept;

    friend bool operator==(const ArgbImage&, const ArgbImage&) = default;

  private:
    int m_Width = 0;
    int m_Height = 0;
    std::vector<ArgbPixel> m_Pixels;
};

/// AutoTracker's two mask shapes (`Ellipse2D.Double` or `Rectangle2D.Double`).
enum class MaskShape : std::uint8_t
{
    Ellipse,
    Rectangle,
};

/// The template mask: the frame of the mask shape in image pixel coordinates, as AutoTracker stores it.
struct MatchMask
{
    MaskShape shape = MaskShape::Ellipse;
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;

    /// `RectangularShape.setFrameFromCenter`: half extents |corner - center| on each axis.
    [[nodiscard]] static MatchMask fromCenter(MaskShape shape, double centerX, double centerY, double cornerX, double cornerY) noexcept;

    friend bool operator==(const MatchMask&, const MatchMask&) = default;
};

/// `RectangularShape.getBounds()`: floor of the minimum and ceiling of the maximum; an empty rectangle
/// at the origin when a size is negative. Nothing for non-finite frames or bounds outside `int`.
[[nodiscard]] std::optional<PixelRect> frameBounds(double x, double y, double width, double height) noexcept;

/// The mask's whole-pixel bounds (`mask.getBounds()`), the template's untrimmed extent.
[[nodiscard]] std::optional<PixelRect> maskBounds(const MatchMask& mask) noexcept;

/// The pixels of the mask bounds a template includes, row-major over `maskBounds(mask)` (1 = included).
/// As in TemplateMatcher, a pixel is included only when all four of its corners lie inside the mask,
/// using `java.awt.geom.Path2D` containment of the mask translated to the bounds origin. An ellipse is
/// therefore Java's four-cubic Bezier approximation, and boundaries follow Path2D's half-open rules.
/// Empty when the bounds are not representable.
[[nodiscard]] std::vector<std::uint8_t> maskPixels(const MatchMask& mask);

/// The image TemplateMatcher compares: the mask-bounds template trimmed to the included pixels.
/// Included pixels are opaque (alpha 255); excluded pixels are 0 and do not contribute to scores.
struct MatchTemplate
{
    ArgbImage image;
    /// Columns and rows trimmed from the left and top of the mask bounds (`trimLeft`, `trimTop`).
    int trimLeft = 0;
    int trimTop = 0;

    friend bool operator==(const MatchTemplate&, const MatchTemplate&) = default;
};

/// A template and its evolution state, as one Tracker `TemplateMatcher` per AutoTracker key frame.
class TemplateMatcher
{
  public:
    /// Masks whose bounds exceed this many pixels are refused (AutoTracker limits masks to 99 x 99).
    static constexpr int MAX_TEMPLATE_PIXELS = 1 << 22;

    /// `AutoTracker.createTemplateMatcher`: crops the mask bounds from `frame` (black where they leave
    /// the frame) and builds the template from it at full opacity. Nothing when the bounds are empty,
    /// unrepresentable or too large, or when the mask includes no pixel.
    ///
    /// Runs synchronously without cancellation: one recursive Path2D containment test per pixel corner,
    /// about 10^4 for AutoTracker's largest (99 x 99) mask. Callers that accept masks from users (C6)
    /// must enforce that wizard limit (TRACKER_MAX_MASK_SIZE) rather than rely on MAX_TEMPLATE_PIXELS.
    [[nodiscard]] static std::optional<TemplateMatcher> create(const ArgbImage& frame, const MatchMask& mask);

    [[nodiscard]] const MatchMask& mask() const noexcept;
    /// The mask bounds in the key frame; also the size of every template-building crop.
    [[nodiscard]] PixelRect bounds() const noexcept;
    /// The current template (`getTemplate()`).
    [[nodiscard]] const MatchTemplate& matchTemplate() const noexcept;
    /// The opaque key-frame crop the template is tethered to (`original`).
    [[nodiscard]] const ArgbImage& original() const noexcept;
    /// The working image the template is cut from; excluded pixels are 0 (`getWorkingPixels()`).
    [[nodiscard]] const ArgbImage& working() const noexcept;
    /// `getAlphas()`: {alphaInput, alphaOriginal} as passed to the latest build, unclamped; {255, 0}
    /// after creation.
    [[nodiscard]] std::array<int, 2> alphas() const noexcept;

    /// `buildTemplate(newVideoImage(rect), alphaInput, alphaOriginal)` for the mask-bounds-sized rect at
    /// `origin`: overlays the crop at opacity alphaInput / 255 (evolution) and then the original at
    /// alphaOriginal / 255 (tethering) onto the working image with Java2D's 8-bit SrcOver arithmetic,
    /// clears excluded pixels and cuts the new template. Alphas are clamped to 0..255. When both are 0
    /// nothing changes, `alphas()` included, and false is returned.
    bool evolve(const ArgbImage& frame, PixelPoint origin, int alphaInput, int alphaOriginal);

  private:
    TemplateMatcher() = default;
    void rebuild();

    MatchMask m_Mask;
    PixelRect m_Bounds;
    PixelRect m_Trimmed;
    std::vector<std::uint8_t> m_Included;
    ArgbImage m_Original;
    ArgbImage m_Working;
    MatchTemplate m_Template;
    std::array<int, 2> m_Alphas{255, 0};
};

/// Why a search produced no match location.
enum class MatchStatus : std::uint8_t
{
    Matched,        ///< A best position was found; its score decides whether it is acceptable.
    OutsideImage,   ///< The search area leaves no position where the template fits in the frame.
    NoSearchPoints, ///< A line search found no position on the line inside the search area.
    Cancelled,      ///< MatchOptions::cancelled requested a stop.
};

/// The outcome of one search (`getMatchLocation` and `getMatchWidthAndHeight`).
struct MatchResult
{
    MatchStatus status = MatchStatus::OutsideImage;
    /// The search area after Tracker moved it into the frame, inset so that the whole template fits.
    /// Searched positions are those of the trimmed template's pixel (width / 2, height / 2).
    PixelRect searchArea;
    /// Mask-bounds origin of the best whole-pixel match.
    PixelPoint pixel;
    /// Subpixel mask-bounds origin: `pixel` plus the parabolic peak offset (Tracker's returned point).
    /// AutoTracker's match center is this plus the key frame's mask center minus its bounds origin.
    double x = 0.0;
    double y = 0.0;
    /// Tracker's match score: mean / minimum squared RGB difference - 1 over the searched positions,
    /// divided by the peak width when that exceeds 1. +inf for a perfect match; NaN when no position was
    /// searched or every position matches perfectly.
    double peakHeight = std::numeric_limits<double>::quiet_NaN();
    /// Width of the parabolic peak fit (mean of both axes in 2D), NaN when not fitted.
    double peakWidth = std::numeric_limits<double>::quiet_NaN();
    /// The best position is on the boundary of the searched positions (the outermost row or column of
    /// the area, or the first or last line pixel), so the target may lie outside the search area.
    /// Tracker does not report this.
    bool atSearchEdge = false;
};

/// Coarse cancellation for long searches.
struct MatchOptions
{
    /// Polled before each searched column (2D) or block of line points; true abandons the search.
    std::function<bool()> cancelled;
};

/// `getMatchLocation(frame, searchArea, null, ...)`: exhaustive search of every position in the area.
/// MoTrack fits the subpixel peak with true neighbor scores on all four sides and never moves it
/// beyond the neighbors (see docs/template-matching.md, deliberate differences); away from the
/// boundary of the area the result equals Tracker's bit for bit.
[[nodiscard]] MatchResult
findMatch(const MatchTemplate& matchTemplate, const ArgbImage& frame, PixelRect searchArea, const MatchOptions& options = {});

/// Tracker's coordinate-system x-axis, along which one-dimensional AutoTracking searches: through
/// (x0, y0) at `angle` radians counterclockwise, so its image direction is (cos angle, -sin angle).
struct SearchLine
{
    double x0 = 0.0;
    double y0 = 0.0;
    double angle = 0.0;
};

/// `TemplateMatcher.getSearchPoints`: the pixels the line crosses inside the rectangle, relative to its
/// origin and ordered by increasing x (by y for a vertical line). Nothing when the line does not cross
/// the rectangle between two distinct edge points.
[[nodiscard]] std::optional<std::vector<PixelPoint>> lineSearchPoints(PixelRect searchArea, const SearchLine& line);

/// One-dimensional search along `line` inside `searchArea` (AutoTracker with "1D" tracking). MoTrack
/// computes the line pixels for the adjusted area, reports no match instead of searching in 2D or
/// failing when no line pixel is inside it, and takes the subpixel offset as the along-line component
/// of a two-dimensional peak fit; see docs/template-matching.md for the reference defects this avoids.
[[nodiscard]] MatchResult findMatchAlongLine(const MatchTemplate& matchTemplate,
                                             const ArgbImage& frame,
                                             PixelRect searchArea,
                                             const SearchLine& line,
                                             const MatchOptions& options = {});

/// AutoTracker's final 1D step: moves an image point (a match center) perpendicularly onto `line`.
[[nodiscard]] std::array<double, 2> projectOntoLine(const SearchLine& line, double x, double y) noexcept;

/// AutoTracker's defaults and limits (AutoTracker.java and its wizard).
inline constexpr int TRACKER_DEFAULT_GOOD_MATCH = 4; ///< "Accept" score; the wizard offers 1..10.
inline constexpr int TRACKER_POSSIBLE_MATCH = 1;     ///< Below this a search reports no match.
inline constexpr int TRACKER_MAX_GOOD_MATCH = 10;
inline constexpr int TRACKER_DEFAULT_EVOLVE_PERCENT = 20;
inline constexpr int TRACKER_DEFAULT_TETHER_PERCENT = 5;
inline constexpr int TRACKER_MAX_EVOLVE_PERCENT = 100;
inline constexpr int TRACKER_MAX_TETHER_PERCENT = 100;
inline constexpr int TRACKER_DEFAULT_MASK_SIZE = 9;
inline constexpr int TRACKER_MIN_MASK_RADIUS = 4;
inline constexpr int TRACKER_MAX_MASK_SIZE = 99;
inline constexpr int TRACKER_DEFAULT_SEARCH_SIZE = 40;

/// How AutoTracker treats a search result.
enum class MatchQuality : std::uint8_t
{
    None,     ///< No location, or a score below the possible-match threshold.
    Possible, ///< Shown for review but not marked (includes NaN scores, which compare false).
    Good,     ///< Marked automatically and used to evolve the template.
};

/// AutoTracker's tests: `p == null || score < possibleMatch` is no match; an infinite score or
/// `score >= goodMatch` is good.
[[nodiscard]] MatchQuality
classifyMatch(const MatchResult& result, int goodMatch = TRACKER_DEFAULT_GOOD_MATCH, int possibleMatch = TRACKER_POSSIBLE_MATCH) noexcept;

/// The wizard's evolution/tether percent to alpha: `(int) (2.55 * percent)` clamped to 0..255. The
/// truncation makes 100% evolve with alpha 254.
[[nodiscard]] int alphaFromPercent(int percent) noexcept;

/// Where AutoTracker crops the evolution image after a good match: `(int) Math.round` of the result.
[[nodiscard]] PixelPoint evolutionOrigin(const MatchResult& result) noexcept;

} // namespace motrack::science
