#pragma once

#include "legacy_project.hpp"

#include <optional>
#include <string>

namespace motrack
{

struct WorldPoint
{
    double x;
    double y;
};

/// Transforms a saved image-pixel mark to world coordinates at its original frame.
[[nodiscard]] std::optional<WorldPoint> worldPosition(const Calibration& calibration, const Mark& mark);

/// Exports the saved point-mass marks that fall inside the clip. Unknown world coordinates stay blank.
[[nodiscard]] std::string measurementsCsv(const LegacyProject& project);

} // namespace motrack
