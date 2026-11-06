#pragma once

#include "legacy_project.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
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

/// Resolves the effective calibration for an original frame, including fixed fields.
[[nodiscard]] std::optional<CalibrationKey> calibrationAt(const Calibration& calibration, int frame);

/// Transforms a saved image-pixel mark to world coordinates at its original frame.
[[nodiscard]] std::optional<WorldPoint> worldPosition(const Calibration& calibration, const Mark& mark);

/// Exports the saved point-mass marks that fall inside the clip. Unknown world coordinates stay blank.
[[nodiscard]] std::string measurementsCsv(const LegacyProject& project);

/// Uses saved constant timing when available. Velocity excludes clip edges and gaps in the marks.
[[nodiscard]] PlotSeries measurementPlot(const LegacyProject& project, std::size_t track, PlotQuantity quantity);

} // namespace motrack
