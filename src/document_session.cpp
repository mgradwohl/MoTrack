#include "document_session.hpp"

#include <utility>

namespace motrack
{

void DocumentSession::replace(WorkspaceDocument incoming, const QString& source, DocumentSourceKind kind)
{
    SaveState state;
    state.reset(source, kind);
    document = std::move(incoming);
    m_SaveState = std::move(state);
    history.clear();
}

const QString& DocumentSession::sourcePath() const
{
    return m_SaveState.sourcePath();
}

const QString& DocumentSession::nativeSavePath() const
{
    return m_SaveState.nativeSavePath();
}

const QString& DocumentSession::standardSavePath() const
{
    return m_SaveState.standardSavePath();
}

const QString& DocumentSession::recoveryPath() const
{
    return m_SaveState.recoveryPath();
}

bool DocumentSession::requiresPairedSave() const
{
    return m_SaveState.requiresPairedSave();
}

bool DocumentSession::isModified() const
{
    return history.isModified() || (requiresPairedSave() && (history.isModified(SaveTarget::Tracker) || !recoveryPath().isEmpty()));
}

DocumentSession::SaveCheckpoint DocumentSession::saveCheckpoint() const
{
    return m_SaveState.saveCheckpoint(history.revision());
}

SaveOutcome DocumentSession::applySaveUpdate(const SaveUpdate& update, const QHash<QString, QString>& mediaLocations)
{
    if (update.adoptMediaLocations)
        document.mediaLocations = mediaLocations;
    const auto apply = [this](SaveChange change, SaveTarget target)
    {
        if (change == SaveChange::Saved)
            history.markSaved(target);
        else if (change == SaveChange::Invalidated)
            history.invalidateSaved(target);
    };
    apply(update.native, SaveTarget::Native);
    apply(update.tracker, SaveTarget::Tracker);
    return update.outcome;
}

SaveOutcome DocumentSession::completeSave(const SaveCheckpoint& checkpoint, const QString& destination, const NativeSaveResult& result)
{
    return applySaveUpdate(m_SaveState.completeSave(checkpoint, history.revision(), destination, result), result.mediaLocations);
}

SaveOutcome DocumentSession::completePairedSave(const SaveCheckpoint& checkpoint, const PairedSaveResult& result)
{
    return applySaveUpdate(m_SaveState.completePairedSave(checkpoint, history.revision(), result), result.mediaLocations);
}

SaveOutcome DocumentSession::completePairedRecovery(const SaveCheckpoint& checkpoint,
                                                    const QString& requestedRecoveryPath,
                                                    const PairedSaveResult& result)
{
    return applySaveUpdate(m_SaveState.completePairedRecovery(checkpoint, requestedRecoveryPath, result), result.mediaLocations);
}

} // namespace motrack
