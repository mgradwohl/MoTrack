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
/// local image references use its physical parent directory, resolving existing filesystem aliases before rebasing.
/// The source/native snapshot remains unchanged. Audited constant-rate linked movies permit existing mass/mark and clip-range edits
/// after native timestamp indexing; their original timing, calibration and other fields stay unchanged.
/// Archives, filters, movie rename/membership edits and numeric media timing edits are refused.
/// All linked files must be readable. Symbolic-link output files and unsafe generated references are refused.
/// No media is copied and no session save checkpoint is changed.
void writeTrackerDocument(const QString& path, const WorkspaceDocument& document, const QString& mediaProjectPath);
/// Export the selected original archive member, retaining all other members and embedded media.
/// Requires the owned source payload; replacing embedded media or the selected source is refused.
/// Audited image sequences and fully indexed constant-rate MOV files retain the standalone writer's edit limits.
void writeTrackerArchiveDocument(const QString& path, const WorkspaceDocument& document, const std::atomic_bool* cancelled = nullptr);
/// Create a portable .trz from an audited linked-image document, copying each image without conversion.
/// The document and its native/source bindings stay unchanged. Existing archives, movies and unsupported edits are refused.
/// Stage and validate the bounded package before atomic replacement; cancellation leaves the destination unchanged.
/// PNG, JPEG and BMP frames are decoded before replacement, limited to 32 Mi pixels each. Other formats are refused.
/// Implemented by the motrack_image_package media adapter; the project model does not link image decoders.
void writeTrackerImagePackage(const QString& path,
                              const WorkspaceDocument& document,
                              const QString& mediaProjectPath,
                              const std::atomic_bool* cancelled = nullptr);

} // namespace motrack
