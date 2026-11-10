#include "archive_group_session.hpp"

#include "tracker_archive.hpp"

#include <algorithm>
#include <utility>

namespace motrack
{
namespace
{

void require(bool valid)
{
    if (!valid)
        throw ProjectError("Invalid live archive group.");
}

void requireTransferable(const std::unique_ptr<DocumentSession>& incoming)
{
    require(incoming && !incoming->sourcePath().isEmpty() && incoming->recoveryPath().isEmpty());
}

} // namespace

void ArchiveGroupSession::replace(ArchiveGroup incoming, const QString& path, ArchiveGroupSource source)
{
    require((source == ArchiveGroupSource::Native && path.endsWith(".motrack", Qt::CaseInsensitive)) ||
            (source == ArchiveGroupSource::Tracker && path.endsWith(".trz", Qt::CaseInsensitive)));
    validateArchiveGroupMembership(incoming);
    SaveState state;
    state.reset(path);
    std::vector<Member> members;
    members.reserve(incoming.members.size());
    for (auto& member : incoming.members)
    {
        auto session = std::make_unique<DocumentSession>();
        session->replace(std::move(member.document), path);
        members.push_back({.id = member.id, .member = std::move(member.member), .session = std::move(session)});
    }
    m_Id = incoming.id;
    m_Archive = std::move(incoming.archive);
    m_Members = std::move(members);
    m_SaveState = std::move(state);
    m_MembershipRevision = 0;
    m_SavedMembership.fill(0);
    m_InheritedPairedSaving = false;
}

QUuid ArchiveGroupSession::promote(std::unique_ptr<DocumentSession>& incoming)
{
    requireTransferable(incoming);
    auto prepared = makeArchiveGroup(incoming->document);
    SaveState state;
    // A standalone .motrack is not an existing group target. Its conversion requires Save As.
    const auto kind =
        incoming->sourcePath().endsWith(".trz", Qt::CaseInsensitive) ? DocumentSourceKind::Project : DocumentSourceKind::Media;
    state.reset(incoming->sourcePath(), kind);
    std::vector<Member> members;
    const auto id = prepared.members.front().id;
    members.push_back({.id = id, .member = std::move(prepared.members.front().member), .session = {}});
    const bool paired = incoming->requiresPairedSave();
    members.front().session = std::move(incoming);
    m_Id = prepared.id;
    m_Archive = std::move(prepared.archive);
    m_Members = std::move(members);
    m_SaveState = std::move(state);
    m_MembershipRevision = 0;
    m_SavedMembership.fill(std::nullopt);
    m_InheritedPairedSaving = paired;
    return id;
}

QUuid ArchiveGroupSession::append(std::unique_ptr<DocumentSession>& incoming)
{
    requireTransferable(incoming);
    require(m_Archive && std::cmp_less(m_Members.size(), m_Archive->projects().size()));
    auto prepared = makeArchiveGroup(incoming->document);
    const auto& member = prepared.members.front();
    require(prepared.archive == m_Archive &&
            std::ranges::none_of(m_Members,
                                 [&](const auto& existing) { return existing.id == member.id || existing.member == member.member; }));
    const auto id = member.id;
    // Allocate before transferring ownership, so a refused/failed append cannot discard the incoming edits.
    m_Members.push_back({.id = id, .member = member.member, .session = {}});
    m_InheritedPairedSaving = m_InheritedPairedSaving || incoming->requiresPairedSave();
    m_Members.back().session = std::move(incoming);
    ++m_MembershipRevision;
    return id;
}

void ArchiveGroupSession::replaceMember(QUuid id, WorkspaceDocument incoming)
{
    auto& member = findMember(id);
    auto prepared = makeArchiveGroup(std::move(incoming));
    require(prepared.archive == m_Archive && prepared.members.front().member == member.member);
    member.session->replace(std::move(prepared.members.front().document), sourcePath());
    ++m_MembershipRevision;
}

const ArchiveGroupSession::Member& ArchiveGroupSession::findMember(QUuid id) const
{
    const auto member = std::ranges::find(m_Members, id, &Member::id);
    require(member != m_Members.end());
    return *member;
}

ArchiveGroupSession::Member& ArchiveGroupSession::findMember(QUuid id)
{
    const auto member = std::ranges::find(m_Members, id, &Member::id);
    require(member != m_Members.end());
    return *member;
}

std::vector<QUuid> ArchiveGroupSession::memberIds() const
{
    std::vector<QUuid> result;
    result.reserve(m_Members.size());
    for (const auto& member : m_Members)
        result.push_back(member.id);
    return result;
}

const WorkspaceDocument& ArchiveGroupSession::document(QUuid id) const
{
    return findMember(id).session->document;
}

LegacyProject& ArchiveGroupSession::project(QUuid id)
{
    return findMember(id).session->document.project;
}

TrackEdits& ArchiveGroupSession::history(QUuid id)
{
    return findMember(id).session->history;
}

bool ArchiveGroupSession::restoreSavedMember(QUuid id)
{
    auto& member = findMember(id);
    return recoveryPath().isEmpty() && member.session->restoreSavedProject(requiresPairedSave());
}

bool ArchiveGroupSession::canRestoreSavedMember(QUuid id) const
{
    return recoveryPath().isEmpty() && findMember(id).session->canRestoreSavedProject(requiresPairedSave());
}

bool ArchiveGroupSession::requiresPairedSave() const
{
    return m_InheritedPairedSaving || m_SaveState.requiresPairedSave();
}

bool ArchiveGroupSession::isModified() const
{
    if (m_Members.empty())
        return false;
    const bool paired = requiresPairedSave();
    if (m_SavedMembership[0] != m_MembershipRevision ||
        (paired && (m_SavedMembership[1] != m_MembershipRevision || !recoveryPath().isEmpty())))
        return true;
    return std::ranges::any_of(m_Members,
                               [paired](const auto& member) {
                                   return member.session->history.isModified() ||
                                          (paired && member.session->history.isModified(SaveTarget::Tracker));
                               });
}

const QString& ArchiveGroupSession::sourcePath() const
{
    return m_SaveState.sourcePath();
}

const QString& ArchiveGroupSession::nativeSavePath() const
{
    return m_SaveState.nativeSavePath();
}

const QString& ArchiveGroupSession::standardSavePath() const
{
    return m_SaveState.standardSavePath();
}

const QString& ArchiveGroupSession::recoveryPath() const
{
    return m_SaveState.recoveryPath();
}

ArchiveGroupSession::SaveCheckpoint ArchiveGroupSession::saveCheckpoint() const
{
    SaveCheckpoint result{.group = m_SaveState.saveCheckpoint(m_MembershipRevision), .members = {}};
    result.members.reserve(m_Members.size());
    for (const auto& member : m_Members)
        result.members.push_back({.id = member.id, .session = member.session->saveCheckpoint()});
    return result;
}

ArchiveGroupSession::SaveRequest ArchiveGroupSession::prepareSave() const
{
    require(m_Archive && !m_Members.empty());
    ArchiveGroup snapshot{.id = m_Id, .archive = m_Archive, .members = {}};
    snapshot.members.reserve(m_Members.size());
    for (const auto& member : m_Members)
        snapshot.members.push_back({.id = member.id, .member = member.member, .document = member.session->document});
    return {.snapshot = std::move(snapshot), .checkpoint = saveCheckpoint()};
}

ArchiveGroupSession::SavedProjects ArchiveGroupSession::captureSavedProjects(const SaveCheckpoint& checkpoint,
                                                                             const ArchiveGroup* snapshot) const
{
    SavedProjects saved;
    saved.reserve(static_cast<qsizetype>(m_Members.size()));
    QHash<QUuid, const MemberCheckpoint*> captured;
    captured.reserve(static_cast<qsizetype>(checkpoint.members.size()));
    for (const auto& member : checkpoint.members)
        captured.insert(member.id, &member);
    QHash<QUuid, const LegacyProject*> originals;
    if (snapshot)
    {
        originals.reserve(static_cast<qsizetype>(snapshot->members.size()));
        for (const auto& member : snapshot->members)
            originals.insert(member.id, &member.document.project);
    }
    for (const auto& member : m_Members)
    {
        const auto* checkpointMember = captured.value(member.id);
        if (!checkpointMember)
            continue;
        const auto* project = originals.value(member.id);
        if (snapshot && !project)
            continue;
        saved.insert(member.id, member.session->captureSavedProject(checkpointMember->session, project));
    }
    return saved;
}

SaveOutcome ArchiveGroupSession::applyUpdate(const SaveCheckpoint& checkpoint, const SaveUpdate& update, const SavedProjects& snapshots)
{
    bool staleMember = false;
    const auto apply = [&](SaveChange change, SaveTarget target)
    {
        if (change == SaveChange::Unchanged)
            return;
        auto& saved = m_SavedMembership.at(static_cast<std::size_t>(target));
        if (change == SaveChange::Saved)
            saved = m_MembershipRevision;
        else
            saved.reset();
        for (const auto& member : m_Members)
        {
            const auto captured = std::ranges::find(checkpoint.members, member.id, &MemberCheckpoint::id);
            const auto current = member.session->saveCheckpoint();
            const bool currentMember =
                captured != checkpoint.members.end() && captured->session.identity == current.identity &&
                captured->session.revision == current.revision &&
                (target == SaveTarget::Native ? captured->session.nativeInvalidation == current.nativeInvalidation
                                              : captured->session.trackerInvalidation == current.trackerInvalidation);
            if (change == SaveChange::Saved && currentMember)
                member.session->history.markSaved(target);
            else
            {
                member.session->history.invalidateSaved(target);
                staleMember = true;
            }
        }
    };
    apply(update.native, SaveTarget::Native);
    apply(update.tracker, SaveTarget::Tracker);
    for (const auto& member : m_Members)
        member.session->applySavedProject(update, snapshots.value(member.id));
    // Group writes retain archive-relative bindings; an empty worker media map cannot replace member maps.
    return staleMember && update.outcome == SaveOutcome::Saved ? SaveOutcome::Stale : update.outcome;
}

SaveOutcome ArchiveGroupSession::completeSave(const SaveCheckpoint& checkpoint,
                                              const QString& destination,
                                              const NativeSaveResult& result,
                                              const ArchiveGroup* snapshot)
{
    if (m_Members.empty())
        return SaveOutcome::Replaced;
    const auto saved = result.saved ? captureSavedProjects(checkpoint, snapshot) : SavedProjects{};
    return applyUpdate(checkpoint, m_SaveState.completeSave(checkpoint.group, m_MembershipRevision, destination, result), saved);
}

SaveOutcome
ArchiveGroupSession::completePairedSave(const SaveCheckpoint& checkpoint, const PairedSaveResult& result, const ArchiveGroup* snapshot)
{
    if (m_Members.empty())
        return SaveOutcome::Replaced;
    const auto saved = result.standardCurrent || result.nativeCurrent ? captureSavedProjects(checkpoint, snapshot) : SavedProjects{};
    return applyUpdate(checkpoint, m_SaveState.completePairedSave(checkpoint.group, m_MembershipRevision, result), saved);
}

SaveOutcome
ArchiveGroupSession::completePairedRecovery(const SaveCheckpoint& checkpoint, const QString& path, const PairedSaveResult& result)
{
    if (m_Members.empty())
        return SaveOutcome::Replaced;
    return applyUpdate(checkpoint, m_SaveState.completePairedRecovery(checkpoint.group, path, result));
}

} // namespace motrack
