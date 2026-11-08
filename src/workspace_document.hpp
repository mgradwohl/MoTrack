#pragma once

#include "legacy_project.hpp"

#include <QString>

#include <atomic>
#include <memory>

namespace motrack
{

class TrackerArchive;

struct WorkspaceDocument
{
    LegacyProject project;
    std::vector<double> movieFrameTimes;
    bool movie = false;
    bool useDisplayOrientation = false;
    bool alignMovieToClip = false;
    std::string archiveReference = {};
    std::optional<TrackerSource> trackerSource = std::nullopt;
    std::shared_ptr<const TrackerArchive> archivePayload = {};
    std::optional<std::string> archiveMember = std::nullopt;
};

/// Imports a standalone Tracker XML file (or one already extracted archive member), retaining its source.
[[nodiscard]] WorkspaceDocument importTrackerDocument(const QString& path);
/// Reads a versioned MoTrack document. Media paths are relative to the document.
[[nodiscard]] WorkspaceDocument readWorkspaceDocument(const QString& path, const std::atomic_bool* cancelled = nullptr);
/// Rebase a private snapshot for native saving. Archive members remain archive-root-relative.
/// Uses physical project directories and verifies every generated local reference; throws before writing on failure.
void rebaseWorkspaceMedia(WorkspaceDocument& document, const QString& sourcePath, const QString& destinationPath);
/// Writes atomically. The caller supplies media paths relative to the destination.
void writeWorkspaceDocument(const QString& path, const WorkspaceDocument& document);

} // namespace motrack
