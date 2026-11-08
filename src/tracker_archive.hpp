#pragma once

#include "legacy_project.hpp"

#include <QByteArray>
#include <QHash>
#include <QStringList>
#include <QTemporaryDir>

#include <atomic>

namespace motrack
{

inline constexpr qint64 MAX_TRACKER_ARCHIVE_BYTES = qint64{544} * 1024 * 1024;

/// Owns a temporary, validated extraction of a Tracker experiment archive.
class TrackerArchive
{
  public:
    [[nodiscard]] static TrackerArchive open(const QString& path, const std::atomic_bool* cancelled = nullptr);
    [[nodiscard]] const QStringList& projects() const;
    [[nodiscard]] QString projectPath(const QString& member) const;
    [[nodiscard]] QString root() const;
    /// Exact compressed input snapshot, owned until this archive is destroyed. Extraction uses these bytes.
    [[nodiscard]] QString sourcePath() const;
    void resolveMedia(LegacyProject& project, const QString& projectMember) const;
    /// Checks extracted bytes against their immutable import-time digest, with bounded, cancellable I/O.
    [[nodiscard]] bool memberMatchesSource(const QString& member, const std::atomic_bool* cancelled = nullptr) const;

  private:
    QTemporaryDir m_Directory;
    QTemporaryDir m_SourceDirectory;
    QString m_CanonicalRoot;
    QStringList m_Projects;
    QHash<QString, QByteArray> m_MemberDigests;
};

} // namespace motrack
