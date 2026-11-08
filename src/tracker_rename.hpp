#pragma once

#include "legacy_project.hpp"

class QDomDocument;

namespace motrack
{
/// Internal conservative point-mass rename patcher. Updates baseline names only after validating every source relationship.
[[nodiscard]] bool
patchTrackerPointMassNames(QDomDocument& xml, const LegacyProject& project, LegacyProject& baseline, const TrackerSource& source);
} // namespace motrack
