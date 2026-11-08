#pragma once

#include "workspace_document.hpp"

#include <QHash>
#include <QString>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace motrack
{

enum class PairedSaveStatus : std::uint8_t
{
    Saved,
    Cancelled,
    Failed,
    RecoveryRequired,
    RecoveryNotNeeded // The journal is absent under the lock; no output was changed.
};

enum class PairedSaveProgress : std::uint8_t
{
    StandardPrepared,
    NativePrepared,
    RecoveryRecorded,
    StandardSaved,
    NativeSaved
};

struct PairedSaveResult
{
    PairedSaveStatus status = PairedSaveStatus::Failed;
    QString standardPath;
    QString nativePath;
    QString recoveryPath;
    QString observedRecoveryPath;                    // Journal path observed under the lock, retained even after successful cleanup.
    std::shared_ptr<const QString> recoveryIdentity; // Process-local filesystem slot identity; never serialized.
    QString transactionId;                           // Persistent UUID identifies this journal instance, independent of its reusable path.
    QString diagnostic;
    bool standardCurrent = false;
    bool nativeCurrent = false;
    // Process-local observation order, assigned before releasing the journal lock; never persisted.
    std::uint64_t observation = 0;
    // Resolved original-reference bindings from this write's snapshot, for successful live-session adoption.
    // Runtime-only: never serialized in the recovery record or used to reconstruct a recovered snapshot.
    QHash<QString, QString> mediaLocations = {};
};

/// Worker-only progress observer; it must not access widgets or mutate the supplied document.
using PairedSaveObserver = std::function<void(PairedSaveProgress)>;

/// Prepare one audited standard file and its same-stem native companion, then commit standard first.
/// A partial commit retains recovery data; never reports the whole pair saved. Does not update a live session.
[[nodiscard]] PairedSaveResult writePairedDocuments(const QString& standardPath,
                                                    const WorkspaceDocument& document,
                                                    const QString& mediaProjectPath,
                                                    const std::atomic_bool* cancelled = nullptr,
                                                    const PairedSaveObserver& observer = {});
/// Explicitly resume a locally initiated pair. Refuses altered destinations, stages, aliases and escaping paths.
/// Recovery is never implicitly triggered by opening imported project/archive content.
/// When expectedTransaction is supplied, check it while holding the journal lock, before any output changes.
/// A different transaction is returned as RecoveryRequired for explicit reconfirmation; an absent one is RecoveryNotNeeded.
/// An explicitly supplied empty ID means unidentified: observe a valid ID without committing, requiring reconfirmation.
/// Omitting expectedTransaction is an unrestricted explicit adapter recovery. Absence can reconcile either form.
[[nodiscard]] PairedSaveResult recoverPairedDocuments(const QString& recoveryPath,
                                                      const std::atomic_bool* cancelled = nullptr,
                                                      const PairedSaveObserver& observer = {},
                                                      const std::optional<QString>& expectedTransaction = std::nullopt);

} // namespace motrack
