#pragma once

#include "science/sampling/argb_image.hpp"

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace motrack::science
{

/// `LineProfile.MAX_SPREAD`: a profile averages at most 100 pixels on each side of its line.
inline constexpr int LINE_PROFILE_MAX_SPREAD = 100;

/// `LineProfile.setSpread` clamps the spread to [0, LINE_PROFILE_MAX_SPREAD].
[[nodiscard]] int clampLineProfileSpread(int spread) noexcept;

/// The two sampling algorithms of `LineProfileStep.getProfileData`.
enum class LineProfileMode : std::uint8_t
{
    /// Whole pixels of one image row (`getHorizontalProfileData`).
    Horizontal,
    /// Area-weighted 1-pixel probes along an arbitrary direction (`getTiltedProfileData`).
    Tilted,
};

/// Tracker treats the x-axis as horizontal when |sin(angle)| is below this value.
inline constexpr double LINE_PROFILE_HORIZONTAL_SINE = 1e-5;

/// Tracker's choice: horizontal when the profile's orientation is "horizontal" or when the
/// coordinate system's x-axis angle (radians, counter-clockwise) has |sin| < 1e-5; otherwise the
/// line follows the rotated x-axis and is sampled with the tilted algorithm.
[[nodiscard]] LineProfileMode lineProfileMode(bool alwaysHorizontal, double axisAngle) noexcept;

/// Tilted profiles longer than this many pixels are rejected as invalid geometry.
inline constexpr double LINE_PROFILE_MAX_LENGTH = 1 << 24;

/// Column names and order of Tracker's line-profile data.
inline constexpr std::array<std::string_view, 8> LINE_PROFILE_COLUMNS = {"n", "x", "y", "R", "G", "B", "luma", "pixels"};

/// One profile sample (one row of Tracker's profile table).
struct LineProfileSample
{
    /// `n`: the sample index along the line, from 0.
    int index = 0;
    /// `x`, `y`: the sample centre in image pixels. The coordinate system maps it to world units.
    ImagePoint position;
    /// Mean 8-bit channel values (0-255) across the profile width.
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    /// `trackerLuma` of the means.
    double luma = 0.0;
    /// `pixels`: the profile width 2 * spread + 1 (the number of pixels averaged).
    double pixels = 0.0;
};

enum class LineProfileStatus : std::uint8_t
{
    Measured,
    /// Horizontal: both ends truncate to the same pixel column. Tilted: shorter than one pixel.
    TooShort,
    /// Horizontal: the row band [y - spread, y + spread] or the column span leaves the image.
    /// Tilted: no 1-pixel probe of full width fits inside the image.
    OutsideImage,
    /// NaN or infinite end points, a spread outside [0, 100] or a tilted line longer than
    /// LINE_PROFILE_MAX_LENGTH (MoTrack-only rejections; Tracker never saves these).
    InvalidGeometry,
};

struct LineProfileData
{
    LineProfileStatus status = LineProfileStatus::TooShort;
    std::vector<LineProfileSample> samples;
    /// Horizontal profiles whose every pixel is pure black: Tracker 6.3.5 treats an all-zero
    /// raster as "image failed" and shows no data; MoTrack reports the zero values (MoTrack #235).
    bool trackerOmitsBlackStrip = false;
};

/// `LineProfileStep.getProfileData` for the line from `end0` to `end1` (image pixels).
///
/// Horizontal mode truncates the end x coordinates toward zero (Java `(int)`), clamps them to
/// [0, width] and samples the columns in between (the end column itself excluded) on row
/// `(int) end0.y`, averaging rows y - spread to y + spread with equal weights. Samples are centred
/// on pixel centres; `end1.y` is ignored.
///
/// Tilted mode steps a probe one pixel long and 2 * spread + 1 pixels wide from `end0` toward
/// `end1`, first advancing the start (and shortening the far end) by whole pixels until the probe
/// corners lie inside the image, exactly as Tracker does. Each sample is the mean of the pixels
/// weighted by their exact overlap area with the probe; Tracker's own area computation is
/// incorrect for directions with sin * cos < 0, downward vertical lines and some probes that
/// start on a pixel corner (MoTrack #233), and fails on other grid-aligned probes
/// (MoTrack #234), so MoTrack's weights are the exact geometric ones (docs/image-sampling.md).
[[nodiscard]] LineProfileData sampleLineProfile(const ArgbImage& image, ImagePoint end0, ImagePoint end1, int spread, LineProfileMode mode);

} // namespace motrack::science
