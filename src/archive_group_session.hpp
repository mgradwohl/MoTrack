#pragma once

#include "archive_group.hpp"
#include "document_session.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace motrack
{

enum class ArchiveGroupSource : std::uint8_t
{
    Native,
    Tracker
};

/// Owner-thread records for one archive, independent of visible tabs. Closing a view does not remove a record.
/// Workers receive owned snapshots; only the owner thread applies completion actions.
class ArchiveGroupSession
{
  public:
    struct MemberCheckpoint
    {
        QUuid id;
        DocumentSession::SaveCheckpoint session;
    };
    struct SaveCheckpoint
    {
        SaveState::Checkpoint group;
        std::vector<MemberCheckpoint> members;
    };
    struct SaveRequest
    {
        ArchiveGroup snapshot;
        SaveCheckpoint checkpoint;
    };

    /// Install a prepared on-disk baseline. Tracker groups require .trz, native groups .motrack.
    /// This is for freshly loaded input, not transferring edited live sessions (use promote/append).
    /// Invalid input leaves the current group untouched.
    void replace(ArchiveGroup incoming, const QString& path, ArchiveGroupSource source = ArchiveGroupSource::Native);
    /// Transfer an idle standalone archive session, preserving history and member runtime identity.
    /// The caller must finish/cancel standalone jobs before transfer and route future saves through this group.
    /// Pending recovery is refused. On failure, incoming retains ownership; on success it becomes null.
    [[nodiscard]] QUuid promote(std::unique_ptr<DocumentSession>& incoming);
    /// Adopt another member from this exact archive owner. It remains retained after its view closes.
    [[nodiscard]] QUuid append(std::unique_ptr<DocumentSession>& incoming);
    /// Explicitly install a newly prepared version of the same member; invalidate membership save points.
    void replaceMember(QUuid id, WorkspaceDocument incoming);

    [[nodiscard]] std::vector<QUuid> memberIds() const;
    [[nodiscard]] const WorkspaceDocument& document(QUuid id) const;
    /// Use this project's history for edits. Do not reset history or mutate source/member bindings.
    [[nodiscard]] LegacyProject& project(QUuid id);
    [[nodiscard]] TrackEdits& history(QUuid id);
    [[nodiscard]] bool isModified() const;
    [[nodiscard]] bool requiresPairedSave() const;
    [[nodiscard]] const QString& sourcePath() const;
    [[nodiscard]] const QString& nativeSavePath() const;
    [[nodiscard]] const QString& standardSavePath() const;
    [[nodiscard]] const QString& recoveryPath() const;
    [[nodiscard]] SaveCheckpoint saveCheckpoint() const;
    /// Capture membership, all member revisions and frozen values in one owner-thread operation.
    [[nodiscard]] SaveRequest prepareSave() const;
    [[nodiscard]] SaveOutcome completeSave(const SaveCheckpoint& checkpoint, const QString& destination, const NativeSaveResult& result);
    [[nodiscard]] SaveOutcome completePairedSave(const SaveCheckpoint& checkpoint, const PairedSaveResult& result);
    [[nodiscard]] SaveOutcome completePairedRecovery(const SaveCheckpoint& checkpoint, const QString& path, const PairedSaveResult& result);

    ArchiveGroupSession() = default;
    ArchiveGroupSession(const ArchiveGroupSession&) = delete;
    ArchiveGroupSession& operator=(const ArchiveGroupSession&) = delete;
    ArchiveGroupSession(ArchiveGroupSession&&) = delete;
    ArchiveGroupSession& operator=(ArchiveGroupSession&&) = delete;
    ~ArchiveGroupSession() = default;

  private:
    struct Member
    {
        QUuid id;
        std::string member;
        std::unique_ptr<DocumentSession> session;
    };
    [[nodiscard]] const Member& findMember(QUuid id) const;
    [[nodiscard]] Member& findMember(QUuid id);
    SaveOutcome applyUpdate(const SaveCheckpoint& checkpoint, const SaveUpdate& update);

    QUuid m_Id;
    std::shared_ptr<const TrackerArchive> m_Archive;
    std::vector<Member> m_Members;
    SaveState m_SaveState;
    std::uint64_t m_MembershipRevision = 0;
    std::array<std::optional<std::uint64_t>, 2> m_SavedMembership{};
    bool m_InheritedPairedSaving = false;
};

} // namespace motrack
