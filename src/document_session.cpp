#include "document_session.hpp"

#include <algorithm>
#include <utility>

namespace motrack
{

void DocumentSession::replace(WorkspaceDocument incoming, const QString& source, DocumentSourceKind kind)
{
    SaveState state;
    state.reset(source, kind);
    const auto baseline = std::make_shared<const LegacyProject>(incoming.project);
    document = std::move(incoming);
    m_SaveState = std::move(state);
    history.clear();
    m_SavedProjects.fill(baseline);
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

std::shared_ptr<const LegacyProject> DocumentSession::captureSavedProject(const SaveCheckpoint& checkpoint,
                                                                          const LegacyProject* snapshot) const
{
    if (checkpoint.identity != saveCheckpoint().identity)
        return {};
    // Legacy callers may omit the snapshot only when its model still equals the live revision.
    if (!snapshot)
    {
        if (checkpoint.revision != history.revision())
            return {};
        snapshot = &document.project;
    }
    return std::make_shared<const LegacyProject>(*snapshot);
}

void DocumentSession::applySavedProject(const SaveUpdate& update, const std::shared_ptr<const LegacyProject>& snapshot)
{
    const auto apply = [&](std::size_t index, bool accepted, SaveChange change)
    {
        if (accepted)
            m_SavedProjects[index] = snapshot;
        else if (change == SaveChange::Invalidated)
            m_SavedProjects[index].reset();
    };
    apply(0, update.nativeSnapshotAccepted, update.native);
    apply(1, update.trackerSnapshotAccepted, update.tracker);
}

bool DocumentSession::canRestoreSavedProject(bool requireTracker) const
{
    if (!recoveryPath().isEmpty() || !m_SavedProjects[0])
        return false;
    const bool matching = m_SavedProjects[1] && *m_SavedProjects[0] == *m_SavedProjects[1];
    return !(requireTracker || requiresPairedSave()) || matching;
}

bool DocumentSession::restoreSavedProject(bool requireTracker)
{
    if (!canRestoreSavedProject(requireTracker))
        return false;
    const bool matching = m_SavedProjects[1] && *m_SavedProjects[0] == *m_SavedProjects[1];
    auto restored = *m_SavedProjects[0]; // Allocate before changing any live model/history.
    // Like ordinary undo, reverting cannot recycle identities allocated by discarded edits.
    restored.nextTrackId = std::max(restored.nextTrackId, document.project.nextTrackId);
    // Indexing can discover the full movie after opening an older saved clip. That derived extent
    // belongs to the retained media, not the edits being discarded; keep the saved clip selection.
    if (const auto knownFrames = document.project.clip.sourceFrameCount;
        document.movie && knownFrames && restored.clip.mediaReferences == document.project.clip.mediaReferences)
        restored.clip.sourceFrameCount = std::max(restored.clip.sourceFrameCount.value_or(0), *knownFrames);
    document.project = std::move(restored);
    history.clear();
    if (!matching)
        history.invalidateSaved(SaveTarget::Tracker);
    return true;
}

SaveOutcome DocumentSession::applySaveUpdate(const SaveUpdate& update,
                                             const QHash<QString, QString>& mediaLocations,
                                             const std::shared_ptr<const LegacyProject>& snapshot)
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
    applySavedProject(update, snapshot);
    return update.outcome;
}

SaveOutcome DocumentSession::completeSave(const SaveCheckpoint& checkpoint,
                                          const QString& destination,
                                          const NativeSaveResult& result,
                                          const LegacyProject* snapshot)
{
    std::shared_ptr<const LegacyProject> saved;
    if (result.saved)
        saved = captureSavedProject(checkpoint, snapshot);
    return applySaveUpdate(m_SaveState.completeSave(checkpoint, history.revision(), destination, result), result.mediaLocations, saved);
}

SaveOutcome
DocumentSession::completePairedSave(const SaveCheckpoint& checkpoint, const PairedSaveResult& result, const LegacyProject* snapshot)
{
    std::shared_ptr<const LegacyProject> saved;
    if (result.nativeCurrent || result.standardCurrent)
        saved = captureSavedProject(checkpoint, snapshot);
    return applySaveUpdate(m_SaveState.completePairedSave(checkpoint, history.revision(), result), result.mediaLocations, saved);
}

SaveOutcome DocumentSession::completePairedRecovery(const SaveCheckpoint& checkpoint,
                                                    const QString& requestedRecoveryPath,
                                                    const PairedSaveResult& result)
{
    return applySaveUpdate(m_SaveState.completePairedRecovery(checkpoint, requestedRecoveryPath, result), result.mediaLocations);
}

} // namespace motrack
