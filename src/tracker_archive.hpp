#pragma once

#include "legacy_project.hpp"

#include <QByteArray>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QStringView>
#include <QTemporaryDir>

#include <atomic>
#include <optional>
#include <string>
#include <string_view>

namespace motrack
{

inline constexpr qint64 MAX_TRACKER_ARCHIVE_BYTES = qint64{544} * 1024 * 1024;
inline constexpr qint64 MAX_TRACKER_EXTRACTED_BYTES = qint64{512} * 1024 * 1024;

/// Archive member names have two spellings (#308):
///
/// - The **stored name** is the member's bytes in the ZIP. Tracker uses them as they are: java.util.zip never
///   normalizes Unicode, and OSP finds a media file only when the reference in the experiment spells the stored
///   name exactly. "é" stored precomposed (U+00E9) and decomposed ("e" + U+0301, as macOS spells file names) are
///   different names to Tracker. MoTrack never changes a stored name, and never respells a reference to one: saving
///   into an archive copies every name's bytes and leaves the experiment's media references as its XML has them.
/// - The **identity** is the NFC form of the stored name, which this function returns. libarchive normalizes the
///   names it reads (to NFD on macOS, to NFC elsewhere), so the stored form never reaches MoTrack; the identity is
///   the same on every platform. Everything inside MoTrack names a member by its identity: TrackerArchive's lists,
///   digests, extraction paths and lookups, the in-place writer's matching, group membership and native companions.
///   A name from outside (a media reference, a saved companion, a tabset entry) goes through this function before
///   it is compared. Two stored names with one identity make the archive ambiguous, and it is refused.
[[nodiscard]] QString archiveMemberKey(QStringView member);
[[nodiscard]] std::string archiveMemberKey(std::string_view member);

/// The NFC names of the `.trk` members of the Tracker archive at @p path, read from its member headers alone within
/// TrackerArchive::open's size and member bounds, without extracting anything. Nullopt when the file cannot be read as
/// a Tracker archive, including a member name that is not UTF-8. Finder metadata under `__MACOSX/` is left out by
/// name; TrackerArchive::open still decides which members are projects.
[[nodiscard]] std::optional<QSet<QString>> trackerArchiveProjectNames(const QString& path);

namespace detail
{
/// Whether a libarchive header warning is a member or link name that is not UTF-8 (hdrcharset=UTF-8), as opposed
/// to another recoverable header problem.
[[nodiscard]] bool isNameConversionWarning(std::string_view libarchiveError) noexcept;
} // namespace detail

/// Owns a temporary, validated extraction of a Tracker experiment archive.
/// Members with unsafe names (absolute, drive-letter, UNC, '\', '.'/'..', empty-segment, control-character,
/// replacement-character, reserved-device or overlong) are never extracted, listed as projects or used to resolve
/// media; an unsafe project (.trk) name, or a media reference naming an unsafe member, rejects the archive.
/// Member names are UTF-8 whether or not their ZIP UTF-8 flag is set, as Tracker reads them; a name that is not
/// UTF-8 rejects the archive, as it does in Tracker (#275).
class TrackerArchive
{
  public:
    [[nodiscard]] static TrackerArchive open(const QString& path, const std::atomic_bool* cancelled = nullptr);
    /// Extracted .trk members, sorted. macOS AppleDouble files (magic 0x00051607, or "._" names under
    /// `__MACOSX/`) are not projects.
    [[nodiscard]] const QStringList& projects() const;
    /// Extracted path of a project. Like every lookup here, @p member may be in any normalization form.
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
    /// Import-time SHA-256 of each extracted regular member, keyed by its identity (archiveMemberKey).
    [[nodiscard]] const QHash<QString, QByteArray>& memberDigests() const;
    /// Maps references to the identities of extracted members; unresolved references become empty and are never
    /// looked up on disk. Throws ProjectError when a reference names a skipped unsafe member. A reference that
    /// differs from a stored name only in Unicode normalization resolves here, though Tracker would not find it.
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
