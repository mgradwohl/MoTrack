#include "tracker_archive.hpp"

#include "utf8_thread_locale.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <array>
#include <memory>
#include <utility>
#include <vector>

#include <archive.h>
#include <archive_entry.h>

namespace motrack
{
namespace
{

constexpr std::size_t READ_BLOCK_BYTES = std::size_t{64} * 1024;
constexpr int MAX_MEMBERS = 4096;

void require(bool valid)
{
    if (!valid)
    {
        throw ProjectError("Invalid or unsupported Tracker archive.");
    }
}

/// A member name that is not UTF-8. Tracker's java.util.zip refuses the whole archive too (#275).
[[noreturn]] void refuseNonUtf8Name()
{
    throw ProjectError(QCoreApplication::translate("TrackerArchive",
                                                   "A member name in this Tracker archive is not valid UTF-8 text, so the archive "
                                                   "cannot be opened. Tracker cannot open it either.")
                           .toStdString());
}

bool safeMember(const QString& path)
{
    static const QRegularExpression forbidden(R"([\\:*?"<>|])");
    static const QRegularExpression device(R"(^(CON|CONIN\$|CONOUT\$|PRN|AUX|NUL|COM[0-9¹²³]|LPT[0-9¹²³])$)",
                                           QRegularExpression::CaseInsensitiveOption);
    if (path.size() > 512 || path.contains(QChar::ReplacementCharacter) || path.contains(forbidden) ||
        std::ranges::any_of(path, [](QChar character) { return character.unicode() < 32; }))
    {
        return false;
    }
    const auto parts = path.split('/');
    return std::ranges::none_of(
        parts,
        [](const QString& part)
        { return part.isEmpty() || part.endsWith('.') || part.endsWith(' ') || part.section('.', 0, 0).trimmed().contains(device); });
}

/// Platform-independent comparison form shared by duplicate detection and media lookup: '\' read as '/',
/// the leading '/' run kept, empty and '.' segments dropped, '..' resolved against a preceding segment,
/// then Unicode NFC and case folding. A safe name changes only in Unicode form and case.
QString memberKey(QString member)
{
    member.replace('\\', '/');
    qsizetype root = 0;
    while (root < member.size() && member[root] == '/')
        ++root;
    QStringList segments;
    for (const auto& segment : member.sliced(root).split('/', Qt::SkipEmptyParts))
    {
        if (segment == QStringLiteral("."))
            continue;
        if (segment == QStringLiteral("..") && !segments.isEmpty() && segments.constLast() != QStringLiteral(".."))
            segments.removeLast();
        else
            segments.push_back(segment);
    }
    return (QString(root, '/') + segments.join('/')).normalized(QString::NormalizationForm_C).toCaseFolded();
}

/// The final name segment, whichever separator the name uses.
QString fileName(const QString& member)
{
    return member.mid((std::max)(member.lastIndexOf('/'), member.lastIndexOf('\\')) + 1);
}

/// Finder metadata never holds a Tracker project, even when its name copies a project's ".trk" suffix.
bool appleDouble(const QString& member, const QByteArray& head)
{
    static const auto magic = QByteArray::fromHex("00051607");
    return head.startsWith(magic) || (fileName(member).startsWith("._") && member.split('/').contains(QStringLiteral("__MACOSX")));
}

QByteArray snapshotArchive(const QString& path, const QString& destination, const std::atomic_bool* cancelled)
{
    require(!cancelled || !*cancelled);
    QFile input(path);
    require(QFileInfo(path).isFile() && input.open(QIODevice::ReadOnly));
    const auto expected = input.size();
    require(expected > 0 && expected <= MAX_TRACKER_ARCHIVE_BYTES);
    QFile output(destination);
    require(output.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    std::array<char, READ_BLOCK_BYTES> buffer{};
    QCryptographicHash digest(QCryptographicHash::Sha256);
    qint64 copied = 0;
    for (;;)
    {
        require(!cancelled || !*cancelled);
        const auto count = input.read(buffer.data(), static_cast<qint64>(buffer.size()));
        require(count >= 0 && count <= MAX_TRACKER_ARCHIVE_BYTES - copied);
        if (count == 0)
        {
            break;
        }
        require(output.write(buffer.data(), count) == count);
        digest.addData(QByteArrayView(buffer.data(), count));
        copied += count;
    }
    require(copied == expected && output.flush());
    return digest.result();
}

} // namespace

std::string archiveMemberKey(std::string_view member)
{
    return QString::fromUtf8(member.data(), static_cast<qsizetype>(member.size())).normalized(QString::NormalizationForm_C).toStdString();
}

bool detail::isNameConversionWarning(std::string_view libarchiveError) noexcept
{
    // libarchive's ZIP reader: "Pathname cannot be converted from UTF-8 to current locale." and the same for
    // "Symlink". CRC, size and truncation problems keep the generic message.
    return libarchiveError.starts_with("Pathname cannot be converted") || libarchiveError.starts_with("Symlink cannot be converted");
}

TrackerArchive TrackerArchive::open(const QString& path, const std::atomic_bool* cancelled)
{
    TrackerArchive result;
    require(result.m_Directory.isValid() && result.m_SourceDirectory.isValid());
    result.m_CanonicalRoot = QFileInfo(result.m_Directory.path()).canonicalFilePath();
    require(!result.m_CanonicalRoot.isEmpty());
    result.m_SourceDigest = snapshotArchive(path, result.sourcePath(), cancelled);
    // libarchive converts names through the thread's LC_CTYPE on POSIX; outlives the reader below.
    const ScopedUtf8ThreadLocale utf8Names;
    const std::unique_ptr<archive, decltype(&archive_read_free)> reader(archive_read_new(), &archive_read_free);
    // Tracker reads archives with java.util.zip, which decodes every member name as UTF-8 whether or not the
    // name's UTF-8 flag (general-purpose bit 11) is set, and refuses a name that is not UTF-8. libarchive would
    // otherwise decode an unflagged name through the C locale or the OEM code page, which differ between motrack,
    // the test binaries and the user's system (#275; CONTRIBUTING.md, "Tracker compatibility exceptions").
    // macOS builds of libarchive hide "__MACOSX/" members by default, bypassing every check below and
    // differing from the raw records the writer copies. Read the same entries on every platform.
    require(reader && archive_read_support_format_zip_seekable(reader.get()) == ARCHIVE_OK &&
            archive_read_set_format_option(reader.get(), "zip", "hdrcharset", "UTF-8") == ARCHIVE_OK &&
            archive_read_set_options(reader.get(), "zip:!mac-ext") == ARCHIVE_OK);
#ifdef _WIN32
    require(archive_read_open_filename_w(
                reader.get(), QDir::toNativeSeparators(result.sourcePath()).toStdWString().c_str(), READ_BLOCK_BYTES) == ARCHIVE_OK);
#else
    require(archive_read_open_filename(reader.get(), QFile::encodeName(result.sourcePath()).constData(), READ_BLOCK_BYTES) == ARCHIVE_OK);
#endif
    const QDir destination(result.m_Directory.path());
    QSet<QString> seen;
    std::array<char, READ_BLOCK_BYTES> buffer{};
    qint64 remaining = MAX_TRACKER_EXTRACTED_BYTES;
    int members = 0;
    archive_entry* entry = nullptr;
    QByteArray head;
    // Bounded, cancellable read of the current member: SHA-256, first bytes, and an optional copy.
    const auto readMember = [&](QFile* output)
    {
        require(archive_entry_size(entry) >= 0 && archive_entry_size(entry) <= remaining);
        QCryptographicHash digest(QCryptographicHash::Sha256);
        head.clear();
        for (;;)
        {
            require(!cancelled || !*cancelled);
            const auto read = archive_read_data(reader.get(), buffer.data(), buffer.size());
            if (read == 0)
            {
                return digest.result();
            }
            require(read > 0 && read <= remaining && (!output || output->write(buffer.data(), read) == read));
            digest.addData(QByteArrayView(buffer.data(), read));
            if (head.size() < 4)
            {
                head.append(buffer.data(), std::min<qsizetype>(4 - head.size(), read));
            }
            remaining -= read;
        }
    };
    std::vector<std::pair<QString, QByteArray>> skippedFiles;
    for (auto status = archive_read_next_header(reader.get(), &entry); status != ARCHIVE_EOF;
         status = archive_read_next_header(reader.get(), &entry))
    {
        const auto* warning = status == ARCHIVE_WARN ? archive_error_string(reader.get()) : nullptr;
        if (warning != nullptr && detail::isNameConversionWarning(warning))
        {
            refuseNonUtf8Name();
        }
        if (status != ARCHIVE_OK)
        {
            const auto* error = archive_error_string(reader.get());
            throw ProjectError(std::string("Could not read Tracker archive member: ") + (error ? error : "unknown archive error"));
        }
        require((!cancelled || !*cancelled) && ++members <= MAX_MEMBERS && entry && archive_entry_is_encrypted(entry) == 0 &&
                !archive_entry_hardlink(entry));
        const auto type = archive_entry_filetype(entry);
        require(type == AE_IFREG || type == AE_IFDIR);
        const auto* utf8 = archive_entry_pathname_utf8(entry);
        auto member = QString::fromUtf8(utf8 ? utf8 : archive_entry_pathname(entry));
        if (type == AE_IFDIR && member.endsWith('/'))
        {
            member.chop(1);
        }
        // Member identities are NFC. libarchive gives macOS callers NFD names, and names that differ only in
        // normalization are refused as duplicates below anyway; lookups normalize the same way (#308).
        member = member.normalized(QString::NormalizationForm_C);
        if (member.contains(QChar::ReplacementCharacter))
        {
            refuseNonUtf8Name(); // Windows' UTF-8 decoding substitutes U+FFFD instead of failing
        }
        // Every name, extracted or not, takes part in duplicate detection: an unsafe spelling of a safe
        // member's name (such as "videos\a.mp4" or "./videos/a.mp4") would leave the archive ambiguous.
        const auto key = memberKey(member);
        require(!seen.contains(key));
        seen.insert(key);
        if (!safeMember(member))
        {
            // Tracker would list an unsafe .trk as a tab; MoTrack refuses instead of extracting it by that name.
            if (type == AE_IFREG && member.endsWith(".trk", Qt::CaseInsensitive))
            {
                throw ProjectError(
                    QCoreApplication::translate(
                        "TrackerArchive",
                        "This Tracker archive contains a project with an unsafe member name, which MoTrack does not extract.")
                        .toStdString());
            }
            // Never extracted, opened or followed. Only its digest is read, to recognize redundant copies.
            result.m_SkippedMembers.append(member);
            result.m_SkippedKeys.insert(key);
            if (type == AE_IFREG)
            {
                skippedFiles.emplace_back(member, readMember(nullptr));
            }
            continue;
        }
        if (type == AE_IFDIR)
        {
            require(destination.mkpath(member));
            continue;
        }
        require(destination.mkpath(QFileInfo(member).path()));
        QFile file(destination.absoluteFilePath(member));
        require(file.open(QIODevice::WriteOnly | QIODevice::NewOnly));
        result.m_MemberDigests.insert(member, readMember(&file));
        require(file.flush());
        if (member.endsWith(".trk", Qt::CaseInsensitive) && !appleDouble(member, head))
        {
            result.m_Projects.append(member);
        }
    }
    require(!result.m_Projects.isEmpty());
    // Standard saves drop only skipped members that duplicate an extracted member with the same file name
    // (the OSP JarTool absolute copies). Every other skipped member, Finder icons included, is kept.
    QSet<std::pair<QString, QByteArray>> extracted;
    for (auto member = result.m_MemberDigests.cbegin(); member != result.m_MemberDigests.cend(); ++member)
    {
        extracted.insert({fileName(member.key()), member.value()});
    }
    for (const auto& member : std::as_const(result.m_SkippedMembers))
    {
        const auto file = std::ranges::find(skippedFiles, member, &std::pair<QString, QByteArray>::first);
        if (file != skippedFiles.end() && extracted.contains({fileName(member), file->second}))
        {
            result.m_OmittedOnSave.append(member);
        }
    }
    result.m_Projects.sort();
    return result;
}

const QStringList& TrackerArchive::projects() const
{
    return m_Projects;
}

QString TrackerArchive::projectPath(const QString& member) const
{
    const auto key = member.normalized(QString::NormalizationForm_C);
    require(m_Projects.contains(key));
    return m_Directory.filePath(key);
}

QString TrackerArchive::root() const
{
    return m_Directory.path();
}

QString TrackerArchive::sourcePath() const
{
    return m_SourceDirectory.filePath("source.trz");
}

const QByteArray& TrackerArchive::sourceDigest() const
{
    return m_SourceDigest;
}

const QStringList& TrackerArchive::skippedMembers() const
{
    return m_SkippedMembers;
}

const QStringList& TrackerArchive::omittedOnSave() const
{
    return m_OmittedOnSave;
}

const QHash<QString, QByteArray>& TrackerArchive::memberDigests() const
{
    return m_MemberDigests;
}

bool TrackerArchive::memberMatchesSource(const QString& member, const std::atomic_bool* cancelled) const
{
    require(!cancelled || !*cancelled);
    const auto key = member.normalized(QString::NormalizationForm_C);
    const auto expected = m_MemberDigests.constFind(key);
    if (expected == m_MemberDigests.cend())
        return false;
    QFile input(m_Directory.filePath(key));
    const QFileInfo info(input);
    if (!info.isFile() || !info.canonicalFilePath().startsWith(m_CanonicalRoot + '/') || !input.open(QIODevice::ReadOnly) ||
        input.size() > MAX_TRACKER_EXTRACTED_BYTES)
        return false;
    QCryptographicHash digest(QCryptographicHash::Sha256);
    std::array<char, READ_BLOCK_BYTES> buffer{};
    qint64 remaining = MAX_TRACKER_EXTRACTED_BYTES;
    for (;;)
    {
        require(!cancelled || !*cancelled);
        const auto count = input.read(buffer.data(), static_cast<qint64>(buffer.size()));
        if (count < 0 || count > remaining)
            return false;
        if (count == 0)
            return digest.result() == *expected;
        digest.addData(QByteArrayView(buffer.data(), count));
        remaining -= count;
    }
}

void TrackerArchive::resolveMedia(LegacyProject& project, const QString& projectMember) const
{
    const auto parent = projectMember.isEmpty() ? QString(".") : QFileInfo(projectMember).path();
    for (auto& reference : project.clip.mediaReferences)
    {
        auto value = QString::fromStdString(reference).replace('\\', '/');
        const auto separator = value.lastIndexOf("!/");
        // Local and URL paths are never opened; such a path can only match a skipped member below.
        const bool external = separator < 0 && (value.startsWith('/') || value.contains(':'));
        if (separator >= 0)
        {
            value = value.mid(separator + 2);
        }
        else if (!external)
        {
            value = parent + '/' + value;
        }
        const auto member = QDir::cleanPath(value).normalized(QString::NormalizationForm_C);
        if (m_SkippedKeys.contains(memberKey(value)))
        {
            throw ProjectError(QCoreApplication::translate(
                                   "TrackerArchive",
                                   "This Tracker project refers to an archive member with an unsafe name, which MoTrack does not extract.")
                                   .toStdString());
        }
        reference = !external && m_MemberDigests.contains(member) ? member.toStdString() : std::string{};
    }
}

} // namespace motrack
