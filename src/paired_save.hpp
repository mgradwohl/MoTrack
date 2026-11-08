#pragma once

#include "workspace_document.hpp"

#include <QHash>
#include <QString>

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>

namespace motrack
{

enum class SaveDestinationConflict : std::uint8_t
{
    Busy,
    PendingRecovery
};

/// Structured adapter failure so desktop callers can provide translated recovery/retry guidance.
class SaveDestinationError final : public ProjectError
{
  public:
    explicit SaveDestinationError(SaveDestinationConflict conflict);
    [[nodiscard]] SaveDestinationConflict conflict() const noexcept
    {
        return m_Conflict;
    }

  private:
    SaveDestinationConflict m_Conflict;
};

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
    QString observedRecoveryPath; // Journal path observed under the lock, retained even after successful cleanup.
    QString transactionId;        // Persistent UUID identifies this journal instance, independent of its reusable path.
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
/// Write a prepared/rebased native copy while holding its sibling paired-save lock.
/// Refuses unresolved journals without changing any output; copies to other destinations remain available.
/// The caller still validates/rebases media before invoking this worker-only adapter.
void writeNativeDocument(const QString& path, const WorkspaceDocument& document, const std::atomic_bool* cancelled = nullptr);

/// Export a standalone image archive copy under the same per-stem destination lock as paired saving.
/// Refuses pending journals and file aliases; no session checkpoints or companion files are changed.
void writeTrackerImagePackageCopy(const QString& path,
                                  const WorkspaceDocument& document,
                                  const QString& mediaProjectPath,
                                  const std::atomic_bool* cancelled = nullptr);

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
