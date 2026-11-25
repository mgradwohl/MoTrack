#pragma once

#include "frame_time_base.hpp"
#include "legacy_project.hpp"

#include <QHash>
#include <QString>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace motrack
{

class TrackerArchive;

/// Immutable runtime index retained with a document, independent of decoders and widgets.
/// Absolute presentation timestamps are for seeking; movieFrameTimes remains zero-origin seconds.
/// Never serialized. Adapters validate the physical file and orientation before reusing it.
struct MovieTimeline
{
    QString physicalPath;
    qint64 fileSize = 0;
    qint64 modifiedMsecs = 0;
    bool displayOrientation = false;
    int frameCount = 0;
    std::vector<std::chrono::nanoseconds> presentationTimes;
};

struct NativeSaveResult
{
    bool saved = false;
    std::uint64_t observation = 0;               // Process-local completion order shared with paired saves.
    QHash<QString, QString> mediaLocations = {}; // Original-reference bindings; never serialized.
};

struct WorkspaceDocument
{
    LegacyProject project;
    /// Decoded movie frame times in seconds from the first frame, indexed by original frame number.
    std::vector<double> movieFrameTimes;
    /// The movie stream's time base those times were quantized to, with the precision inferred from the
    /// timestamps (#335); unknown for documents saved before #331 (the precision before #335) until the movie is
    /// indexed again. Persisted with the frame times as `movieTimeBase` and `movieTimestampPrecision`.
    FrameTimeBase movieTimeBase = {};
    bool movie = false;
    bool useDisplayOrientation = false;
    bool alignMovieToClip = false;
    std::string archiveReference = {};
    std::optional<TrackerSource> trackerSource = std::nullopt;
    std::shared_ptr<const TrackerArchive> archivePayload = {};
    std::optional<std::string> archiveMember = std::nullopt;
    // Runtime source locations captured when opening a desktop document. Never serialized.
    QHash<QString, QString> mediaLocations = {};
    /// Runtime: opening dropped source bindings that no longer name the preserved source's tracks, so standard
    /// export is unavailable until the original Tracker file is imported again. Never serialized.
    bool trackerBindingsReset = false;
    /// The reading of `trackerSource` that the typed values follow, serialized as `trackerReading`. A native file
    /// that saves none was written by TrackerReading::SavedIndices; opening migrates it when that is safe.
    TrackerReading trackerReading = CURRENT_TRACKER_READING;
    /// Runtime: the typed values follow an earlier reading of the source and could not be migrated without overwriting
    /// edits or losing marks, so standard export is unavailable (#313/#314). Re-derived on every open.
    bool trackerReadingOutdated = false;
    /// Runtime: opening migrated values of an earlier reading to the current one and so changed measurements; a native
    /// save records the current reading. Never serialized.
    bool trackerReadingMigrated = false;
    /// Runtime: why the current reader refuses to import the preserved source, for example because Tracker cannot load
    /// it either (#315), or empty. The stored values then open native-only and unchanged, with the source associations
    /// dropped and the earlier `trackerReading` kept, so standard export is unavailable. Re-derived on every open.
    std::string trackerSourceRefusal = {};
    std::shared_ptr<const MovieTimeline> movieTimeline = {};
};

/// Retains physical media locations before the original project directory can disappear.
/// Call when adopting an external document; clear/rebuild when rebinding its media.
void retainWorkspaceMediaLocations(WorkspaceDocument& document, const QString& sourcePath);
/// Refresh restored aliases against captured physical bases in a private worker snapshot.
/// Successful save completion may adopt these runtime bindings without changing serialized references.
void refreshWorkspaceMediaLocations(WorkspaceDocument& document, const QString& sourcePath);
/// Resolves a retained reference, falling back to the supplied base for uncaptured references.
[[nodiscard]] QString workspaceMediaPath(const WorkspaceDocument& document, const QString& sourcePath, const std::string& reference);

/// Imports a standalone Tracker XML file (or one already extracted archive member), retaining its source.
[[nodiscard]] WorkspaceDocument importTrackerDocument(const QString& path);
/// Reads a versioned MoTrack document. Media paths are relative to the document.
[[nodiscard]] WorkspaceDocument readWorkspaceDocument(const QString& path, const std::atomic_bool* cancelled = nullptr);
/// Rebase a private snapshot for native saving. Archive members remain archive-root-relative.
/// Uses physical project directories and verifies every generated local reference; throws before writing on failure.
void rebaseWorkspaceMedia(WorkspaceDocument& document, const QString& sourcePath, const QString& destinationPath);
/// Writes atomically. The caller supplies media paths relative to the destination.
/// Low-level storage only: live-session saves use writeNativeDocument to serialize replacement and observation.
/// Cancellation is checked before serialization, while streaming archive data and before replacement.
void writeWorkspaceDocument(const QString& path, const WorkspaceDocument& document, const std::atomic_bool* cancelled = nullptr);

} // namespace motrack
