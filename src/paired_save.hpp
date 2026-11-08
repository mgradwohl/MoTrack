#pragma once

#include "workspace_document.hpp"

#include <QString>

#include <atomic>
#include <cstdint>
#include <functional>

namespace motrack
{

enum class PairedSaveStatus : std::uint8_t
{
    Saved,
    Cancelled,
    Failed,
    RecoveryRequired,
    RecoveryNotNeeded // A previously identified journal is absent under the lock; no output was changed.
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
    QString transactionId; // Persistent UUID identifies this journal instance, independent of its reusable path.
    QString diagnostic;
    bool standardCurrent = false;
    bool nativeCurrent = false;
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
[[nodiscard]] PairedSaveResult recoverPairedDocuments(const QString& recoveryPath,
                                                      const std::atomic_bool* cancelled = nullptr,
                                                      const PairedSaveObserver& observer = {},
                                                      const QString& expectedTransaction = {});

} // namespace motrack
