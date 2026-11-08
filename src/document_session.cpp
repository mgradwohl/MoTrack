#include "document_session.hpp"

#include <QFileInfo>

#include <algorithm>
#include <utility>

namespace motrack
{

void DocumentSession::replace(WorkspaceDocument incoming, const QString& source, DocumentSourceKind kind)
{
    auto identity = std::make_shared<const Identity>();
    const auto path = QFileInfo(source).absoluteFilePath();
    const auto native = kind == DocumentSourceKind::Project && source.endsWith(".motrack", Qt::CaseInsensitive) ? path : QString{};
    document = std::move(incoming);
    m_Identity = std::move(identity);
    m_SourcePath = path;
    m_NativeSavePath = native;
    m_StandardSavePath = kind == DocumentSourceKind::Project &&
                                 (source.endsWith(".trk", Qt::CaseInsensitive) || source.endsWith(".trz", Qt::CaseInsensitive))
                             ? path
                             : QString{};
    m_RecoveryPaths.clear();
    m_RecoveryTransactions.clear();
    m_CompletedRecoveryTransactions.clear();
    m_ClearedRecoveryObservations.clear();
    m_NativeInvalidation = 0;
    m_TrackerInvalidation = 0;
    m_PairedSaving = false;
    history.clear();
}

const QString& DocumentSession::sourcePath() const
{
    return m_SourcePath;
}

const QString& DocumentSession::nativeSavePath() const
{
    return m_NativeSavePath;
}

const QString& DocumentSession::standardSavePath() const
{
    return m_StandardSavePath;
}

const QString& DocumentSession::recoveryPath() const
{
    static const QString empty;
    return m_RecoveryPaths.isEmpty() ? empty : m_RecoveryPaths.front();
}

bool DocumentSession::requiresPairedSave() const
{
    return m_PairedSaving;
}

bool DocumentSession::isModified() const
{
    return history.isModified() || (m_PairedSaving && (history.isModified(SaveTarget::Tracker) || !m_RecoveryPaths.isEmpty()));
}

DocumentSession::SaveCheckpoint DocumentSession::saveCheckpoint() const
{
    QHash<QString, QString> firstTransactions;
    for (auto entry = m_RecoveryTransactions.cbegin(); entry != m_RecoveryTransactions.cend(); ++entry)
        if (!entry.value().isEmpty())
            firstTransactions.insert(entry.key(), entry.value().front().transaction);
    return {.identity = m_Identity,
            .revision = history.revision(),
            .recoveryTransactions = std::move(firstTransactions),
            .nativeInvalidation = m_NativeInvalidation,
            .trackerInvalidation = m_TrackerInvalidation};
}

void DocumentSession::invalidateSaved(SaveTarget target)
{
    ++(target == SaveTarget::Native ? m_NativeInvalidation : m_TrackerInvalidation);
    history.invalidateSaved(target);
}

void DocumentSession::rememberRecovery(const QString& path, const QString& transaction, std::uint64_t observation)
{
    auto& transactions = m_RecoveryTransactions[path];
    if (transaction.isEmpty() &&
        std::ranges::any_of(transactions,
                            [&](const auto& pending)
                            { return !pending.transaction.isEmpty() && (observation == 0 || pending.observation >= observation); }))
        return; // Later known identities supersede earlier unreadable observations.
    if (!transaction.isEmpty())
        transactions.removeIf([observation](const auto& pending)
                              { return pending.transaction.isEmpty() && pending.observation <= observation; });
    const auto existing = std::ranges::find(transactions, transaction, &RecoveryObservation::transaction);
    if (existing == transactions.end())
        transactions.append({.transaction = transaction, .observation = observation});
    else
        existing->observation = std::max(existing->observation, observation);
    if (!m_RecoveryPaths.contains(path))
        m_RecoveryPaths.append(path);
}

void DocumentSession::forgetRecovery(const QString& path, const QString& transaction, std::uint64_t observation, bool cleared)
{
    if (cleared)
        m_ClearedRecoveryObservations[path] = std::max(m_ClearedRecoveryObservations.value(path), observation);
    if (!transaction.isEmpty())
        m_CompletedRecoveryTransactions[transaction] = std::max(m_CompletedRecoveryTransactions.value(transaction), observation);
    auto entry = m_RecoveryTransactions.find(path);
    if (entry == m_RecoveryTransactions.end())
        return;
    entry.value().removeIf(
        [&](const auto& pending)
        {
            const bool obsolete = cleared && observation != 0
                                      ? pending.observation <= observation
                                      : pending.transaction == transaction && (observation == 0 || pending.observation <= observation);
            if (obsolete && !pending.transaction.isEmpty())
                m_CompletedRecoveryTransactions[pending.transaction] =
                    std::max(m_CompletedRecoveryTransactions.value(pending.transaction), observation);
            return obsolete;
        });
    if (entry.value().isEmpty())
    {
        m_RecoveryTransactions.erase(entry);
        m_RecoveryPaths.removeAll(path);
    }
}

SaveOutcome DocumentSession::completeSave(const SaveCheckpoint& checkpoint, const QString& destination, bool success)
{
    if (checkpoint.identity != m_Identity)
    {
        return SaveOutcome::Replaced;
    }
    if (!success)
    {
        return SaveOutcome::Failed;
    }
    m_NativeSavePath = QFileInfo(destination).absoluteFilePath();
    if (checkpoint.revision != history.revision() || checkpoint.nativeInvalidation != m_NativeInvalidation)
    {
        invalidateSaved();
        return SaveOutcome::Stale;
    }
    history.markSaved();
    return SaveOutcome::Saved;
}

SaveOutcome DocumentSession::completePairedSave(const SaveCheckpoint& checkpoint, const PairedSaveResult& result)
{
    if (checkpoint.identity != m_Identity)
        return SaveOutcome::Replaced;
    if ((!result.transactionId.isEmpty() && m_CompletedRecoveryTransactions.contains(result.transactionId) &&
         result.observation <= m_CompletedRecoveryTransactions.value(result.transactionId)) ||
        (!result.recoveryPath.isEmpty() && result.observation != 0 &&
         result.observation <= m_ClearedRecoveryObservations.value(result.recoveryPath)))
        return SaveOutcome::Stale;
    if (!result.standardCurrent && !result.nativeCurrent && result.status != PairedSaveStatus::RecoveryRequired)
        return SaveOutcome::Failed;
    if (result.standardCurrent || result.nativeCurrent)
        document.mediaLocations = result.mediaLocations;
    // A blocked retry only reports an existing journal; it must not restore an older destination.
    const bool adoptStandard = !m_PairedSaving || m_StandardSavePath.isEmpty() || result.standardCurrent;
    m_PairedSaving = true;
    const bool changedRevision = checkpoint.revision != history.revision();
    const bool nativeStale = changedRevision || checkpoint.nativeInvalidation != m_NativeInvalidation;
    const bool trackerStale = changedRevision || checkpoint.trackerInvalidation != m_TrackerInvalidation;
    const auto standard = QFileInfo(result.standardPath).absoluteFilePath();
    if (adoptStandard && !result.standardPath.isEmpty() && standard != m_StandardSavePath)
    {
        m_StandardSavePath = standard;
        invalidateSaved(SaveTarget::Tracker);
    }
    if (result.standardCurrent)
    {
        if (trackerStale)
            invalidateSaved(SaveTarget::Tracker);
        else
            history.markSaved(SaveTarget::Tracker);
    }
    if (result.nativeCurrent)
    {
        m_NativeSavePath = QFileInfo(result.nativePath).absoluteFilePath();
        if (nativeStale)
            invalidateSaved();
        else
            history.markSaved();
    }
    if (!result.recoveryPath.isEmpty())
        rememberRecovery(result.recoveryPath, result.transactionId, result.observation);
    if (result.status == PairedSaveStatus::RecoveryRequired || !m_RecoveryPaths.isEmpty())
        return SaveOutcome::Partial;
    if (result.status != PairedSaveStatus::Saved || !result.standardCurrent || !result.nativeCurrent)
        return SaveOutcome::Failed;
    return nativeStale || trackerStale ? SaveOutcome::Stale : SaveOutcome::Saved;
}

SaveOutcome DocumentSession::completePairedRecovery(const SaveCheckpoint& checkpoint,
                                                    const QString& requestedRecoveryPath,
                                                    const PairedSaveResult& result)
{
    if (checkpoint.identity != m_Identity)
        return SaveOutcome::Replaced;
    if (!checkpoint.recoveryTransactions.contains(requestedRecoveryPath))
        return m_RecoveryPaths.contains(requestedRecoveryPath) ? SaveOutcome::Stale : SaveOutcome::Replaced;
    const auto transaction = checkpoint.recoveryTransactions.value(requestedRecoveryPath);
    // The accessor may have supplied a list-element reference that completion is about to remove.
    const auto completedPath = requestedRecoveryPath; // NOLINT(performance-unnecessary-copy-initialization)
    if (result.status == PairedSaveStatus::RecoveryRequired && result.observation != 0)
    {
        if (result.observation <= m_ClearedRecoveryObservations.value(completedPath))
            return SaveOutcome::Stale;
        // An older success may already have removed the pending entry. A newer failed observation
        // must restore it even when the journal's UUID is unchanged or could not be read.
        if (!result.transactionId.isEmpty() && result.transactionId != transaction)
            forgetRecovery(completedPath, transaction, result.observation, false);
        rememberRecovery(completedPath, result.transactionId, result.observation);
        invalidateSaved();
        invalidateSaved(SaveTarget::Tracker);
        return SaveOutcome::Partial;
    }
    if (!m_RecoveryPaths.contains(completedPath))
        return SaveOutcome::Replaced;
    const auto& transactions = m_RecoveryTransactions[completedPath];
    const auto pending = std::ranges::find(transactions, transaction, &RecoveryObservation::transaction);
    if (pending == transactions.end())
        return SaveOutcome::Stale;
    const bool newerObservation = result.observation != 0 && pending->observation > result.observation;
    const bool absent = result.status == PairedSaveStatus::RecoveryNotNeeded;
    if (!absent && (result.status != PairedSaveStatus::Saved || !result.standardCurrent || !result.nativeCurrent))
        return SaveOutcome::Partial;
    if (!transaction.isEmpty() && result.transactionId != transaction)
        return SaveOutcome::Stale;
    if (!absent && !newerObservation)
    {
        const auto recoveredStandard = QFileInfo(result.standardPath).absoluteFilePath();
        if (m_StandardSavePath.isEmpty())
            m_StandardSavePath = recoveredStandard;
        if (m_NativeSavePath.isEmpty() && m_StandardSavePath == recoveredStandard)
            m_NativeSavePath = QFileInfo(result.nativePath).absoluteFilePath();
    }
    forgetRecovery(completedPath, transaction, result.observation, true);
    if (!result.transactionId.isEmpty())
        m_CompletedRecoveryTransactions[result.transactionId] =
            std::max(m_CompletedRecoveryTransactions.value(result.transactionId), result.observation);
    // Recovery never establishes that its snapshot equals the live document. Previously dispatched
    // writes cannot clear this invalidation merely because their edit revision happens to match.
    invalidateSaved();
    invalidateSaved(SaveTarget::Tracker);
    return newerObservation ? SaveOutcome::Stale : SaveOutcome::Recovered;
}

} // namespace motrack
