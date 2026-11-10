#include "workspace_session.hpp"

#include "tracker_archive.hpp"

#include <QDir>

#include <algorithm>
#include <cmath>
#include <utility>

namespace motrack
{
namespace
{

constexpr std::size_t MAX_PENDING_SAVES = 2;

void require(bool valid)
{
    if (!valid)
        throw ProjectError("Invalid workspace operation.");
}

void cancel(const std::shared_ptr<std::atomic_bool>& flag)
{
    if (flag)
        flag->store(true);
}

WorkspaceOperation newOperation(QUuid source)
{
    return {.source = source, .id = QUuid::createUuid(), .cancelled = std::make_shared<std::atomic_bool>(false)};
}

} // namespace

WorkspaceSession::~WorkspaceSession()
{
    for (const auto& entry : m_Documents)
        for (const auto& flag : entry->jobs)
            cancel(flag);
    for (const auto& operation : m_Operations)
        cancel(operation.token.cancelled);
}

void WorkspaceSession::reserveAddition(std::size_t count)
{
    m_Sources.reserve(m_Sources.size() + 1);
    m_Documents.reserve(m_Documents.size() + count);
    m_Tabs.reserve(m_Tabs.size() + count);
}

void WorkspaceSession::selectAdded(QUuid id, WorkspaceActivation activation)
{
    if (!m_Active || activation == WorkspaceActivation::SelectNew)
        m_Active = id;
}

WorkspaceSession::OpenedSource
WorkspaceSession::addDocument(std::unique_ptr<DocumentSession>& incoming, const QString& mediaProjectPath, WorkspaceActivation activation)
{
    require(incoming && !incoming->sourcePath().isEmpty() && !mediaProjectPath.isEmpty());
    auto source = std::make_unique<Source>();
    source->id = QUuid::createUuid();
    source->mediaProjectPath = mediaProjectPath;
    auto entry = std::make_unique<Entry>();
    entry->id = QUuid::createUuid();
    entry->source = source.get();
    OpenedSource result{.source = source->id, .documents = {entry->id}};
    reserveAddition(1);
    source->standalone = std::move(incoming);
    m_Sources.push_back(std::move(source));
    m_Documents.push_back(std::move(entry));
    m_Tabs.push_back(result.documents.front());
    selectAdded(result.documents.front(), activation);
    return result;
}

WorkspaceSession::OpenedSource WorkspaceSession::addGroup(std::unique_ptr<ArchiveGroupSession>& incoming, WorkspaceActivation activation)
{
    require(incoming && !incoming->memberIds().empty());
    auto source = std::make_unique<Source>();
    source->id = QUuid::createUuid();
    OpenedSource result{.source = source->id, .documents = {}};
    std::vector<std::unique_ptr<Entry>> entries;
    for (const auto member : incoming->memberIds())
    {
        auto entry = std::make_unique<Entry>();
        entry->id = QUuid::createUuid();
        entry->member = member;
        entry->source = source.get();
        result.documents.push_back(entry->id);
        entries.push_back(std::move(entry));
    }
    reserveAddition(entries.size());
    source->group = std::move(incoming);
    m_Sources.push_back(std::move(source));
    for (auto& entry : entries)
    {
        m_Tabs.push_back(entry->id);
        m_Documents.push_back(std::move(entry));
    }
    selectAdded(result.documents.front(), activation);
    return result;
}

QUuid WorkspaceSession::appendMember(QUuid sourceId, std::unique_ptr<DocumentSession>& incoming, WorkspaceActivation activation)
{
    auto& source = findSource(sourceId);
    require(source.group != nullptr);
    auto entry = std::make_unique<Entry>();
    entry->id = QUuid::createUuid();
    entry->source = &source;
    reserveAddition(1);
    entry->member = source.group->append(incoming);
    const auto id = entry->id;
    m_Documents.push_back(std::move(entry));
    m_Tabs.push_back(id);
    selectAdded(id, activation);
    return id;
}

const WorkspaceSession::Entry& WorkspaceSession::findDocument(QUuid id) const
{
    const auto found = std::ranges::find_if(m_Documents, [id](const auto& entry) { return entry->id == id; });
    require(found != m_Documents.end());
    return **found;
}

WorkspaceSession::Entry& WorkspaceSession::findDocument(QUuid id)
{
    return const_cast<Entry&>(std::as_const(*this).findDocument(id));
}

const WorkspaceSession::Source& WorkspaceSession::findSource(QUuid id) const
{
    const auto found = std::ranges::find_if(m_Sources, [id](const auto& source) { return source->id == id; });
    require(found != m_Sources.end());
    return **found;
}

WorkspaceSession::Source& WorkspaceSession::findSource(QUuid id)
{
    return const_cast<Source&>(std::as_const(*this).findSource(id));
}

const WorkspaceDocument& WorkspaceSession::document(QUuid id) const
{
    const auto& entry = findDocument(id);
    return entry.source->group ? entry.source->group->document(entry.member) : entry.source->standalone->document;
}

LegacyProject& WorkspaceSession::project(QUuid id)
{
    auto& entry = findDocument(id);
    return entry.source->group ? entry.source->group->project(entry.member) : entry.source->standalone->document.project;
}

TrackEdits& WorkspaceSession::history(QUuid id)
{
    auto& entry = findDocument(id);
    return entry.source->group ? entry.source->group->history(entry.member) : entry.source->standalone->history;
}

QUuid WorkspaceSession::sourceId(QUuid id) const
{
    return findDocument(id).source->id;
}

const QString& WorkspaceSession::sourcePath(QUuid id) const
{
    const auto& source = findSource(id);
    return source.group ? source.group->sourcePath() : source.standalone->sourcePath();
}

QString WorkspaceSession::mediaProjectPath(QUuid id) const
{
    const auto& entry = findDocument(id);
    if (entry.source->group)
        return QDir(document(id).archivePayload->root()).absoluteFilePath("archive.trk");
    return entry.source->mediaProjectPath;
}

DocumentSession::SaveCheckpoint WorkspaceSession::documentCheckpoint(QUuid id) const
{
    const auto& entry = findDocument(id);
    if (!entry.source->group)
        return entry.source->standalone->saveCheckpoint();
    auto checkpoint = entry.source->group->findMember(entry.member).session->saveCheckpoint();
    const auto source = entry.source->group->m_SaveState.saveCheckpoint(0);
    checkpoint.recoveryTransactions = source.recoveryTransactions;
    checkpoint.nativeInvalidation = source.nativeInvalidation;
    checkpoint.trackerInvalidation = source.trackerInvalidation;
    return checkpoint;
}

WorkspaceJobToken WorkspaceSession::beginMovieIndex(QUuid id)
{
    auto& entry = findDocument(id);
    auto& document =
        entry.source->group ? entry.source->group->findMember(entry.member).session->document : entry.source->standalone->document;
    require(document.project.clip.mediaReferences.size() == 1);
    auto token = beginJob(id, WorkspaceJob::MediaIndex);
    document.movie = true;
    return token;
}

bool WorkspaceSession::applyMovieIndex(const WorkspaceJobToken& token, std::vector<double> frameTimes, int frameCount)
{
    if (token.kind != WorkspaceJob::MediaIndex || !accepts(token) || frameCount <= 0 || !std::cmp_equal(frameCount, frameTimes.size()) ||
        !std::ranges::all_of(frameTimes, [](double time) { return std::isfinite(time); }) || !std::ranges::is_sorted(frameTimes))
        return false;
    auto& entry = findDocument(token.document);
    auto& document =
        entry.source->group ? entry.source->group->findMember(entry.member).session->document : entry.source->standalone->document;
    document.movie = true;
    document.movieFrameTimes = std::move(frameTimes);
    auto& clip = document.project.clip;
    const auto steps = clip.stepCount.value_or(0);
    const auto last = static_cast<std::int64_t>(clip.firstFrame) + static_cast<std::int64_t>(steps - 1) * clip.stepSize;
    if (steps == 0 || last < frameCount)
        clip.sourceFrameCount = std::max(clip.sourceFrameCount.value_or(0), frameCount);
    return true;
}

bool WorkspaceSession::isModified(QUuid id) const
{
    const auto& source = findSource(id);
    return source.group ? source.group->isModified() : source.standalone->isModified();
}

bool WorkspaceSession::requiresPairedSave(QUuid id) const
{
    const auto& source = findSource(id);
    return source.group ? source.group->requiresPairedSave() : source.standalone->requiresPairedSave();
}

const QString& WorkspaceSession::nativeSavePath(QUuid id) const
{
    const auto& source = findSource(id);
    return source.group ? source.group->nativeSavePath() : source.standalone->nativeSavePath();
}

const QString& WorkspaceSession::standardSavePath(QUuid id) const
{
    const auto& source = findSource(id);
    return source.group ? source.group->standardSavePath() : source.standalone->standardSavePath();
}

const QString& WorkspaceSession::recoveryPath(QUuid id) const
{
    const auto& source = findSource(id);
    return source.group ? source.group->recoveryPath() : source.standalone->recoveryPath();
}

const std::vector<QUuid>& WorkspaceSession::tabs() const noexcept
{
    return m_Tabs;
}

std::optional<QUuid> WorkspaceSession::activeDocument() const noexcept
{
    return m_Active;
}

void WorkspaceSession::showDocument(QUuid id)
{
    (void)findDocument(id);
    if (std::ranges::find(m_Tabs, id) == m_Tabs.end())
        m_Tabs.push_back(id);
    m_Active = id;
}

void WorkspaceSession::hideDocument(QUuid id)
{
    (void)findDocument(id);
    const auto found = std::ranges::find(m_Tabs, id);
    if (found == m_Tabs.end())
        return;
    const auto position = static_cast<std::size_t>(found - m_Tabs.begin());
    m_Tabs.erase(found);
    if (m_Active == id)
        m_Active = m_Tabs.empty() ? std::nullopt : std::optional(m_Tabs[std::min(position, m_Tabs.size() - 1)]);
}

void WorkspaceSession::moveDocument(QUuid id, std::size_t position)
{
    const auto found = std::ranges::find(m_Tabs, id);
    require(found != m_Tabs.end() && position < m_Tabs.size());
    const auto from = static_cast<std::size_t>(found - m_Tabs.begin());
    if (from < position)
        std::rotate(found, found + 1, m_Tabs.begin() + static_cast<std::ptrdiff_t>(position + 1));
    else
        std::rotate(m_Tabs.begin() + static_cast<std::ptrdiff_t>(position), found, found + 1);
}

bool WorkspaceSession::releaseSource(QUuid id, WorkspaceDiscard discard)
{
    (void)findSource(id);
    if (std::ranges::any_of(m_Tabs, [&](QUuid tab) { return sourceId(tab) == id; }) ||
        std::ranges::any_of(m_Operations, [id](const auto& op) { return op.token.source == id; }) ||
        (discard != WorkspaceDiscard::Confirmed && isModified(id)))
        return false;
    cancelSourceWork(id);
    std::erase_if(m_Documents, [id](const auto& entry) { return entry->source->id == id; });
    std::erase_if(m_Sources, [id](const auto& source) { return source->id == id; });
    return true;
}

WorkspaceJobToken WorkspaceSession::beginJob(QUuid id, WorkspaceJob kind)
{
    auto& entry = findDocument(id);
    const auto index = static_cast<std::size_t>(kind);
    require(index < entry.jobs.size());
    auto flag = std::make_shared<std::atomic_bool>(false);
    cancel(entry.jobs[index]);
    entry.jobs[index] = flag;
    return {.document = id, .kind = kind, .cancelled = std::move(flag)};
}

bool WorkspaceSession::accepts(const WorkspaceJobToken& token) const
{
    if (!token.cancelled || token.cancelled->load())
        return false;
    const auto found = std::ranges::find_if(m_Documents, [&](const auto& entry) { return entry->id == token.document; });
    const auto index = static_cast<std::size_t>(token.kind);
    return found != m_Documents.end() && index < (*found)->jobs.size() && (*found)->jobs[index] == token.cancelled;
}

bool WorkspaceSession::finishJob(const WorkspaceJobToken& token)
{
    if (!accepts(token))
        return false;
    cancel(token.cancelled);
    return true;
}

void WorkspaceSession::cancelJobs(QUuid id)
{
    for (const auto& flag : findDocument(id).jobs)
        cancel(flag);
}

void WorkspaceSession::cancelSourceWork(QUuid id)
{
    (void)findSource(id);
    for (const auto& entry : m_Documents)
        if (entry->source->id == id)
            cancelJobs(entry->id);
    for (const auto& operation : m_Operations)
        if (operation.token.source == id)
            cancel(operation.token.cancelled);
}

std::optional<WorkspaceSaveRequest> WorkspaceSession::beginSave(QUuid id)
{
    const auto& source = *findDocument(id).source;
    if (m_Operations.size() >= MAX_PENDING_SAVES)
        return std::nullopt;
    WorkspaceSaveRequest result{.operation = {}, .snapshot = {}};
    if (source.group)
        result.snapshot = source.group->prepareSave();
    else
        result.snapshot = WorkspaceDocumentSnapshot{.document = source.standalone->document,
                                                    .mediaProjectPath = source.mediaProjectPath,
                                                    .checkpoint = source.standalone->saveCheckpoint()};
    result.operation = newOperation(source.id);
    m_Operations.push_back({.token = result.operation, .recovery = false});
    return result;
}

std::optional<WorkspaceRecoveryRequest> WorkspaceSession::beginRecovery(QUuid id)
{
    const auto& source = *findDocument(id).source;
    if (m_Operations.size() >= MAX_PENDING_SAVES || recoveryPath(source.id).isEmpty())
        return std::nullopt;
    WorkspaceRecoveryRequest result{.operation = {}, .checkpoint = {}, .path = recoveryPath(source.id), .transaction = {}};
    if (source.group)
    {
        auto checkpoint = source.group->saveCheckpoint();
        result.transaction = checkpoint.group.recoveryTransactions.value(result.path);
        result.checkpoint = std::move(checkpoint);
    }
    else
    {
        auto checkpoint = source.standalone->saveCheckpoint();
        result.transaction = checkpoint.recoveryTransactions.value(result.path);
        result.checkpoint = std::move(checkpoint);
    }
    result.operation = newOperation(source.id);
    m_Operations.push_back({.token = result.operation, .recovery = true});
    return result;
}

WorkspaceSession::Source* WorkspaceSession::operationSource(const WorkspaceOperation& token, bool recovery)
{
    const auto found = std::ranges::find_if(m_Operations,
                                            [&](const auto& op) {
                                                return op.token.source == token.source && op.token.id == token.id &&
                                                       op.token.cancelled == token.cancelled && op.recovery == recovery;
                                            });
    return found == m_Operations.end() ? nullptr : &findSource(token.source);
}

void WorkspaceSession::finishOperation(const WorkspaceOperation& token)
{
    cancel(token.cancelled);
    std::erase_if(m_Operations, [&](const auto& op) { return op.token.id == token.id; });
}

SaveOutcome WorkspaceSession::completeSave(const WorkspaceSaveRequest& request, const QString& destination, const NativeSaveResult& result)
{
    auto* source = operationSource(request.operation, false);
    if (!source)
        return SaveOutcome::Replaced;
    const auto outcome =
        source->group
            ? source->group->completeSave(std::get<ArchiveGroupSession::SaveRequest>(request.snapshot).checkpoint, destination, result)
            : source->standalone->completeSave(std::get<WorkspaceDocumentSnapshot>(request.snapshot).checkpoint, destination, result);
    finishOperation(request.operation);
    return outcome;
}

SaveOutcome WorkspaceSession::completePairedSave(const WorkspaceSaveRequest& request, const PairedSaveResult& result)
{
    auto* source = operationSource(request.operation, false);
    if (!source)
        return SaveOutcome::Replaced;
    const auto outcome =
        source->group ? source->group->completePairedSave(std::get<ArchiveGroupSession::SaveRequest>(request.snapshot).checkpoint, result)
                      : source->standalone->completePairedSave(std::get<WorkspaceDocumentSnapshot>(request.snapshot).checkpoint, result);
    finishOperation(request.operation);
    return outcome;
}

SaveOutcome WorkspaceSession::completeRecovery(const WorkspaceRecoveryRequest& request, const PairedSaveResult& result)
{
    auto* source = operationSource(request.operation, true);
    if (!source)
        return SaveOutcome::Replaced;
    const auto outcome =
        source->group
            ? source->group->completePairedRecovery(std::get<ArchiveGroupSession::SaveCheckpoint>(request.checkpoint), request.path, result)
            : source->standalone->completePairedRecovery(
                  std::get<DocumentSession::SaveCheckpoint>(request.checkpoint), request.path, result);
    finishOperation(request.operation);
    return outcome;
}

NativeSaveResult writeWorkspaceNative(const WorkspaceSaveRequest& request, const QString& path, const PairedSaveObserver& observer)
{
    if (const auto* group = std::get_if<ArchiveGroupSession::SaveRequest>(&request.snapshot))
        return writeNativeArchiveGroup(path, group->snapshot, request.operation.cancelled.get(), observer);
    const auto& snapshot = std::get<WorkspaceDocumentSnapshot>(request.snapshot);
    auto document = snapshot.document;
    refreshWorkspaceMediaLocations(document, snapshot.mediaProjectPath);
    rebaseWorkspaceMedia(document, snapshot.mediaProjectPath, path);
    return writeNativeDocument(path, document, request.operation.cancelled.get(), observer);
}

PairedSaveResult writeWorkspacePair(const WorkspaceSaveRequest& request, const QString& path, const PairedSaveObserver& observer)
{
    if (const auto* group = std::get_if<ArchiveGroupSession::SaveRequest>(&request.snapshot))
        return writePairedArchiveGroup(path, group->snapshot, request.operation.cancelled.get(), observer);
    const auto& snapshot = std::get<WorkspaceDocumentSnapshot>(request.snapshot);
    return writePairedDocuments(path, snapshot.document, snapshot.mediaProjectPath, request.operation.cancelled.get(), observer);
}

PairedSaveResult recoverWorkspacePair(const WorkspaceRecoveryRequest& request, const PairedSaveObserver& observer)
{
    return recoverPairedDocuments(request.path, request.operation.cancelled.get(), observer, request.transaction);
}

} // namespace motrack
