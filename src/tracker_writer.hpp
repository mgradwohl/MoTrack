#pragma once

#include "legacy_project.hpp"

#include <QString>

namespace motrack
{

/// Patch mass/marks, vectors, clip ranges and fixed calibration/length units in media-free projects; reject other edits.
/// Requires known source bindings. Unknown XML objects/properties are retained; the baseline stays unchanged.
[[nodiscard]] std::string renderTrackerProject(const LegacyProject& project, const TrackerSource& source);
/// Atomically write validated standard .trk XML. Does not write .trz or mark a native session saved.
void writeTrackerProject(const QString& path, const LegacyProject& project, const TrackerSource& source);

} // namespace motrack
