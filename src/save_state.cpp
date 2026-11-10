#include "save_state.hpp"

#include <QFileInfo>

#include <algorithm>
#include <utility>

namespace motrack
{

void SaveState::reset(const QString& source, DocumentSourceKind kind)
{
    auto identity = std::make_shared<const Identity>();
    const auto path = QFileInfo(source).absoluteFilePath();
    const auto native = kind == DocumentSourceKind::Project && source.endsWith(".motrack", Qt::CaseInsensitive) ? path : QString{};
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
    m_RecoveryLocationIdentities.clear();
    m_NativeInvalidation = 0;
    m_TrackerInvalidation = 0;
    m_PairedSaving = false;
    m_MediaObservation = 0;
    m_NativeDestinationObservation = 0;
    m_StandardDestinationObservation = 0;
}

const QString& SaveState::sourcePath() const
{
    return m_SourcePath;
}

const QString& SaveState::nativeSavePath() const
{
    return m_NativeSavePath;
}

const QString& SaveState::standardSavePath() const
{
    return m_StandardSavePath;
}

const QString& SaveState::recoveryPath() const
{
    static const QString empty;
    return m_RecoveryPaths.isEmpty() ? empty : m_RecoveryPaths.front();
}

bool SaveState::requiresPairedSave() const
{
    return m_PairedSaving;
}

SaveState::Checkpoint SaveState::saveCheckpoint(std::uint64_t revision) const
{
    QHash<QString, QString> firstTransactions;
    for (auto entry = m_RecoveryTransactions.cbegin(); entry != m_RecoveryTransactions.cend(); ++entry)
        if (!entry.value().isEmpty())
            firstTransactions.insert(entry.key(), entry.value().front().transaction);
    return {.identity = m_Identity,
            .revision = revision,
            .recoveryTransactions = std::move(firstTransactions),
            .nativeInvalidation = m_NativeInvalidation,
            .trackerInvalidation = m_TrackerInvalidation};
}

void SaveState::invalidateSaved(SaveTarget target, SaveUpdate& update)
{
    ++(target == SaveTarget::Native ? m_NativeInvalidation : m_TrackerInvalidation);
    (target == SaveTarget::Native ? update.native : update.tracker) = SaveChange::Invalidated;
}

void SaveState::rememberRecovery(const QString& path, const QString& transaction, std::uint64_t observation)
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

void SaveState::forgetRecovery(const QString& path, const QString& transaction, std::uint64_t observation, bool cleared)
{
    if (cleared)
        m_ClearedRecoveryObservations[path] = std::max(m_ClearedRecoveryObservations.value(path), observation);
    if (!transaction.isEmpty())
        m_CompletedRecoveryTransactions[path][transaction] =
            std::max(m_CompletedRecoveryTransactions.value(path).value(transaction), observation);
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
                m_CompletedRecoveryTransactions[path][pending.transaction] =
                    std::max(m_CompletedRecoveryTransactions.value(path).value(pending.transaction), observation);
            return obsolete;
        });
    if (entry.value().isEmpty())
    {
        m_RecoveryTransactions.erase(entry);
        m_RecoveryPaths.removeAll(path);
    }
}

SaveUpdate
SaveState::completeSave(const Checkpoint& checkpoint, std::uint64_t revision, const QString& destination, const NativeSaveResult& result)
{
    SaveUpdate update;
    const auto finish = [&update](SaveOutcome outcome)
    {
        update.outcome = outcome;
        return update;
    };
    if (checkpoint.identity != m_Identity)
    {
        return finish(SaveOutcome::Replaced);
    }
    if (!result.saved || result.observation == 0)
    {
        return finish(SaveOutcome::Failed);
    }
    if (result.observation < m_NativeDestinationObservation)
        return finish(SaveOutcome::Superseded);
    m_NativeSavePath = QFileInfo(destination).absoluteFilePath();
    m_NativeDestinationObservation = result.observation;
    if (result.observation >= m_MediaObservation)
    {
        update.adoptMediaLocations = true;
        m_MediaObservation = result.observation;
    }
    if (checkpoint.revision != revision || checkpoint.nativeInvalidation != m_NativeInvalidation)
    {
        invalidateSaved(SaveTarget::Native, update);
        return finish(SaveOutcome::Stale);
    }
    update.native = SaveChange::Saved;
    return finish(SaveOutcome::Saved);
}

