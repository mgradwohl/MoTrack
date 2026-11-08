#pragma once

#include "legacy_project.hpp"

class QDomDocument;

namespace motrack
{
/// Internal conservative mass/vector rename patcher. Updates baseline names only after validating every source relationship.
[[nodiscard]] bool patchTrackerNames(QDomDocument& xml, const LegacyProject& project, LegacyProject& baseline, const TrackerSource& source);
} // namespace motrack
