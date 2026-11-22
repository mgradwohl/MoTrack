#pragma once

#include "legacy_project.hpp"

namespace motrack
{
class SourceEdits;
}

namespace motrack::osp
{
class Source;
}

namespace motrack
{
/// Validate the bounded schema used for audited identity edits, including the final emitted document.
void validateTrackerEditSchema(const osp::Source& xml);
/// Explicit movie adapter audit, also used by the movie identity/membership path.
void validateTrackerMovieEditSchema(const osp::Source& xml);
/// Internal conservative mass/vector rename patcher. Updates baseline names only after validating every source relationship.
[[nodiscard]] bool
patchTrackerNames(SourceEdits& xml, const LegacyProject& project, LegacyProject& baseline, const TrackerSource& source, bool movie);
/// Audit independent-track membership edits; deleted tracks may have selection, plot or table-column/format references.
/// Validates original schemas and resolved links without mutating source content.
void validateTrackerTrackMembership(const osp::Source& xml, const LegacyProject& project, const TrackerSource& source, bool movie);
/// Clear deleted selections, choose surviving view targets, and remove deleted plot links or table column/format owners.
/// Call after live-name patches and after appending created tracks; membership must already be audited.
void patchTrackerDeletedReferences(SourceEdits& xml, const LegacyProject& project, const TrackerSource& source);
} // namespace motrack
