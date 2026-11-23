#pragma once

#include "legacy_project.hpp"

#include <QByteArray>
#include <QHash>
#include <QSet>
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

/// Whether @p path names a Tracker experiment archive by its suffix, in any letter case: a `.trz`, or a `.zip`, which
/// Tracker opens the same way (`ResourceLoader.isJarZipTrz`, #224). TrackerArchive::open reads either.
[[nodiscard]] bool isTrackerArchivePath(const QString& path);
/// Whether @p path names a `.zip` experiment archive. It is never a save target: Tracker never writes into the archive
/// a tab came from, and its archive export always writes `.trz`. A standard save asks for a `.trz` instead (#224).
[[nodiscard]] bool isTrackerZipPath(const QString& path);

namespace detail
{
/// Whether a libarchive header warning is a member or link name that is not UTF-8 (hdrcharset=UTF-8), as opposed
/// to another recoverable header problem.
[[nodiscard]] bool isNameConversionWarning(std::string_view libarchiveError) noexcept;
} // namespace detail

/// Owns a temporary, validated extraction of a Tracker experiment archive.
/// Members with unsafe names (absolute, drive-letter, UNC, '\', '.'/'..', empty-segment, control-character,
/// reserved-device or overlong) are never extracted, listed as projects or used to resolve media; an
/// unsafe project (.trk) name, or a media reference naming an unsafe member, rejects the archive.
/// So does an archive with no project, which Tracker reports as "Not a Tracker file".
class TrackerArchive
{
  public:
    [[nodiscard]] static TrackerArchive open(const QString& path, const std::atomic_bool* cancelled = nullptr);
    /// Extracted .trk members, sorted. macOS AppleDouble files (magic 0x00051607, or "._" names under
    /// `__MACOSX/`) are not projects.
    [[nodiscard]] const QStringList& projects() const;
    [[nodiscard]] QString projectPath(const QString& member) const;
    [[nodiscard]] QString root() const;
    /// Exact compressed input snapshot, owned until this archive is destroyed. Extraction uses these bytes.
    [[nodiscard]] QString sourcePath() const;
    /// Immutable SHA-256 of the exact compressed bytes captured at import.
    [[nodiscard]] const QByteArray& sourceDigest() const;
    /// Archive-order names of members skipped because their names are unsafe. They are never extracted;
    /// their bytes stay in the source snapshot, and standard saves copy them except omittedOnSave().
    [[nodiscard]] const QStringList& skippedMembers() const;
    /// Skipped members that standard saves omit: exact copies of an extracted member with the same file
    /// name (OSP JarTool absolute copies), which Tracker never uses.
    [[nodiscard]] const QStringList& omittedOnSave() const;
    /// Import-time SHA-256 of each extracted regular member, keyed by its exact archive name.
    [[nodiscard]] const QHash<QString, QByteArray>& memberDigests() const;
    /// Maps references to extracted member names; unresolved references become empty and are never
    /// looked up on disk. Throws ProjectError when a reference names a skipped unsafe member.
    void resolveMedia(LegacyProject& project, const QString& projectMember) const;
    /// Checks extracted bytes against their immutable import-time digest, with bounded, cancellable I/O.
    [[nodiscard]] bool memberMatchesSource(const QString& member, const std::atomic_bool* cancelled = nullptr) const;

  private:
    QTemporaryDir m_Directory;
    QTemporaryDir m_SourceDirectory;
    QString m_CanonicalRoot;
    QByteArray m_SourceDigest;
    QStringList m_Projects;
    QStringList m_SkippedMembers;
    QStringList m_OmittedOnSave;
    QSet<QString> m_SkippedKeys;
    QHash<QString, QByteArray> m_MemberDigests;
};

} // namespace motrack