SaveUpdate SaveState::completePairedSave(const Checkpoint& checkpoint, std::uint64_t revision, const PairedSaveResult& result)
{
    SaveUpdate update;
    const auto finish = [&update](SaveOutcome outcome)
    {
        update.outcome = outcome;
        return update;
    };
    if (checkpoint.identity != m_Identity)
        return finish(SaveOutcome::Replaced);
    if (result.recoveryIdentity)
        m_RecoveryLocationIdentities.insert(*result.recoveryIdentity, result.recoveryIdentity);
    const auto recovery = result.recoveryIdentity ? *result.recoveryIdentity : result.recoveryPath;
    const auto observed = result.recoveryIdentity ? *result.recoveryIdentity : result.observedRecoveryPath;
    const auto completed = m_CompletedRecoveryTransactions.value(observed.isEmpty() ? recovery : observed);
    if ((!result.transactionId.isEmpty() && completed.contains(result.transactionId) &&
         result.observation <= completed.value(result.transactionId)) ||
        (!result.recoveryPath.isEmpty() && result.observation != 0 && result.observation <= m_ClearedRecoveryObservations.value(recovery)))
        return finish(SaveOutcome::Stale);
    if (!result.standardCurrent && !result.nativeCurrent && result.status != PairedSaveStatus::RecoveryRequired)
        return finish(SaveOutcome::Failed);
    if ((result.standardCurrent || result.nativeCurrent) && result.observation >= m_MediaObservation)
    {
        update.adoptMediaLocations = true;
        m_MediaObservation = result.observation;
    }
    // A blocked retry only reports an existing journal; it must not restore an older destination.
    const bool olderStandard = result.observation < m_StandardDestinationObservation;
    const bool olderNative = result.observation < m_NativeDestinationObservation;
    const bool adoptStandard = !olderStandard && (!m_PairedSaving || m_StandardSavePath.isEmpty() || result.standardCurrent);
    m_PairedSaving = true;
    const bool changedRevision = checkpoint.revision != revision;
    const bool nativeStale = changedRevision || checkpoint.nativeInvalidation != m_NativeInvalidation;
    const bool trackerStale = changedRevision || checkpoint.trackerInvalidation != m_TrackerInvalidation;
    const auto standard = QFileInfo(result.standardPath).absoluteFilePath();
    if (adoptStandard && !result.standardPath.isEmpty())
    {
        m_StandardDestinationObservation = result.observation;
        if (standard != m_StandardSavePath)
        {
            m_StandardSavePath = standard;
            if (!result.standardCurrent)
                invalidateSaved(SaveTarget::Tracker, update);
        }
    }
    if (result.standardCurrent && !olderStandard)
    {
        if (trackerStale)
            invalidateSaved(SaveTarget::Tracker, update);
        else
            update.tracker = SaveChange::Saved;
    }
    if (result.nativeCurrent && !olderNative)
    {
        m_NativeSavePath = QFileInfo(result.nativePath).absoluteFilePath();
        m_NativeDestinationObservation = result.observation;
        if (nativeStale)
            invalidateSaved(SaveTarget::Native, update);
        else
            update.native = SaveChange::Saved;
    }
    if (result.status == PairedSaveStatus::Saved && result.standardCurrent && result.nativeCurrent &&
        !result.observedRecoveryPath.isEmpty() && result.observation != 0)
        forgetRecovery(observed, result.transactionId, result.observation, true);
    if (!result.recoveryPath.isEmpty())
        rememberRecovery(recovery, result.transactionId, result.observation);
    if (result.status == PairedSaveStatus::RecoveryRequired || !m_RecoveryPaths.isEmpty())
        return finish(SaveOutcome::Partial);
    if (result.status != PairedSaveStatus::Saved || !result.standardCurrent || !result.nativeCurrent)
        return finish(SaveOutcome::Failed);
    return finish(nativeStale || trackerStale || olderStandard || olderNative ? SaveOutcome::Stale : SaveOutcome::Saved);
}

