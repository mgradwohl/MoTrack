#include "document_session.hpp"

#include <QFileInfo>

#include <utility>

namespace motrack
{

void DocumentSession::replace(WorkspaceDocument incoming, const QString& source)
{
    auto identity = std::make_shared<const Identity>();
    const auto path = QFileInfo(source).absoluteFilePath();
    const auto native = source.endsWith(".motrack", Qt::CaseInsensitive) ? path : QString{};
    document = std::move(incoming);
    m_Identity = std::move(identity);
    m_SourcePath = path;
    m_NativeSavePath = native;
    m_StandardSavePath = source.endsWith(".trk", Qt::CaseInsensitive) || source.endsWith(".trz", Qt::CaseInsensitive) ? path : QString{};
    m_RecoveryPath.clear();
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
    return m_RecoveryPath;
}

bool DocumentSession::requiresPairedSave() const
{
    return m_PairedSaving;
}

bool DocumentSession::isModified() const
{
    return history.isModified() || (m_PairedSaving && (history.isModified(SaveTarget::Tracker) || !m_RecoveryPath.isEmpty()));
}

DocumentSession::SaveCheckpoint DocumentSession::saveCheckpoint() const
{
    return {.identity = m_Identity, .revision = history.revision()};
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
    m_RecoveryPath = result.recoveryPath;
    if (result.status == PairedSaveStatus::RecoveryRequired)
        return SaveOutcome::Partial;
    if (result.status != PairedSaveStatus::Saved || !result.standardCurrent || !result.nativeCurrent)
        return SaveOutcome::Failed;
    return stale ? SaveOutcome::Stale : SaveOutcome::Saved;
}

SaveOutcome DocumentSession::completePairedRecovery(const SaveCheckpoint& checkpoint,
                                                    const QString& requestedRecoveryPath,
                                                    const PairedSaveResult& result)
{
    if (checkpoint.identity != m_Identity || m_RecoveryPath.isEmpty() || requestedRecoveryPath != m_RecoveryPath)
        return SaveOutcome::Replaced;
    if (result.status != PairedSaveStatus::Saved || !result.standardCurrent || !result.nativeCurrent)
        return SaveOutcome::Partial;
    m_StandardSavePath = QFileInfo(result.standardPath).absoluteFilePath();
    m_NativeSavePath = QFileInfo(result.nativePath).absoluteFilePath();
    m_RecoveryPath.clear();
    // A journal can predate the live session or be recovered after further edits. A fresh paired
    // save establishes its relationship to the current document; recovery alone never does.
    history.invalidateSaved();
    history.invalidateSaved(SaveTarget::Tracker);
    return SaveOutcome::Recovered;
}

} // namespace motrack
