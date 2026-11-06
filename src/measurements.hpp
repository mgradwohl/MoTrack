#pragma once

#include "legacy_project.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
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

/// Exports the saved point-mass marks that fall inside the clip. Unknown world coordinates stay blank.
[[nodiscard]] std::string measurementsCsv(const LegacyProject& project);

/// Frame times are seconds indexed by original frame number. Saved constant timing is the fallback.
[[nodiscard]] PlotSeries
measurementPlot(const LegacyProject& project, std::size_t track, PlotQuantity quantity, std::span<const double> frameTimes = {});

/// Fits a straight line to finite plotted samples with at least two distinct horizontal values.
[[nodiscard]] std::optional<LinearFit> fitStraightLine(std::span<const PlotSample> samples);

} // namespace motrack
