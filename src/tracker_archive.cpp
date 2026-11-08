#include "tracker_archive.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSet>

#include <algorithm>
#include <array>
#include <clocale>
#include <memory>

#include <archive.h>
#include <archive_entry.h>

#ifdef __APPLE__
#include <xlocale.h>
#endif

namespace motrack
{
namespace
{

constexpr std::size_t READ_BLOCK_BYTES = std::size_t{64} * 1024;
constexpr qint64 MAX_EXTRACTED_BYTES = qint64{512} * 1024 * 1024;
constexpr int MAX_MEMBERS = 4096;

void require(bool valid)
{
    if (!valid)
    {
        throw ProjectError("Invalid or unsupported Tracker archive.");
    }
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
    return std::ranges::none_of(parts,
                                [](const QString& part) {
                                    return part.isEmpty() || part.endsWith('.') || part.endsWith(' ') ||
                                           part.section('.', 0, 0).trimmed().contains(device);
                                });
}

void snapshotArchive(const QString& path, const QString& destination, const std::atomic_bool* cancelled)
{
    require(!cancelled || !*cancelled);
    QFile input(path);
    require(QFileInfo(path).isFile() && input.open(QIODevice::ReadOnly));
    const auto expected = input.size();
    require(expected > 0 && expected <= MAX_TRACKER_ARCHIVE_BYTES);
    QFile output(destination);
    require(output.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    std::array<char, READ_BLOCK_BYTES> buffer{};
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
        copied += count;
    }
    require(copied == expected && output.flush());
}

} // namespace

TrackerArchive TrackerArchive::open(const QString& path, const std::atomic_bool* cancelled)
{
    TrackerArchive result;
    require(result.m_Directory.isValid() && result.m_SourceDirectory.isValid());
    result.m_CanonicalRoot = QFileInfo(result.m_Directory.path()).canonicalFilePath();
    require(!result.m_CanonicalRoot.isEmpty());
    snapshotArchive(path, result.sourcePath(), cancelled);
#ifndef _WIN32
    // libarchive converts ZIP names through the calling thread's character locale,
    // even when archive_entry_pathname_utf8 is requested. A C locale rejects Unicode.
    // Keep this conversion local to the archive call; never change the global locale.
    auto textLocale = newlocale(LC_CTYPE_MASK, "C.UTF-8", nullptr);
    if (!textLocale)
        textLocale = newlocale(LC_CTYPE_MASK, "en_US.UTF-8", nullptr);
    if (!textLocale)
        throw ProjectError("Could not create a UTF-8 locale for Tracker archive names.");
    const auto freeTextLocale = qScopeGuard([textLocale] { freelocale(textLocale); });
    const auto previousLocale = uselocale(textLocale);
    if (!previousLocale)
        throw ProjectError("Could not select a UTF-8 locale for Tracker archive names.");
    const auto restoreTextLocale = qScopeGuard([previousLocale] { uselocale(previousLocale); });
#endif
    const std::unique_ptr<archive, decltype(&archive_read_free)> reader(archive_read_new(), &archive_read_free);
    require(reader && archive_read_support_format_zip_seekable(reader.get()) == ARCHIVE_OK);
#ifdef _WIN32
    require(archive_read_open_filename_w(
                reader.get(), QDir::toNativeSeparators(result.sourcePath()).toStdWString().c_str(), READ_BLOCK_BYTES) == ARCHIVE_OK);
#else
    require(archive_read_open_filename(reader.get(), QFile::encodeName(result.sourcePath()).constData(), READ_BLOCK_BYTES) == ARCHIVE_OK);
#endif
    const QDir destination(result.m_Directory.path());
    QSet<QString> seen;
    std::array<char, READ_BLOCK_BYTES> buffer{};
    qint64 remaining = MAX_EXTRACTED_BYTES;
    int members = 0;
    archive_entry* entry = nullptr;
    for (auto status = archive_read_next_header(reader.get(), &entry); status != ARCHIVE_EOF;
         status = archive_read_next_header(reader.get(), &entry))
    {
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
    require(m_Projects.contains(member));
    return m_Directory.filePath(member);
}

QString TrackerArchive::root() const
{
    return m_Directory.path();
}

QString TrackerArchive::sourcePath() const
{
    return m_SourceDirectory.filePath("source.trz");
}

bool TrackerArchive::memberMatchesSource(const QString& member, const std::atomic_bool* cancelled) const
{
    require(!cancelled || !*cancelled);
    const auto expected = m_MemberDigests.constFind(member);
    if (expected == m_MemberDigests.cend())
        return false;
    QFile input(m_Directory.filePath(member));
    const QFileInfo info(input);
    if (!info.isFile() || !info.canonicalFilePath().startsWith(m_CanonicalRoot + '/') || !input.open(QIODevice::ReadOnly) ||
        input.size() > MAX_EXTRACTED_BYTES)
        return false;
    QCryptographicHash digest(QCryptographicHash::Sha256);
    std::array<char, READ_BLOCK_BYTES> buffer{};
    qint64 remaining = MAX_EXTRACTED_BYTES;
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
        const auto member = QDir::cleanPath(value);
        reference = m_MemberDigests.contains(member) ? member.toStdString() : std::string{};
    }
}

} // namespace motrack
