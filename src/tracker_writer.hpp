#pragma once

#include "legacy_project.hpp"

#include <QString>

#include <atomic>

namespace motrack
{
struct WorkspaceDocument;

/// Patch audited media-free track, clip/calibration and unit edits, including creation and deletion with saved-view cleanup.
/// Unsupported edits are refused before output. Native-only relationships are not silently discarded.
/// Requires known source bindings. Unknown XML objects/properties are retained; the baseline stays unchanged.
[[nodiscard]] std::string renderTrackerProject(const LegacyProject& project, const TrackerSource& source);
/// Atomically write validated standard .trk XML. Does not write .trz or mark a native session saved.
void writeTrackerProject(const QString& path, const LegacyProject& project, const TrackerSource& source);
/// Atomically export a standalone document. mediaProjectPath is the absolute current project file path;
/// local image references are resolved relative to its parent directory.
/// The source/native snapshot remains unchanged. Movies, archives, filters and media timing/range edits are refused.
/// All linked files must be readable; no media is copied and no session save checkpoint is changed.
void writeTrackerDocument(const QString& path, const WorkspaceDocument& document, const QString& mediaProjectPath);
/// Export the selected original archive member, retaining all other members and embedded media.
/// Requires the owned source payload; replacing embedded media or the selected source is refused.
void writeTrackerArchiveDocument(const QString& path, const WorkspaceDocument& document, const std::atomic_bool* cancelled = nullptr);

} // namespace motrack
