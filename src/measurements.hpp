#pragma once

#include "frame_time_base.hpp"
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

/// Clip step of an original frame (its zero-based index among the clip's frames), or nullopt when the
/// frame is outside the clip. An unknown step count leaves the clip unbounded after its first frame.
[[nodiscard]] std::optional<int> clipStepOfFrame(const Clip& clip, int frame);

/// Original frame of a clip step, or nullopt for a step outside the clip or a frame beyond `int`.
[[nodiscard]] std::optional<int> clipFrameOfStep(const Clip& clip, int step);

/// Time of an original frame in milliseconds under the saved constant timing (start time plus the
/// frame offset times the frame duration), or nullopt when the clip has no positive frame duration.
[[nodiscard]] std::optional<double> clipFrameTimeMs(const Clip& clip, int frame);

/// Resolves the effective calibration for an original frame, including fixed fields.
[[nodiscard]] std::optional<CalibrationKey> calibrationAt(const Calibration& calibration, int frame);

/// Transforms a saved image-pixel mark to world coordinates at its original frame.
[[nodiscard]] std::optional<WorldPoint> worldPosition(const Calibration& calibration, const Mark& mark);
[[nodiscard]] std::optional<double> pointDistance(WorldPoint first, WorldPoint second);
[[nodiscard]] std::optional<double> interiorAngleDegrees(WorldPoint vertex, WorldPoint first, WorldPoint second);

/// Checks a media-free constant timebase over original frames (0..500000), in milliseconds.
/// Every frame must have a finite, strictly increasing time both in milliseconds and seconds.
/// Adjacent absolute timestamps must retain the requested interval within relative 1e-9.
[[nodiscard]] bool validConstantClipTiming(const Clip& clip);

/// Timing edits/history require media-free projects with no unverified model or data time source.
/// Point masses, independent vectors and retained axes/protractors are currently supported. The guard itself
/// belongs to the typed model (model::supportsConstantClipTimingEdits); this applies it to the legacy view.
[[nodiscard]] bool supportsConstantClipTimingEdits(const LegacyProject& project);

/// Exports the saved point-mass marks that fall inside the clip. Unknown world coordinates stay blank.
[[nodiscard]] std::string measurementsCsv(const LegacyProject& project);

/// Plots one quantity of a point mass against time (seconds, or frame numbers when untimed).
///
/// `frameTimes` are decoded movie times in seconds from the movie's first frame, indexed by original frame
/// number, and `frameTimeBase` is the stream time base they were quantized to. Without frame times the saved
/// constant timing applies. Positions are world coordinates in the project's length unit (image pixels when
/// uncalibrated), velocity is per second, acceleration per second squared, momentum and kinetic energy use
/// the point mass's mass. Sample frames are original frame numbers; horizontal values include
/// `presentationOffsetSeconds` for decoded times.
///
/// Derived quantities follow Tracker's default Finite Difference algorithm on the clip steps: velocity
/// (x[k+1] - x[k-1]) / 2 and acceleration (2x[k-2] - x[k-1] - 2x[k] - x[k+1] + 2x[k+2]) / 7 per step, scaled
/// by the mean step interval (motrack_motion's pointMassDerivatives). A value needs every clip step of its
/// window (three for velocity, five for acceleration); missing values stay absent, never zero. Where decoded
/// times are not evenly spaced within the time base, velocity is the time-aware three-point slope and
/// acceleration the time-aware five-point least-squares fit (measuredTimeDerivatives; #87, #331).
[[nodiscard]] PlotSeries measurementPlot(const LegacyProject& project,
                                         std::size_t track,
                                         PlotQuantity quantity,
                                         std::span<const double> frameTimes = {},
                                         double presentationOffsetSeconds = 0.0,
                                         FrameTimeBase frameTimeBase = {});

/// Exports the exact samples displayed in a plot, including the source frame.
[[nodiscard]] std::string plotCsv(const PlotSeries& series, std::string_view quantity);

/// Fits a straight line to finite plotted samples with at least two distinct horizontal values.
[[nodiscard]] std::optional<LinearFit> fitStraightLine(std::span<const PlotSample> samples);

} // namespace motrack
