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
#include <utility>
#include <vector>

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

/// Comparison form shared by duplicate detection and media lookup: '/' separators, '.' and '..' resolved
/// where possible, Unicode NFC and case folding. A safe name changes only in Unicode form and case.
QString memberKey(QString member)
{
    member.replace('\\', '/');
    return QDir::cleanPath(member).normalized(QString::NormalizationForm_C).toCaseFolded();
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

/// macOS Finder custom-folder icons ("Icon\r") and their AppleDouble resource forks.
bool finderIcon(const QString& member)
{
    const auto name = fileName(member);
    return name == QStringLiteral("Icon\r") || name == QStringLiteral("._Icon\r");
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

TrackerArchive TrackerArchive::open(const QString& path, const std::atomic_bool* cancelled)
{
    TrackerArchive result;
    require(result.m_Directory.isValid() && result.m_SourceDirectory.isValid());
    result.m_CanonicalRoot = QFileInfo(result.m_Directory.path()).canonicalFilePath();
    require(!result.m_CanonicalRoot.isEmpty());
    result.m_SourceDigest = snapshotArchive(path, result.sourcePath(), cancelled);
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
    // macOS builds of libarchive hide "__MACOSX/" members by default, bypassing every check below and
    // differing from the raw records the writer copies. Read the same entries on every platform.
    require(reader && archive_read_support_format_zip_seekable(reader.get()) == ARCHIVE_OK &&
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
                throw ProjectError("This Tracker archive contains a project with an unsafe member name, which MoTrack does not extract.");
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
    // Standard saves drop only skipped members that Tracker never uses: exact copies of an extracted member
    // with the same file name (the OSP JarTool absolute duplicates) and Finder folder icons.
    QSet<std::pair<QString, QByteArray>> extracted;
    for (auto member = result.m_MemberDigests.cbegin(); member != result.m_MemberDigests.cend(); ++member)
    {
        extracted.insert({fileName(member.key()), member.value()});
    }
    for (const auto& member : std::as_const(result.m_SkippedMembers))
    {
        const auto file = std::ranges::find(skippedFiles, member, &std::pair<QString, QByteArray>::first);
        if (finderIcon(member) || (file != skippedFiles.end() && extracted.contains({fileName(member), file->second})))
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
    const auto expected = m_MemberDigests.constFind(member);
    if (expected == m_MemberDigests.cend())
        return false;
    QFile input(m_Directory.filePath(member));
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
        const auto member = QDir::cleanPath(value);
        if (m_SkippedKeys.contains(memberKey(member)))
        {
            throw ProjectError("A Tracker project in this archive refers to a member with an unsafe name, which MoTrack does not extract.");
        }
        reference = !external && m_MemberDigests.contains(member) ? member.toStdString() : std::string{};
    }
}

} // namespace motrack
