#include "document_session.hpp"

#include <QFileInfo>

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
            firstTransactions.insert(entry.key(), entry.value().front());
    return {.identity = m_Identity, .revision = history.revision(), .recoveryTransactions = std::move(firstTransactions)};
}

void DocumentSession::rememberRecovery(const QString& path, const QString& transaction)
{
    if (!transaction.isEmpty() && m_CompletedRecoveryTransactions.contains(transaction))
        return;
    auto& transactions = m_RecoveryTransactions[path];
    if (transaction.isEmpty() && !transactions.isEmpty())
        return; // An unreadable retry cannot replace a known pending identity.
    if (!transaction.isEmpty())
        transactions.removeAll(QString{});
    if (!transactions.contains(transaction))
        transactions.append(transaction);
    if (!m_RecoveryPaths.contains(path))
        m_RecoveryPaths.append(path);
}

void DocumentSession::forgetRecovery(const QString& path, const QString& transaction)
{
    if (!transaction.isEmpty())
        m_CompletedRecoveryTransactions.insert(transaction);
    auto entry = m_RecoveryTransactions.find(path);
    if (entry == m_RecoveryTransactions.end())
        return;
    entry.value().removeAll(transaction);
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
    if (checkpoint.revision != history.revision())
    {
        history.invalidateSaved();
        return SaveOutcome::Stale;
    }
    history.markSaved();
    return SaveOutcome::Saved;
}

SaveOutcome DocumentSession::completePairedSave(const SaveCheckpoint& checkpoint, const PairedSaveResult& result)
{
    if (checkpoint.identity != m_Identity)
        return SaveOutcome::Replaced;
    if (!result.transactionId.isEmpty() && m_CompletedRecoveryTransactions.contains(result.transactionId))
        return SaveOutcome::Stale;
    if (!result.standardCurrent && !result.nativeCurrent && result.status != PairedSaveStatus::RecoveryRequired)
        return SaveOutcome::Failed;
    m_PairedSaving = true;
    const bool stale = checkpoint.revision != history.revision();
    const auto standard = QFileInfo(result.standardPath).absoluteFilePath();
    if (!result.standardPath.isEmpty() && standard != m_StandardSavePath)
    {
        m_StandardSavePath = standard;
        history.invalidateSaved(SaveTarget::Tracker);
    }
    if (result.standardCurrent)
    {
        if (stale)
            history.invalidateSaved(SaveTarget::Tracker);
        else
            history.markSaved(SaveTarget::Tracker);
    }
    if (result.nativeCurrent)
    {
        m_NativeSavePath = QFileInfo(result.nativePath).absoluteFilePath();
        if (stale)
            history.invalidateSaved();
        else
            history.markSaved();
    }
    if (!result.recoveryPath.isEmpty())
        rememberRecovery(result.recoveryPath, result.transactionId);
    if (result.status == PairedSaveStatus::RecoveryRequired || !m_RecoveryPaths.isEmpty())
        return SaveOutcome::Partial;
    if (result.status != PairedSaveStatus::Saved || !result.standardCurrent || !result.nativeCurrent)
        return SaveOutcome::Failed;
    return stale ? SaveOutcome::Stale : SaveOutcome::Saved;
}

SaveOutcome DocumentSession::completePairedRecovery(const SaveCheckpoint& checkpoint,
                                                    const QString& requestedRecoveryPath,
                                                    const PairedSaveResult& result)
{
    if (checkpoint.identity != m_Identity || !m_RecoveryPaths.contains(requestedRecoveryPath))
        return SaveOutcome::Replaced;
    if (!checkpoint.recoveryTransactions.contains(requestedRecoveryPath) ||
        !m_RecoveryTransactions.value(requestedRecoveryPath).contains(checkpoint.recoveryTransactions.value(requestedRecoveryPath)))
        return SaveOutcome::Stale;
    const auto transaction = checkpoint.recoveryTransactions.value(requestedRecoveryPath);
    // The public accessor may have supplied a reference to the list element that is about to be removed.
    const auto completedPath = requestedRecoveryPath; // NOLINT(performance-unnecessary-copy-initialization)
    if (result.status == PairedSaveStatus::RecoveryRequired && !result.transactionId.isEmpty() && result.transactionId != transaction)
    {
        // Another process replaced the journal. No output changed; the next confirmation must identify this transaction.
        forgetRecovery(completedPath, transaction);
        rememberRecovery(completedPath, result.transactionId);
        history.invalidateSaved();
        history.invalidateSaved(SaveTarget::Tracker);
        return SaveOutcome::Partial;
    }
    const bool absent = result.status == PairedSaveStatus::RecoveryNotNeeded && !transaction.isEmpty();
    if (!absent && (result.status != PairedSaveStatus::Saved || !result.standardCurrent || !result.nativeCurrent))
        return SaveOutcome::Partial;
    if (!transaction.isEmpty() && result.transactionId != transaction)
        return SaveOutcome::Stale;
    if (!absent)
    {
        const auto recoveredStandard = QFileInfo(result.standardPath).absoluteFilePath();
        if (m_StandardSavePath.isEmpty())
            m_StandardSavePath = recoveredStandard;
        if (m_NativeSavePath.isEmpty() && m_StandardSavePath == recoveredStandard)
            m_NativeSavePath = QFileInfo(result.nativePath).absoluteFilePath();
    }
    forgetRecovery(completedPath, transaction);
    if (!result.transactionId.isEmpty())
        m_CompletedRecoveryTransactions.insert(result.transactionId);
    // A journal can predate the live session or be recovered after further edits. A fresh paired
    // save establishes its relationship to the current document; recovery alone never does.
    history.invalidateSaved();
    history.invalidateSaved(SaveTarget::Tracker);
    return SaveOutcome::Recovered;
}

} // namespace motrack
