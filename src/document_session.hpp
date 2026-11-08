#pragma once

#include "paired_save.hpp"
#include "track_edits.hpp"
#include "workspace_document.hpp"

#include <QHash>
#include <QSet>
#include <QStringList>

#include <cstdint>
#include <memory>

namespace motrack
{

enum class DocumentSourceKind : std::uint8_t
{
    Project,
    Media
};

enum class SaveOutcome : std::uint8_t
{
    Saved,
    Failed,
    Stale,
    Replaced,
    Partial,
    Recovered
};

/// One live document, its undo history and per-output save checkpoints. GUI-thread owned; workers receive copies.
class DocumentSession
{
    struct Identity
    {};

  public:
    struct SaveCheckpoint
    {
        std::shared_ptr<const Identity> identity;
        std::uint64_t revision;
        QHash<QString, QString> recoveryTransactions;
    };

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
    /// Failed/replaced writes leave this session untouched. A successful stale write keeps its destination but stays dirty.
    [[nodiscard]] SaveOutcome completeSave(const SaveCheckpoint& checkpoint, const QString& destination, bool success);
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
    void rememberRecovery(const QString& path, const QString& transaction);
    void forgetRecovery(const QString& path, const QString& transaction);
    // Tokens retain the identity object, preventing reuse after replacement or destruction, even across sessions.
    std::shared_ptr<const Identity> m_Identity = std::make_shared<const Identity>();
    QString m_SourcePath;
    QString m_NativeSavePath;
    QString m_StandardSavePath;
    QStringList m_RecoveryPaths;
    QHash<QString, QStringList> m_RecoveryTransactions;
    QSet<QString> m_CompletedRecoveryTransactions;
    bool m_PairedSaving = false;
};

} // namespace motrack
