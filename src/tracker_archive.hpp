#pragma once

#include "legacy_project.hpp"

#include <QByteArray>
#include <QHash>
#include <QStringList>
#include <QTemporaryDir>

#include <atomic>
#include <string>
#include <string_view>

namespace motrack
{

inline constexpr qint64 MAX_TRACKER_ARCHIVE_BYTES = qint64{544} * 1024 * 1024;
inline constexpr qint64 MAX_TRACKER_EXTRACTED_BYTES = qint64{512} * 1024 * 1024;

/// The identity of archive member @p member: its NFC form. TrackerArchive's names and lookups use it, so a member
/// name stored or compared elsewhere (native companions, group membership) goes through it too (#308).
[[nodiscard]] std::string archiveMemberKey(std::string_view member);

namespace detail
{
/// Whether a libarchive header warning is a member or link name that is not UTF-8 (hdrcharset=UTF-8), as opposed
/// to another recoverable header problem.
[[nodiscard]] bool isNameConversionWarning(std::string_view libarchiveError) noexcept;
} // namespace detail

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
    /// Immutable SHA-256 of the exact compressed bytes captured at import.
    [[nodiscard]] const QByteArray& sourceDigest() const;
    void resolveMedia(LegacyProject& project, const QString& projectMember) const;
    /// Checks extracted bytes against their immutable import-time digest, with bounded, cancellable I/O.
    [[nodiscard]] bool memberMatchesSource(const QString& member, const std::atomic_bool* cancelled = nullptr) const;

  private:
    QTemporaryDir m_Directory;
    QTemporaryDir m_SourceDirectory;
    QString m_CanonicalRoot;
    QByteArray m_SourceDigest;
    QStringList m_Projects;
    QHash<QString, QByteArray> m_MemberDigests;
};

} // namespace motrack
