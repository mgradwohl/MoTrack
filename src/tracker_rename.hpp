#pragma once

#include "legacy_project.hpp"

class QDomDocument;

namespace motrack
{
/// Validate the bounded schema used for audited identity edits, including the final emitted document.
void validateTrackerEditSchema(const QDomDocument& xml);
/// Explicit movie adapter audit, also used by the movie identity/membership path.
void validateTrackerMovieEditSchema(const QDomDocument& xml);
/// Internal conservative mass/vector rename patcher. Updates baseline names only after validating every source relationship.
[[nodiscard]] bool
patchTrackerNames(QDomDocument& xml, const LegacyProject& project, LegacyProject& baseline, const TrackerSource& source, bool movie);
/// Audit independent-track membership edits; deleted tracks may have selection, plot or table-column/format references.
/// Validates original schemas and resolved links without mutating source content.
void validateTrackerTrackMembership(const QDomDocument& xml, const LegacyProject& project, const TrackerSource& source, bool movie);
/// Clear deleted selections, choose surviving view targets, and remove deleted plot links or table column/format owners.
/// Call after live-name patches and before removing original track nodes; membership must already be audited.
void patchTrackerDeletedReferences(QDomDocument& xml, const LegacyProject& project, const TrackerSource& source);
} // namespace motrack
