#pragma once

#include "tracker_tabset.hpp"
#include "workspace_session.hpp"

#include <QList>
#include <QMetaType>
#include <QStringList>

#include <optional>
#include <vector>

namespace motrack
{

/// A standard-archive .trk member that opened no experiment, with the user-facing reason.
struct UnreadableProject
{
    QString member;
    QString reason;
    bool operator==(const UnreadableProject&) const = default;
};

/// An idle source prepared off the owner thread: exactly one session, or a Tracker tabset that owns no source
/// and lists the inputs to open next. No live jobs or views. Native group files retain their exact saved
/// membership; standard archives prepare every readable experiment.
struct PreparedWorkspaceInput
{
    std::unique_ptr<DocumentSession> document;
    std::unique_ptr<ArchiveGroupSession> group;
    QString mediaProjectPath;
    /// Archive order. Their bytes stay in the archive and in saved copies, as Tracker keeps a failed tab.
    QList<UnreadableProject> unreadableProjects;
    /// Expanded references of a `TFrame` .trk, in Tracker's open order; `mediaProjectPath` is the tabset file.
    std::optional<std::vector<TabsetItem>> tabset{};
};

/// Read a local .trk, .trz or .motrack with bounded archive/native readers and cancellation boundaries.
/// Never changes a live registry. An archive opens when at least one project is readable. A project
/// that fails to import or uses an unsafe member name is reported instead; an unsafe project name,
/// invalid media and malformed native files reject the entire source.
/// Legacy standalone native archive companions stay standalone; no member association is invented.
/// A tabset .trk is expanded against its directory and the process working directory, not opened.
/// This adapter probes/indexes available movies off the owner thread, including members without views.
[[nodiscard]] PreparedWorkspaceInput prepareWorkspaceInput(const QString& path, const std::atomic_bool* cancelled = nullptr);
/// New native experiments from local media. Preserve physical references; media files are never save targets.
[[nodiscard]] PreparedWorkspaceInput prepareWorkspaceMovie(const QString& path, const std::atomic_bool* cancelled = nullptr);
[[nodiscard]] PreparedWorkspaceInput
prepareWorkspaceImages(const QStringList& paths, double frameRate, const std::atomic_bool* cancelled = nullptr);
/// One media file listed by a tabset: a still image opens as a one-frame image sequence with unknown timing, as in
/// Tracker; an animated image or any other file opens as a movie. Inspects the file off the owner thread.
[[nodiscard]] PreparedWorkspaceInput prepareWorkspaceMediaFile(const QString& path, const std::atomic_bool* cancelled = nullptr);
/// Owner-thread transfer. Refusal leaves incoming owned by the caller and existing sources unchanged.
[[nodiscard]] WorkspaceSession::OpenedSource adoptWorkspaceInput(WorkspaceSession& workspace,
                                                                 PreparedWorkspaceInput& incoming,
                                                                 WorkspaceActivation activation = WorkspaceActivation::SelectNew);

} // namespace motrack

Q_DECLARE_METATYPE(motrack::UnreadableProject)
