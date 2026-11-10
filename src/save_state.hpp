#pragma once

#include "paired_save.hpp"
#include "track_edits.hpp"

#include <QHash>
#include <QList>
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
    Recovered,
    Superseded // A newer native write already owns the destination and bindings.
};

enum class SaveChange : std::uint8_t
{
    Unchanged,
    Saved,
    Invalidated
};

/// Apply every returned action immediately on the owner thread, regardless of outcome.
/// Partial and stale writes can still change individual output checkpoints or media bindings.
struct SaveUpdate
{
    SaveOutcome outcome = SaveOutcome::Failed;
    SaveChange native = SaveChange::Unchanged;
    SaveChange tracker = SaveChange::Unchanged;
    bool adoptMediaLocations = false;
};

/// Owner-thread save destinations, recovery records and completion ordering, independent of edit history.
/// The owner supplies its revision and applies returned output actions to its own saved checkpoints.
/// Runtime identity survives in pending tokens; persistent document/group UUIDs never substitute for it.
class SaveState
{
    struct Identity
    {};

  public:
    struct Checkpoint
    {
        std::shared_ptr<const Identity> identity;
        std::uint64_t revision;
        QHash<QString, QString> recoveryTransactions;
        std::uint64_t nativeInvalidation = 0;
        std::uint64_t trackerInvalidation = 0;
    };

    void reset(const QString& source, DocumentSourceKind kind = DocumentSourceKind::Project);
    [[nodiscard]] const QString& sourcePath() const;
    [[nodiscard]] const QString& nativeSavePath() const;
    [[nodiscard]] const QString& standardSavePath() const;
    [[nodiscard]] const QString& recoveryPath() const;
    [[nodiscard]] bool requiresPairedSave() const;
    [[nodiscard]] Checkpoint saveCheckpoint(std::uint64_t revision) const;
    [[nodiscard]] SaveUpdate
    completeSave(const Checkpoint& checkpoint, std::uint64_t revision, const QString& destination, const NativeSaveResult& result);
    [[nodiscard]] SaveUpdate completePairedSave(const Checkpoint& checkpoint, std::uint64_t revision, const PairedSaveResult& result);
    /// Recovery always invalidates affected live checkpoints; it cannot establish current model equality.
    [[nodiscard]] SaveUpdate
    completePairedRecovery(const Checkpoint& checkpoint, const QString& requestedRecoveryPath, const PairedSaveResult& result);

    SaveState() = default;
    SaveState(const SaveState&) = delete;
    SaveState& operator=(const SaveState&) = delete;
    // Transfer ownership only; a moved-from state must be reset before reuse.
    SaveState(SaveState&&) noexcept = default;
    SaveState& operator=(SaveState&&) noexcept = default;
    ~SaveState() = default;

  private:
    struct RecoveryObservation
    {
        QString transaction;
        std::uint64_t observation;
    };
    void invalidateSaved(SaveTarget target, SaveUpdate& update);
    void rememberRecovery(const QString& path, const QString& transaction, std::uint64_t observation);
    void forgetRecovery(const QString& path, const QString& transaction, std::uint64_t observation, bool cleared);
    // Tokens retain the identity object, preventing reuse after replacement or destruction, even across sessions.
    std::shared_ptr<const Identity> m_Identity = std::make_shared<const Identity>();
    QString m_SourcePath;
    QString m_NativeSavePath;
    QString m_StandardSavePath;
    QStringList m_RecoveryPaths;
    QHash<QString, QList<RecoveryObservation>> m_RecoveryTransactions;
    QHash<QString, QHash<QString, std::uint64_t>> m_CompletedRecoveryTransactions;
    QHash<QString, std::uint64_t> m_ClearedRecoveryObservations;
    QHash<QString, std::shared_ptr<const QString>> m_RecoveryLocationIdentities;
    std::uint64_t m_NativeInvalidation = 0;
    std::uint64_t m_TrackerInvalidation = 0;
    std::uint64_t m_MediaObservation = 0;
    std::uint64_t m_NativeDestinationObservation = 0;
    std::uint64_t m_StandardDestinationObservation = 0;
    bool m_PairedSaving = false;
};

} // namespace motrack
