#pragma once

#include "save_state.hpp"
#include "track_edits.hpp"
#include "workspace_document.hpp"

#include <QHash>
#include <QString>

namespace motrack
{

/// One live document, its undo history and per-output save checkpoints. GUI-thread owned; workers receive copies.
class DocumentSession
{
  public:
    using SaveCheckpoint = SaveState::Checkpoint;

    WorkspaceDocument document;
    TrackEdits history;

    /// Install a successfully prepared document, clearing only this session's old history/save target.
    void replace(WorkspaceDocument incoming, const QString& source, DocumentSourceKind kind = DocumentSourceKind::Project);
    [[nodiscard]] const QString& sourcePath() const;
    [[nodiscard]] const QString& nativeSavePath() const;
    [[nodiscard]] const QString& standardSavePath() const;
    /// First unresolved record; later destinations never discard earlier interrupted saves.
    [[nodiscard]] const QString& recoveryPath() const;
    [[nodiscard]] bool requiresPairedSave() const;
    [[nodiscard]] bool isModified() const;
    [[nodiscard]] SaveCheckpoint saveCheckpoint() const;
    /// Failed/replaced/superseded writes leave this session untouched. A current stale write stays dirty.
    [[nodiscard]] SaveOutcome completeSave(const SaveCheckpoint& checkpoint, const QString& destination, const NativeSaveResult& result);
    /// Apply a new paired-write result, marking only outputs confirmed current for this revision.
    [[nodiscard]] SaveOutcome completePairedSave(const SaveCheckpoint& checkpoint, const PairedSaveResult& result);
    /// Capture a fresh checkpoint before starting recovery; it identifies the pending transaction.
    /// Recovery may belong to an older snapshot, even after restart. Never marks the live document clean.
    /// Successful recovery clears the pending record but requires a fresh save of the current snapshot.
    [[nodiscard]] SaveOutcome
    completePairedRecovery(const SaveCheckpoint& checkpoint, const QString& requestedRecoveryPath, const PairedSaveResult& result);

    DocumentSession() = default;
    DocumentSession(const DocumentSession&) = delete;
    DocumentSession& operator=(const DocumentSession&) = delete;
    DocumentSession(DocumentSession&&) = delete;
    DocumentSession& operator=(DocumentSession&&) = delete;
    ~DocumentSession() = default;

  private:
    SaveOutcome applySaveUpdate(const SaveUpdate& update, const QHash<QString, QString>& mediaLocations);
    SaveState m_SaveState;
};

} // namespace motrack
