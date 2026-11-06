#pragma once

#include <QByteArray>
#include <QString>

#include <atomic>
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
/// Atomically replace with JSON, or a streamed ZIP retaining the exact owned Tracker archive snapshot.
void writeWorkspaceStorage(const QString& path, const QByteArray& json, const std::shared_ptr<const TrackerArchive>& archive);

} // namespace motrack
