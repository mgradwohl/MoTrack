#pragma once

#include "legacy_project.hpp"

class QDomDocument;

namespace motrack
{
/// Internal conservative mass/vector rename patcher. Updates baseline names only after validating every source relationship.
[[nodiscard]] bool patchTrackerNames(QDomDocument& xml, const LegacyProject& project, LegacyProject& baseline, const TrackerSource& source);
/// Audit independent-track membership edits; deleted tracks may have selection references only.
/// Validates original schemas and resolved links without mutating source content.
void validateTrackerTrackMembership(const QDomDocument& xml, const LegacyProject& project, const TrackerSource& source);
/// Clear deleted panel selections and apply Tracker's first-viewable-track fallback to saved plot/table selections.
/// Call after live-name patches and before removing original track nodes; membership must already be audited.
void patchTrackerDeletedSelections(QDomDocument& xml, const LegacyProject& project, const TrackerSource& source);
} // namespace motrack
