#pragma once

#include "legacy_project.hpp"

#include <QString>

namespace motrack
{

/// Patch audited media-free track, clip/calibration and unit edits, including creation and deletion with saved-view cleanup.
/// Unsupported edits are refused before output. Native-only relationships are not silently discarded.
/// Requires known source bindings. Unknown XML objects/properties are retained; the baseline stays unchanged.
[[nodiscard]] std::string renderTrackerProject(const LegacyProject& project, const TrackerSource& source);
/// Atomically write validated standard .trk XML. Does not write .trz or mark a native session saved.
void writeTrackerProject(const QString& path, const LegacyProject& project, const TrackerSource& source);

} // namespace motrack
