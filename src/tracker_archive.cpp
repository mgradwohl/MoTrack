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
    require(reader && archive_read_support_format_zip_seekable(reader.get()) == ARCHIVE_OK &&
            archive_read_set_format_option(reader.get(), "zip", "hdrcharset", "UTF-8") == ARCHIVE_OK);
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
        const auto key = member.normalized(QString::NormalizationForm_C).toCaseFolded();
        require(safeMember(member) && !seen.contains(key));
        seen.insert(key);
        if (type == AE_IFDIR)
        {
            require(destination.mkpath(member));
            continue;
        }
        require(archive_entry_size(entry) >= 0 && archive_entry_size(entry) <= remaining);
        require(destination.mkpath(QFileInfo(member).path()));
        QFile file(destination.absoluteFilePath(member));
        require(file.open(QIODevice::WriteOnly | QIODevice::NewOnly));
        QCryptographicHash digest(QCryptographicHash::Sha256);
        for (;;)
        {
            require(!cancelled || !*cancelled);
            const auto read = archive_read_data(reader.get(), buffer.data(), buffer.size());
            if (read == 0)
            {
                break;
            }
            require(read > 0 && read <= remaining && file.write(buffer.data(), read) == read);
            digest.addData(QByteArrayView(buffer.data(), read));
            remaining -= read;
        }
        require(file.flush());
        result.m_MemberDigests.insert(member, digest.result());
        if (member.endsWith(".trk", Qt::CaseInsensitive))
        {
            result.m_Projects.append(member);
        }
    }
    require(!result.m_Projects.isEmpty());
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
        if (separator >= 0)
        {
            value = value.mid(separator + 2);
        }
        else if (value.startsWith('/') || value.contains(':'))
        {
            reference.clear();
            continue;
        }
        else
        {
            value = parent + '/' + value;
        }
        const auto member = QDir::cleanPath(value).normalized(QString::NormalizationForm_C);
        reference = m_MemberDigests.contains(member) ? member.toStdString() : std::string{};
    }
}

} // namespace motrack
