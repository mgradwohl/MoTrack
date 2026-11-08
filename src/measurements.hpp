#pragma once

#include "legacy_project.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace motrack
{

struct WorldPoint
{
    double x;
    double y;
};

enum class PlotQuantity : std::uint8_t
{
    PositionX,
    PositionY,
    VelocityX,
    VelocityY,
    Speed,
    AccelerationX,
    AccelerationY,
    MomentumX,
    MomentumY,
    KineticEnergy,
};

struct PlotSample
{
    int frame;
    double horizontal;
    double vertical;
};

struct PlotSeries
{
    std::vector<PlotSample> samples;
    bool timed = false;
    bool calibrated = false;
    std::string timeUnit = "s";
};

struct LinearFit
{
    double slope;
    double intercept;
    std::optional<double> rSquared;
};

/// Resolves the effective calibration for an original frame, including fixed fields.
[[nodiscard]] std::optional<CalibrationKey> calibrationAt(const Calibration& calibration, int frame);

/// Transforms a saved image-pixel mark to world coordinates at its original frame.
[[nodiscard]] std::optional<WorldPoint> worldPosition(const Calibration& calibration, const Mark& mark);
[[nodiscard]] std::optional<double> pointDistance(WorldPoint first, WorldPoint second);
[[nodiscard]] std::optional<double> interiorAngleDegrees(WorldPoint vertex, WorldPoint first, WorldPoint second);

/// Checks a media-free constant timebase over original frames (0..500000), in milliseconds.
/// Every frame must have a finite, strictly increasing time both in milliseconds and seconds.
[[nodiscard]] bool validConstantClipTiming(const Clip& clip);

/// Timing edits/history require media-free projects with no unverified model or data time source.
/// Point masses, independent vectors and retained axes/protractors are currently supported.
[[nodiscard]] bool supportsConstantClipTimingEdits(const LegacyProject& project);

/// Exports the saved point-mass marks that fall inside the clip. Unknown world coordinates stay blank.
[[nodiscard]] std::string measurementsCsv(const LegacyProject& project);

/// Frame times are seconds indexed by original frame number. Saved constant timing is the fallback.
[[nodiscard]] PlotSeries measurementPlot(const LegacyProject& project,
                                         std::size_t track,
                                         PlotQuantity quantity,
                                         std::span<const double> frameTimes = {},
                                         double presentationOffsetSeconds = 0.0);

/// Exports the exact samples displayed in a plot, including the source frame.
[[nodiscard]] std::string plotCsv(const PlotSeries& series, std::string_view quantity);

/// Fits a straight line to finite plotted samples with at least two distinct horizontal values.
[[nodiscard]] std::optional<LinearFit> fitStraightLine(std::span<const PlotSample> samples);

} // namespace motrack
