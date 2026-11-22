#pragma once

// Ports of the OSP video filters that Tracker 6.3.5 registers. Parameter names follow the XML property
// each OSP Loader saves; ranges are what the setters enforce (Tracker's inspectors restrict them further).
// docs/video-filters.md describes every filter, its state and seek rules and its coordinate transform.

#include "science/filters/video_filter.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace motrack::science
{

/// A point in image space: pixels, x right and y down, pixel (i, j) covering [i, i + 1) x [j, j + 1).
struct FilterPoint
{
    double x = 0.0;
    double y = 0.0;

    constexpr FilterPoint() noexcept = default;
    constexpr FilterPoint(double x, double y) noexcept : x(x), y(y)
    {}

    friend bool operator==(const FilterPoint&, const FilterPoint&) = default;
};

/// Each channel c becomes 255 - c. No parameters.
class NegativeFilter final : public VideoFilter
{
  public:
    [[nodiscard]] std::unique_ptr<VideoFilter> clone() const override;

    NegativeFilter() noexcept : VideoFilter(VideoFilterKind::Negative)
    {}

  protected:
    void render(const FilterImage& input, FilterImage& output, int videoFrame) override;
};

/// gray = (int)((wr r + wg g + wb b) / (wr + wg + wb)), Java int bit packing for any weights ("weights").
class GrayScaleFilter final : public VideoFilter
{
  public:
    [[nodiscard]] std::unique_ptr<VideoFilter> clone() const override;

    /// Tracker's "video" weights (the default) and its "flat" weights.
    static constexpr std::array<double, 3> VIDEO_WEIGHTS{0.30, 0.59, 0.11};
    static constexpr std::array<double, 3> FLAT_WEIGHTS{1.0 / 3, 1.0 / 3, 1.0 / 3};

    GrayScaleFilter() noexcept : VideoFilter(VideoFilterKind::GrayScale)
    {}

    void setWeights(double red, double green, double blue) noexcept;

    [[nodiscard]] std::array<double, 3> weights() const noexcept
    {
        return m_Weights;
    }

  protected:
    void render(const FilterImage& input, FilterImage& output, int videoFrame) override;

  private:
    std::array<double, 3> m_Weights = VIDEO_WEIGHTS;
    double m_WeightSum = (VIDEO_WEIGHTS[0] + VIDEO_WEIGHTS[1]) + VIDEO_WEIGHTS[2];
};

/**
 * Brightness/contrast ("brightness", int; "contrast", double). With theta = pi contrast / 200, each channel
 * becomes clamp((int)(tan(theta) (c + sin^2(theta) brightness - 127) + 127 + cos^2(theta) brightness),
 * 0, 255), computed as OSP does with slope = sin / cos. Tracker's inspector offers brightness -128..128
 * and contrast 0..100; the setters accept any value.
 *
 * FilterArithmetic::Corrected (the default) truncates `value + 1e-9` instead of `value`, so that results
 * Java computes a few ulps below an exact integer are not lowered by one level (#220): at the default
 * contrast 50 the mapping is exactly c + brightness, yet the reference's slope is 0.9999999999999999 and
 * brightness 128 lowers 64 of the 256 channel values. Tracker635 reproduces the reference. Both use
 * correctly rounded sin and cos, which is what HotSpot's x86-64 Math.sin/cos returned for every probed
 * contrast, so neither depends on the platform C library.
 */
class BrightnessFilter final : public VideoFilter
{
  public:
    [[nodiscard]] std::unique_ptr<VideoFilter> clone() const override;

    static constexpr int DEFAULT_BRIGHTNESS = 0;
    static constexpr double DEFAULT_CONTRAST = 50.0;

    BrightnessFilter() noexcept;

    void setBrightness(int brightness) noexcept;
    void setContrast(double contrast) noexcept;

    [[nodiscard]] int brightness() const noexcept
    {
        return m_Brightness;
    }

    [[nodiscard]] double contrast() const noexcept
    {
        return m_Contrast;
    }

    void setArithmetic(FilterArithmetic arithmetic) noexcept
    {
        m_Arithmetic = arithmetic;
    }

    [[nodiscard]] FilterArithmetic arithmetic() const noexcept
    {
        return m_Arithmetic;
    }

    /// Tracker's Reset button: restores the default brightness and contrast.
    void clear() override;

  protected:
    void render(const FilterImage& input, FilterImage& output, int videoFrame) override;

  private:
    void updateFactors() noexcept;

    int m_Brightness = DEFAULT_BRIGHTNESS;
    double m_Contrast = DEFAULT_CONTRAST;
    double m_Slope = 1.0;
    double m_Offset1 = 0.0;
    double m_Offset2 = 0.0;
    FilterArithmetic m_Arithmetic = FilterArithmetic::Corrected;
};

/**
 * Highlight/shadow ("level", int; "grayscale", bool). A 256-entry logarithmic lookup maps the HSB
 * brightness (single-precision java.awt.Color conversions), or with "grayscale" the integer mean
 * (r + g + b) / 3, which then becomes grey. Level 0 is the identity; the inspector offers -100..100.
 */
class LogFilter final : public VideoFilter
{
  public:
    [[nodiscard]] std::unique_ptr<VideoFilter> clone() const override;

    LogFilter() noexcept;

    void setLevel(int level) noexcept;

    [[nodiscard]] int level() const noexcept
    {
        return m_Level;
    }

    void setGrayscale(bool grayscale) noexcept
    {
        m_Grayscale = grayscale;
    }

    [[nodiscard]] bool isGrayscale() const noexcept
    {
        return m_Grayscale;
    }

    [[nodiscard]] std::span<const std::int32_t, 256> lookup() const noexcept
    {
        return m_Lookup;
    }

  protected:
    void render(const FilterImage& input, FilterImage& output, int videoFrame) override;

  private:
    int m_Level = 0;
    bool m_Grayscale = false;
    std::array<std::int32_t, 256> m_Lookup{};
};

/// Keeps one field ("field": "odd" or "even", the default) and writes each kept row twice; an odd final
/// row is copied unchanged. The kept rows are 0, 2, 4... for even and 1, 3, 5... for odd.
class DeinterlaceFilter final : public VideoFilter
{
  public:
    [[nodiscard]] std::unique_ptr<VideoFilter> clone() const override;

    DeinterlaceFilter() noexcept : VideoFilter(VideoFilterKind::Deinterlace)
    {}

    void setOddField(bool odd) noexcept
    {
        m_Odd = odd;
    }

    [[nodiscard]] bool isOddField() const noexcept
    {
        return m_Odd;
    }

  protected:
    void render(const FilterImage& input, FilterImage& output, int videoFrame) override;

  private:
    bool m_Odd = false;
};

/**
 * Ghost trail of bright objects ("fade", |fade| limited to 1, default 0.05). Per pixel it remembers a grey
 * value g, starting with the first frame's (r + g + b) / 3; each evaluation fades it to (int)((1 - fade) g)
 * and shows that grey where it is brighter than the input's value (then remembering it), otherwise the
 * input pixel (remembering its value). Enabling, disabling or clearing restarts from the next frame.
 */
class GhostFilter : public VideoFilter
{
  public:
    [[nodiscard]] std::unique_ptr<VideoFilter> clone() const override;

    static constexpr double DEFAULT_FADE = 0.05;

    GhostFilter() noexcept : GhostFilter(VideoFilterKind::Ghost)
    {}

    void setFade(double fade) noexcept;

    [[nodiscard]] double fade() const noexcept
    {
        return m_Fade;
    }

    /// Applies to the dark ghost's fade toward white only (#281).
    void setArithmetic(FilterArithmetic arithmetic) noexcept
    {
        m_Arithmetic = arithmetic;
    }

    [[nodiscard]] FilterArithmetic arithmetic() const noexcept
    {
        return m_Arithmetic;
    }

    void setEnabled(bool enabled) override;
    void clear() override;

  protected:
    explicit GhostFilter(VideoFilterKind kind) noexcept : VideoFilter(kind)
    {}

    void initialize(const FilterImage& input) override;
    void render(const FilterImage& input, FilterImage& output, int videoFrame) override;

  private:
    double m_Fade = DEFAULT_FADE;
    FilterArithmetic m_Arithmetic = FilterArithmetic::Corrected;
    std::vector<std::int32_t> m_Values;
};

/// The dark-object ghost: grey values fade toward white and show where they are darker than the input.
/// Corrected (the default) fades to 255 - (int)((1 - fade)(255 - g)); Tracker635 keeps OSP's
/// (int)(255 - (1 - fade)(255 - g)), which never reaches white (#281).
class DarkGhostFilter final : public GhostFilter
{
  public:
    [[nodiscard]] std::unique_ptr<VideoFilter> clone() const override;

    DarkGhostFilter() noexcept : GhostFilter(VideoFilterKind::DarkGhost)
    {}
};

/**
 * Strobe trail ("fade", |fade| limited to 1, default 0; "bright_trails", default false). It remembers the
 * previous output; a faded copy of it (per channel) replaces the input pixel where the faded mean is
 * brighter (bright trails) or darker (dark trails) than the input's mean. The first frame initializes it.
 * Enabling, disabling, clearing or switching the trail type restarts from the next frame. Dark trails fade
 * toward white as the dark ghost does, corrected by default (#281).
 */
class StrobeFilter final : public VideoFilter
{
  public:
    [[nodiscard]] std::unique_ptr<VideoFilter> clone() const override;

    StrobeFilter() noexcept : VideoFilter(VideoFilterKind::Strobe)
    {}

    void setFade(double fade) noexcept;

    [[nodiscard]] double fade() const noexcept
    {
        return m_Fade;
    }

    /// Switching also clears (StrobeFilter.setBrightTrails).
    void setBrightTrails(bool bright) noexcept;

    [[nodiscard]] bool hasBrightTrails() const noexcept
    {
        return m_BrightTrails;
    }

    /// Applies to dark trails' fade toward white only (#281).
    void setArithmetic(FilterArithmetic arithmetic) noexcept
    {
        m_Arithmetic = arithmetic;
    }

    [[nodiscard]] FilterArithmetic arithmetic() const noexcept
    {
        return m_Arithmetic;
    }

    void setEnabled(bool enabled) override;
    void clear() override;

  protected:
    void initialize(const FilterImage& input) override;
    void render(const FilterImage& input, FilterImage& output, int videoFrame) override;

  private:
    double m_Fade = 0.0;
    bool m_BrightTrails = false;
    FilterArithmetic m_Arithmetic = FilterArithmetic::Corrected;
    std::vector<std::uint32_t> m_Previous;
};

/**
 * Sum/average. It sums each channel over images: the first evaluation's input, then the input of the
 * first evaluation after each addNextImage() (Tracker calls it on every step-number change, forward or
 * backward, even while disabled). Output channels are (int)min(sum f, 255) with f = 1 / count when
 * showing the mean, otherwise the brightness fraction (|fraction|, default 1). Tracker saves neither
 * setting. Clear restarts the sum from the last input and resets the fraction to 1.
 */
class SumFilter final : public VideoFilter
{
  public:
    [[nodiscard]] std::unique_ptr<VideoFilter> clone() const override;

    SumFilter() noexcept : VideoFilter(VideoFilterKind::Sum)
    {}

    void setBrightness(double fraction) noexcept;

    [[nodiscard]] double brightness() const noexcept
    {
        return m_Brightness;
    }

    void setMean(bool mean) noexcept
    {
        m_Mean = mean;
    }

    [[nodiscard]] bool isMean() const noexcept
    {
        return m_Mean;
    }

    [[nodiscard]] int imageCount() const noexcept
    {
        return m_ImageCount;
    }

    void addNextImage() noexcept
    {
        m_SkipSum = false;
    }

    void clear() override;

  protected:
    void initialize(const FilterImage& input) override;
    void render(const FilterImage& input, FilterImage& output, int videoFrame) override;

  private:
    void restart(std::span<const std::uint32_t> pixels);
    void addPixels(std::span<const std::uint32_t> pixels);

    double m_Brightness = 1.0;
    bool m_Mean = false;
    bool m_SkipSum = true;
    int m_ImageCount = 1;
    std::vector<std::int32_t> m_Red;
    std::vector<std::int32_t> m_Green;
    std::vector<std::int32_t> m_Blue;
    std::vector<std::uint32_t> m_LastInput;
};

/**
 * Subtracts a baseline image channel by channel, clamping at 0 ("imagepath" in Tracker; the image itself
 * is supplied here). The baseline may be larger than the frame, whose size then selects its top-left
 * region. Without a baseline the input is copied.
 */
class BaselineFilter final : public VideoFilter
{
  public:
    [[nodiscard]] std::unique_ptr<VideoFilter> clone() const override;

    BaselineFilter() noexcept : VideoFilter(VideoFilterKind::Baseline)
    {}

    /// Accepts an image at least as large as the current input (any image before the first evaluation);
    /// returns false and keeps the previous baseline otherwise. A frame that later exceeds the baseline
    /// drops it, as Tracker does.
    bool setBaselineImage(FilterImage image);

    [[nodiscard]] const std::optional<FilterImage>& baselineImage() const noexcept
    {
        return m_Baseline;
    }

    /// Tracker's Clear button: removes the baseline.
    void clear() override;

  protected:
    void initialize(const FilterImage& input) override;
    void render(const FilterImage& input, FilterImage& output, int videoFrame) override;

  private:
    void readBaselinePixels();

    std::optional<FilterImage> m_Baseline;
    std::vector<std::uint32_t> m_BaselinePixels;
    int m_Width = 0;
    int m_Height = 0;
};

/**
 * Resize ("width_factor", "height_factor": |factor| limited to [0.01, 10], NaN kept as in OSP). The output
 * is (int)(w * wf) x (int)(h * hf) pixels drawn with Java2D's nearest-neighbour scaled blit; a 720 x 480
 * input at unit factors makes wf 0.889 (DV pixel aspect). The image-space transform is (x, y) -> (wf x,
 * hf y). Every factor change re-initializes, even to the same value.
 */
class ResizeFilter final : public VideoFilter
{
  public:
    [[nodiscard]] std::unique_ptr<VideoFilter> clone() const override;

    ResizeFilter() noexcept : VideoFilter(VideoFilterKind::Resize)
    {}

    void setWidthFactor(double factor) noexcept;
    void setHeightFactor(double factor) noexcept;

    [[nodiscard]] double widthFactor() const noexcept
    {
        return m_WidthFactor;
    }

    [[nodiscard]] double heightFactor() const noexcept
    {
        return m_HeightFactor;
    }

    [[nodiscard]] FilterPoint inputToOutput(FilterPoint point) const noexcept;
    [[nodiscard]] FilterPoint outputToInput(FilterPoint point) const noexcept;

  protected:
    std::pair<int, int> outputSize(int width, int height) override;
    void render(const FilterImage& input, FilterImage& output, int videoFrame) override;

  private:
    double m_WidthFactor = 1.0;
    double m_HeightFactor = 1.0;
};

/// Quarter turns with Tracker's saved names ("rotation": None, CCW, CW, 180).
enum class FilterRotation : std::uint8_t
{
    None,
    CounterClockwise90,
    Clockwise90,
    Rotate180,
};

[[nodiscard]] std::string_view filterRotationName(FilterRotation rotation) noexcept;
[[nodiscard]] std::optional<FilterRotation> filterRotationFromName(std::string_view name) noexcept;

/**
 * Rotate/reflect ("rotation"; "reverse", default false). Reversing mirrors left-right before rotating.
 * For a w x h input, with r = reversed ? (w - x, y) : (x, y), the image-space transforms are
 * CW (x, y) -> (h - y, x); CCW (x, y) -> (y, w - x); 180 (x, y) -> (w - x, h - y); quarter turns swap
 * the output size. A rotation change re-initializes; a reverse change does not.
 */
class RotateFilter final : public VideoFilter
{
  public:
    [[nodiscard]] std::unique_ptr<VideoFilter> clone() const override;

    RotateFilter() noexcept : VideoFilter(VideoFilterKind::Rotate)
    {}

    void setRotation(FilterRotation rotation) noexcept;

    [[nodiscard]] FilterRotation rotation() const noexcept
    {
        return m_Rotation;
    }

    void setReversed(bool reversed) noexcept
    {
        m_Reversed = reversed;
    }

    [[nodiscard]] bool isReversed() const noexcept
    {
        return m_Reversed;
    }

    /// Maps a point of a width x height input image to the output image, and back.
    [[nodiscard]] FilterPoint inputToOutput(FilterPoint point, int width, int height) const noexcept;
    [[nodiscard]] FilterPoint outputToInput(FilterPoint point, int width, int height) const noexcept;

  protected:
    std::pair<int, int> outputSize(int width, int height) override;
    void render(const FilterImage& input, FilterImage& output, int videoFrame) override;

  private:
    FilterRotation m_Rotation = FilterRotation::None;
    bool m_Reversed = false;
};

/// A 3 x 3 projective map, row-major, applied as OSP does: w = m6 x + m7 y + m8, then
/// ((m0 x + m1 y + m2) / w, (m3 x + m4 y + m5) / w), and a point with w == 0 maps to itself.
struct ProjectiveTransform
{
    std::array<double, 9> m{};

    [[nodiscard]] FilterPoint map(FilterPoint point) const noexcept;
};

/// The corners of a perspective plane: Tracker's corners 0..3, by default top-left, top-right,
/// bottom-right, bottom-left. The square-to-quad map sends (0,0), (1,0), (1,1), (0,1) to corners 0..3.
using PerspectiveQuad = std::array<FilterPoint, 4>;

enum class PerspectivePlane : std::uint8_t
{
    Input,
    Output,
};

/**
 * Perspective ("in_corners"/"out_corners" key frames, "fixed_in" default false, "fixed_out" default
 * true). Output pixel (i, j) samples the input at H(i, j), where H maps the output quad to the input quad
 * (OSP: square-to-quad(input) after quad-to-square(output)), using the same corner-origin convention for
 * both images; samples outside the input are black and the last row and column are not interpolated.
 *
 * Corners are key-framed by video frame number. A fixed plane uses key 0 for every frame; otherwise a frame
 * uses the latest key at or before it. Without input keys, the first evaluation sets both planes to
 * (w/4, h/4), (3w/4, h/4), (3w/4, 3h/4), (w/4, 3h/4) with integer division.
 *
 * FilterArithmetic::Corrected (the default) interpolates bilinearly. Tracker635 reproduces OSP, which
 * weights the right neighbour by the vertical fraction and the lower neighbour by the horizontal one (#219).
 */
class PerspectiveFilter final : public VideoFilter
{
  public:
    [[nodiscard]] std::unique_ptr<VideoFilter> clone() const override;

    PerspectiveFilter() : VideoFilter(VideoFilterKind::Perspective)
    {}

    /// Stores a key frame exactly as Tracker's loader does, regardless of the fixed flag.
    void loadKeyFrame(PerspectivePlane plane, int videoFrame, const PerspectiveQuad& quad);
    /// Moving corners in Tracker (Corner.setXY -> saveCorners): key 0 when fixed, else `videoFrame`.
    void setCorners(PerspectivePlane plane, int videoFrame, const PerspectiveQuad& quad);
    /// Tracker's key-frame deletion: removes the key used at `videoFrame`; key 0 cannot be removed.
    bool removeKeyFrame(PerspectivePlane plane, int videoFrame);
    /// Fixing a plane replaces its keys with the corners shown at `currentVideoFrame`, stored at key 0.
    void setFixed(PerspectivePlane plane, bool fixed, int currentVideoFrame);

    [[nodiscard]] bool isFixed(PerspectivePlane plane) const noexcept
    {
        return plane == PerspectivePlane::Input ? m_FixedInput : m_FixedOutput;
    }

    [[nodiscard]] std::vector<int> keyFrames(PerspectivePlane plane) const;
    /// The corners used at `videoFrame`; nullopt before any key exists. When no key precedes the frame,
    /// the first key is used (Tracker throws NullPointerException there, #222).
    [[nodiscard]] std::optional<PerspectiveQuad> corners(PerspectivePlane plane, int videoFrame) const;
    /// OSP's output-to-input matrix at `videoFrame`, and the matching input-to-output map.
    [[nodiscard]] std::optional<ProjectiveTransform> outputToInput(int videoFrame) const;
    [[nodiscard]] std::optional<ProjectiveTransform> inputToOutput(int videoFrame) const;

    void setArithmetic(FilterArithmetic arithmetic) noexcept
    {
        m_Arithmetic = arithmetic;
    }

    [[nodiscard]] FilterArithmetic arithmetic() const noexcept
    {
        return m_Arithmetic;
    }

  protected:
    void initialize(const FilterImage& input) override;
    void render(const FilterImage& input, FilterImage& output, int videoFrame) override;

  private:
    [[nodiscard]] std::map<int, PerspectiveQuad>& keys(PerspectivePlane plane) noexcept
    {
        return plane == PerspectivePlane::Input ? m_InputKeys : m_OutputKeys;
    }

    [[nodiscard]] const std::map<int, PerspectiveQuad>& keys(PerspectivePlane plane) const noexcept
    {
        return plane == PerspectivePlane::Input ? m_InputKeys : m_OutputKeys;
    }

    std::map<int, PerspectiveQuad> m_InputKeys;
    std::map<int, PerspectiveQuad> m_OutputKeys;
    bool m_FixedInput = false;
    bool m_FixedOutput = true;
    FilterArithmetic m_Arithmetic = FilterArithmetic::Corrected;
};

/// Lens projections with Tracker's saved names ("input_type"/"output_type").
enum class RadialProjection : std::uint8_t
{
    Rectilinear,
    Equisolid,
    Equidistant,
    Stereographic,
    Orthographic,
};

[[nodiscard]] std::string_view radialProjectionName(RadialProjection projection) noexcept;
[[nodiscard]] std::optional<RadialProjection> radialProjectionFromName(std::string_view name) noexcept;

/**
 * Radial distortion ("fixed_radius" in [0.2, 1] of the half diagonal, default 0.75; "input_fov", radians in
 * [pi/18, pi - 0.0005], default pi/2; "input_type"/"output_type", default Rectilinear; NaN rejected).
 * Output pixel (i, j) samples the input pixel containing c + s(r) (p - c) + (0.0001, 0.0001), rounded to the
 * nearer of its 2 x 2 neighbourhood as OSP does, with p = (i, j), c = (w / 2, h / 2), r = |p - c| and s the
 * projection stretch; outside the input is black.
 *
 * Evaluating may move the fixed radius to OSP's lower radius limit (up or down) when the output field of
 * view exceeds pi - 0.001 for a projection pair with a lower limit, as OSP's refreshScale does;
 * fixedRadius() then reports the value Tracker would save. The transcendental functions are the platform's;
 * the oracle's samples keep at least 5.8e-6 px from every rounding edge, so last-ulp differences from
 * Java do not change pixels.
 */
class RadialDistortionFilter final : public VideoFilter
{
  public:
    [[nodiscard]] std::unique_ptr<VideoFilter> clone() const override;

    RadialDistortionFilter() noexcept : VideoFilter(VideoFilterKind::RadialDistortion)
    {}

    void setFixedRadius(double fraction) noexcept;
    void setSourceFov(double radians) noexcept;
    void setSourceProjection(RadialProjection projection) noexcept;
    void setOutputProjection(RadialProjection projection) noexcept;

    [[nodiscard]] double fixedRadius() const noexcept
    {
        return m_FixedRadius;
    }

    [[nodiscard]] double sourceFov() const noexcept
    {
        return m_SourceFov;
    }

    [[nodiscard]] RadialProjection sourceProjection() const noexcept
    {
        return m_SourceProjection;
    }

    [[nodiscard]] RadialProjection outputProjection() const noexcept
    {
        return m_OutputProjection;
    }

    /// The output field of view of the last transform (radians); 0 before the first evaluation.
    [[nodiscard]] double outputFov() const noexcept
    {
        return m_OutputFov;
    }

    /// OSP's lowerRadiusLimit in percent of the half diagonal.
    [[nodiscard]] int lowerRadiusLimit() const noexcept
    {
        return m_LowerRadiusLimit;
    }

    /// The sample position for output point `point` of a width x height image, without the 0.0001 nudge.
    /// It uses the current parameters, as the next evaluation will, without changing the filter.
    [[nodiscard]] FilterPoint outputToInput(FilterPoint point, int width, int height) const;

  protected:
    void initialize(const FilterImage& input) override;
    void render(const FilterImage& input, FilterImage& output, int videoFrame) override;

  private:
    struct Scale
    {
        double fixedRadius;
        double outputFov;
        int lowerRadiusLimit;
    };

    [[nodiscard]] Scale refreshScale(Scale scale) const;
    [[nodiscard]] double stretch(double radius, double outputFov, double pixelsToCorner) const;

    double m_FixedRadius = 0.75;
    double m_SourceFov = 1.5707963267948966;
    RadialProjection m_SourceProjection = RadialProjection::Rectilinear;
    RadialProjection m_OutputProjection = RadialProjection::Rectilinear;
    bool m_HasLowerLimit = false;
    int m_LowerRadiusLimit = 20;
    double m_OutputFov = 0.0;
    bool m_ValidTransform = false;
    int m_TransformWidth = -1;
    int m_TransformHeight = -1;
    std::vector<std::int32_t> m_Sources; // input pixel per output pixel, -1 for black
};

} // namespace motrack::science
