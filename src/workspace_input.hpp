#pragma once

#include "workspace_session.hpp"

namespace motrack
{

/// An idle source prepared off the owner thread. Exactly one session is present; no live jobs or views.
/// Native group files retain their exact saved membership; standard archives prepare every experiment.
struct PreparedWorkspaceInput
{
    std::unique_ptr<DocumentSession> document;
    std::unique_ptr<ArchiveGroupSession> group;
    QString mediaProjectPath;
};

/// Read a local .trk, .trz or .motrack with bounded archive/native readers and cancellation boundaries.
/// Never changes a live registry. A malformed archive member rejects the entire prepared source.
/// Legacy standalone native archive companions stay standalone; no member association is invented.
[[nodiscard]] PreparedWorkspaceInput prepareWorkspaceInput(const QString& path, const std::atomic_bool* cancelled = nullptr);
/// Owner-thread transfer. Refusal leaves incoming owned by the caller and existing sources unchanged.
[[nodiscard]] WorkspaceSession::OpenedSource adoptWorkspaceInput(WorkspaceSession& workspace,
                                                                 PreparedWorkspaceInput& incoming,
                                                                 WorkspaceActivation activation = WorkspaceActivation::SelectNew);

} // namespace motrack
