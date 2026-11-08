#pragma once

#include "legacy_project.hpp"

class QDomDocument;

namespace motrack
{
/// Validate the bounded schema used for audited identity edits, including the final emitted document.
void validateTrackerEditSchema(const QDomDocument& xml);
/// Internal conservative mass/vector rename patcher. Updates baseline names only after validating every source relationship.
[[nodiscard]] bool patchTrackerNames(QDomDocument& xml, const LegacyProject& project, LegacyProject& baseline, const TrackerSource& source);
/// Audit original schemas and resolved links before adding independent tracks; no source mutation.
void validateTrackerTrackCreation(const QDomDocument& xml, const LegacyProject& project, const TrackerSource& source);
} // namespace motrack
