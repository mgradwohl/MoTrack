#pragma once

#include <QByteArray>
#include <QString>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

namespace motrack
{

class TrackerArchive;
inline constexpr qint64 MAX_WORKSPACE_JSON_BYTES = qint64{128} * 1024 * 1024;

struct WorkspaceStorage
{
    QByteArray json;
    std::shared_ptr<const TrackerArchive> archive;
};

/// Read legacy/plain JSON or a bounded native ZIP with exactly project.json and tracker/source.trz.
[[nodiscard]] WorkspaceStorage readWorkspaceStorage(const QString& path, const std::atomic_bool* cancelled);
enum class WorkspaceWriteProgress : std::uint8_t
{
    ArchiveChunkWritten,
    ReadyToCommit
};

/// Atomically replace with JSON, or a streamed ZIP retaining the exact owned Tracker archive snapshot.
/// Optional progress runs synchronously on the writing thread and may set cancellation before the next boundary.
void writeWorkspaceStorage(const QString& path,
                           const QByteArray& json,
                           const std::shared_ptr<const TrackerArchive>& archive,
                           const std::atomic_bool* cancelled = nullptr,
                           const std::function<void(WorkspaceWriteProgress)>& progress = {});

} // namespace motrack
