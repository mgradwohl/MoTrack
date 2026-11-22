#pragma once

#include "workspace_session.hpp"

#include <QStringList>

namespace motrack
{

/// An idle source prepared off the owner thread. Exactly one session is present; no live jobs or views.
/// Native group files retain their exact saved membership; standard archives prepare every readable experiment.
struct PreparedWorkspaceInput
{
    std::unique_ptr<DocumentSession> document;
    std::unique_ptr<ArchiveGroupSession> group;
    QString mediaProjectPath;
    /// Standard-archive .trk members that are not readable Tracker projects. They open no experiment;
    /// their bytes stay in the archive and in saved copies, as Tracker keeps its other tabs.
    QStringList unreadableProjects;
};

/// Read a local .trk, .trz or .motrack with bounded archive/native readers and cancellation boundaries.
/// Never changes a live registry. An archive opens when at least one project is readable; unsafe
/// project or referenced media names, invalid media and malformed native files reject the entire source.
/// Legacy standalone native archive companions stay standalone; no member association is invented.
/// This adapter probes/indexes available movies off the owner thread, including members without views.
[[nodiscard]] PreparedWorkspaceInput prepareWorkspaceInput(const QString& path, const std::atomic_bool* cancelled = nullptr);
/// New native experiments from local media. Preserve physical references; media files are never save targets.
[[nodiscard]] PreparedWorkspaceInput prepareWorkspaceMovie(const QString& path, const std::atomic_bool* cancelled = nullptr);
[[nodiscard]] PreparedWorkspaceInput
prepareWorkspaceImages(const QStringList& paths, double frameRate, const std::atomic_bool* cancelled = nullptr);
/// Owner-thread transfer. Refusal leaves incoming owned by the caller and existing sources unchanged.
[[nodiscard]] WorkspaceSession::OpenedSource adoptWorkspaceInput(WorkspaceSession& workspace,
                                                                 PreparedWorkspaceInput& incoming,
                                                                 WorkspaceActivation activation = WorkspaceActivation::SelectNew);

} // namespace motrack