SaveUpdate
SaveState::completePairedRecovery(const Checkpoint& checkpoint, const QString& requestedRecoveryPath, const PairedSaveResult& result)
{
    SaveUpdate update;
    const auto finish = [&update](SaveOutcome outcome)
    {
        update.outcome = outcome;
        return update;
    };
    if (checkpoint.identity != m_Identity)
        return finish(SaveOutcome::Replaced);
    if (!checkpoint.recoveryTransactions.contains(requestedRecoveryPath))
        return finish(m_RecoveryPaths.contains(requestedRecoveryPath) ? SaveOutcome::Stale : SaveOutcome::Replaced);
    // UUIDs survive directory copies. Bind the callback to its observed slot before changing
    // any pending records or watermarks; a successful recovery may have removed the file already.
    const auto& observed = result.recoveryIdentity                  ? *result.recoveryIdentity
                           : !result.observedRecoveryPath.isEmpty() ? result.observedRecoveryPath
                                                                    : result.recoveryPath;
    if (observed.isEmpty() || observed != requestedRecoveryPath)
        return finish(SaveOutcome::Stale);
    if (result.recoveryIdentity)
        m_RecoveryLocationIdentities.insert(*result.recoveryIdentity, result.recoveryIdentity);
    const auto transaction = checkpoint.recoveryTransactions.value(requestedRecoveryPath);
    // The accessor may have supplied a list-element reference that completion is about to remove.
    const auto completedPath = requestedRecoveryPath; // NOLINT(performance-unnecessary-copy-initialization)
    if (result.status == PairedSaveStatus::RecoveryRequired && result.observation != 0)
    {
        if (result.observation <= m_ClearedRecoveryObservations.value(completedPath))
            return finish(SaveOutcome::Stale);
        // An older success may already have removed the pending entry. A newer failed observation
        // must restore it even when the journal's UUID is unchanged or could not be read.
        if (!result.transactionId.isEmpty() && result.transactionId != transaction)
            forgetRecovery(completedPath, transaction, result.observation, false);
        rememberRecovery(completedPath, result.transactionId, result.observation);
        invalidateSaved(SaveTarget::Native, update);
        invalidateSaved(SaveTarget::Tracker, update);
        return finish(SaveOutcome::Partial);
    }
    if (!m_RecoveryPaths.contains(completedPath))
        return finish(SaveOutcome::Replaced);
    const auto& transactions = m_RecoveryTransactions[completedPath];
    const auto pending = std::ranges::find(transactions, transaction, &RecoveryObservation::transaction);
    const bool absent = result.status == PairedSaveStatus::RecoveryNotNeeded;
    // Lock-confirmed absence is about the whole journal slot, even when an unreadable
    // checkpoint has since been superseded by a known transaction at that location.
    if (pending == transactions.end() && !(absent && result.observation != 0))
        return finish(SaveOutcome::Stale);
    const bool newerObservation =
        result.observation != 0 &&
        (absent ? std::ranges::any_of(transactions, [&](const auto& entry) { return entry.observation > result.observation; })
                : pending->observation > result.observation);
    if (!absent && (result.status != PairedSaveStatus::Saved || !result.standardCurrent || !result.nativeCurrent))
        return finish(SaveOutcome::Partial);
    if (!absent && !transaction.isEmpty() && result.transactionId != transaction)
        return finish(SaveOutcome::Stale);
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
        m_CompletedRecoveryTransactions[completedPath][result.transactionId] =
            std::max(m_CompletedRecoveryTransactions.value(completedPath).value(result.transactionId), result.observation);
    // Recovery never establishes that its snapshot equals the live document. Previously dispatched
    // writes cannot clear this invalidation merely because their edit revision happens to match.
    invalidateSaved(SaveTarget::Native, update);
    invalidateSaved(SaveTarget::Tracker, update);
    return finish(newerObservation ? SaveOutcome::Stale : SaveOutcome::Recovered);
}

} // namespace motrack
