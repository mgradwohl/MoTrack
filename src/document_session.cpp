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
    return {.identity = m_Identity, .revision = history.revision(), .recoveryTransactions = m_RecoveryTransactions};
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
    {
        if (!m_RecoveryPaths.contains(result.recoveryPath))
            m_RecoveryPaths.append(result.recoveryPath);
        if (!result.transactionId.isEmpty() || !m_RecoveryTransactions.contains(result.recoveryPath))
            m_RecoveryTransactions.insert(result.recoveryPath, result.transactionId);
    }
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
        checkpoint.recoveryTransactions.value(requestedRecoveryPath) != m_RecoveryTransactions.value(requestedRecoveryPath))
        return SaveOutcome::Stale;
    if (result.status != PairedSaveStatus::Saved || !result.standardCurrent || !result.nativeCurrent)
        return SaveOutcome::Partial;
    const auto transaction = checkpoint.recoveryTransactions.value(requestedRecoveryPath);
    if (!transaction.isEmpty() && result.transactionId != transaction)
        return SaveOutcome::Stale;
    const auto recoveredStandard = QFileInfo(result.standardPath).absoluteFilePath();
    if (m_StandardSavePath.isEmpty())
        m_StandardSavePath = recoveredStandard;
    if (m_NativeSavePath.isEmpty() && m_StandardSavePath == recoveredStandard)
        m_NativeSavePath = QFileInfo(result.nativePath).absoluteFilePath();
    m_RecoveryPaths.removeAll(requestedRecoveryPath);
    m_RecoveryTransactions.remove(requestedRecoveryPath);
    // A journal can predate the live session or be recovered after further edits. A fresh paired
    // save establishes its relationship to the current document; recovery alone never does.
    history.invalidateSaved();
    history.invalidateSaved(SaveTarget::Tracker);
    return SaveOutcome::Recovered;
}

} // namespace motrack
