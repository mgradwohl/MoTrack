#pragma once

#include "archive_group_session.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

namespace motrack
{

enum class WorkspaceActivation : std::uint8_t
{
    SelectNew,
    KeepCurrent
};

enum class WorkspaceDiscard : std::uint8_t
{
    RejectModified,
    Confirmed
};

enum class WorkspaceJob : std::uint8_t
{
    Frame,
    MediaIndex,
    AutoTrack
};

struct WorkspaceJobToken
{
    QUuid document;
    WorkspaceJob kind = WorkspaceJob::Frame;
    std::shared_ptr<std::atomic_bool> cancelled;
};

struct WorkspaceOperation
{
    QUuid source;
    QUuid id;
    std::shared_ptr<std::atomic_bool> cancelled;
};

struct WorkspaceDocumentSnapshot
{
    WorkspaceDocument document;
    QString mediaProjectPath;
    DocumentSession::SaveCheckpoint checkpoint;
};

struct WorkspaceSaveRequest
{
    WorkspaceOperation operation;
    std::variant<WorkspaceDocumentSnapshot, ArchiveGroupSession::SaveRequest> snapshot;
};

struct WorkspaceRecoveryRequest
{
    WorkspaceOperation operation;
    std::variant<DocumentSession::SaveCheckpoint, ArchiveGroupSession::SaveCheckpoint> checkpoint;
    QString path;
    QString transaction;
};

/// Owner-thread document registry, independent of widgets and worker pools. Views borrow records only
/// while their source remains owned; workers receive snapshots/tokens, never pointers to live records.
class WorkspaceSession
{
  public:
    struct OpenedSource
    {
        QUuid source;
        std::vector<QUuid> documents;
    };

    /// Transfer a prepared idle session. All allocations/validation precede ownership transfer.
    /// The media base is the original experiment path (the extracted XML path for an archive member).
    /// Caller must finish old standalone jobs/saves before adoption. Histories and pending journals survive.
    [[nodiscard]] OpenedSource addDocument(std::unique_ptr<DocumentSession>& incoming,
                                           const QString& mediaProjectPath,
                                           WorkspaceActivation activation = WorkspaceActivation::SelectNew);
    [[nodiscard]] OpenedSource addGroup(std::unique_ptr<ArchiveGroupSession>& incoming,
                                        WorkspaceActivation activation = WorkspaceActivation::SelectNew);
    [[nodiscard]] QUuid
    appendMember(QUuid source, std::unique_ptr<DocumentSession>& incoming, WorkspaceActivation activation = WorkspaceActivation::SelectNew);

    [[nodiscard]] const WorkspaceDocument& document(QUuid id) const;
    /// Edit only through this record's history; do not reset source/member bindings or history.
    [[nodiscard]] LegacyProject& project(QUuid id);
    [[nodiscard]] TrackEdits& history(QUuid id);
    [[nodiscard]] QUuid sourceId(QUuid document) const;
    [[nodiscard]] bool isModified(QUuid source) const;
    [[nodiscard]] bool requiresPairedSave(QUuid source) const;
    [[nodiscard]] const QString& nativeSavePath(QUuid source) const;
    [[nodiscard]] const QString& standardSavePath(QUuid source) const;
    [[nodiscard]] const QString& recoveryPath(QUuid source) const;

    [[nodiscard]] const std::vector<QUuid>& tabs() const noexcept;
    [[nodiscard]] std::optional<QUuid> activeDocument() const noexcept;
    void showDocument(QUuid id);
    /// Hide a view, retaining its record/history/jobs and its contribution to later group saves.
    /// This is not a user-facing discard operation; the UI must separately implement its close policy.
    void hideDocument(QUuid id);
    void moveDocument(QUuid id, std::size_t position);
    /// Requires all views hidden and destroyed, no pending save/recovery, and clean or confirmed discard.
    /// Cancels only this source's jobs. Does not delete any files or pending recovery records.
    [[nodiscard]] bool releaseSource(QUuid id, WorkspaceDiscard discard = WorkspaceDiscard::RejectModified);

    /// Supersedes only the same document/job kind. Selection and tab order never cancel jobs.
    [[nodiscard]] WorkspaceJobToken beginJob(QUuid document, WorkspaceJob kind);
    [[nodiscard]] bool accepts(const WorkspaceJobToken& token) const;
    [[nodiscard]] bool finishJob(const WorkspaceJobToken& token);
    void cancelJobs(QUuid document);
    void cancelSourceWork(QUuid source);

    /// At most two pending save/recovery operations across this workspace; busy returns nullopt
    /// before copying models. Capture only when ready to dispatch. Every request needs a completion,
    /// including failed/cancelled work, so its source cannot be destroyed while a write is outstanding.
    [[nodiscard]] std::optional<WorkspaceSaveRequest> beginSave(QUuid document);
    [[nodiscard]] std::optional<WorkspaceRecoveryRequest> beginRecovery(QUuid document);
    [[nodiscard]] SaveOutcome completeSave(const WorkspaceSaveRequest& request, const QString& destination, const NativeSaveResult& result);
    [[nodiscard]] SaveOutcome completePairedSave(const WorkspaceSaveRequest& request, const PairedSaveResult& result);
    [[nodiscard]] SaveOutcome completeRecovery(const WorkspaceRecoveryRequest& request, const PairedSaveResult& result);

    WorkspaceSession() = default;
    WorkspaceSession(const WorkspaceSession&) = delete;
    WorkspaceSession& operator=(const WorkspaceSession&) = delete;
    WorkspaceSession(WorkspaceSession&&) = delete;
    WorkspaceSession& operator=(WorkspaceSession&&) = delete;
    ~WorkspaceSession();

  private:
    struct Source
    {
        QUuid id;
        std::unique_ptr<DocumentSession> standalone;
        std::unique_ptr<ArchiveGroupSession> group;
        QString mediaProjectPath;
    };
    struct Entry
    {
        QUuid id;
        Source* source = nullptr; // Stable owner node; removed only after its entries and views.
        QUuid member;
        std::array<std::shared_ptr<std::atomic_bool>, 3> jobs{};
    };
    struct PendingOperation
    {
        WorkspaceOperation token;
        bool recovery = false;
    };
    [[nodiscard]] Entry& findDocument(QUuid id);
    [[nodiscard]] const Entry& findDocument(QUuid id) const;
    [[nodiscard]] Source& findSource(QUuid id);
    [[nodiscard]] const Source& findSource(QUuid id) const;
    void reserveAddition(std::size_t count);
    void selectAdded(QUuid id, WorkspaceActivation activation);
    [[nodiscard]] Source* operationSource(const WorkspaceOperation& token, bool recovery);
    void finishOperation(const WorkspaceOperation& token);

    std::vector<std::unique_ptr<Source>> m_Sources;
    std::vector<std::unique_ptr<Entry>> m_Documents;
    std::vector<QUuid> m_Tabs;
    std::optional<QUuid> m_Active;
    std::vector<PendingOperation> m_Operations;
};

/// Synchronous worker adapters. They operate only on owned snapshots and cancellation flags;
/// completion must return to the owner thread even if cancellation arrives after a physical commit.
[[nodiscard]] NativeSaveResult
writeWorkspaceNative(const WorkspaceSaveRequest& request, const QString& path, const PairedSaveObserver& observer = {});
[[nodiscard]] PairedSaveResult
writeWorkspacePair(const WorkspaceSaveRequest& request, const QString& path, const PairedSaveObserver& observer = {});
[[nodiscard]] PairedSaveResult recoverWorkspacePair(const WorkspaceRecoveryRequest& request, const PairedSaveObserver& observer = {});

} // namespace motrack
