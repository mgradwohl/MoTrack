#pragma once

#include "utf8_thread_locale.hpp"

#include <QByteArray>
#include <QFile>
#include <QString>
#include <QStringList>

#include <memory>
#include <string>
#include <vector>

#include <archive.h>
#include <archive_entry.h>

namespace motrack::test
{

struct ArchiveEntry
{
    const char* name;
    QByteArray bytes;
    bool symlink = false;
};

/// Small bounded archives, including deliberately invalid native storage, for persistence tests.
/// Names are stored exactly as given, so tests can reproduce unsafe names from real Tracker exports. They are UTF-8,
/// and a non-ASCII name gets the ZIP UTF-8 flag, as Tracker's java.util.zip writes them, whatever the C locale.
inline bool writeArchive(const QString& path, const std::vector<ArchiveEntry>& members, const char* options = "zip:compression=store")
{
    const motrack::ScopedUtf8ThreadLocale utf8Names; // as the production writers do
    const std::unique_ptr<archive, decltype(&archive_write_free)> writer(archive_write_new(), &archive_write_free);
    QByteArray buffer(qsizetype{512} * 1024, Qt::Uninitialized);
    std::size_t used = 0;
    if (!writer || archive_write_set_format_zip(writer.get()) != ARCHIVE_OK ||
        archive_write_set_options(writer.get(), (std::string(options) + ",zip:hdrcharset=UTF-8").c_str()) != ARCHIVE_OK ||
        archive_write_open_memory(writer.get(), buffer.data(), static_cast<std::size_t>(buffer.size()), &used) != ARCHIVE_OK)
    {
        return false;
    }
    for (const auto& member : members)
    {
        const std::unique_ptr<archive_entry, decltype(&archive_entry_free)> entry(archive_entry_new(), &archive_entry_free);
        if (!entry)
        {
            return false;
        }
        archive_entry_set_pathname_utf8(entry.get(), member.name);
        archive_entry_set_filetype(entry.get(), member.symlink ? AE_IFLNK : AE_IFREG);
        archive_entry_set_perm(entry.get(), 0600);
        archive_entry_set_size(entry.get(), member.symlink ? 0 : member.bytes.size());
        if (member.symlink)
        {
            archive_entry_set_symlink(entry.get(), "outside");
        }
        if (archive_write_header(writer.get(), entry.get()) != ARCHIVE_OK ||
            (!member.symlink && archive_write_data(writer.get(), member.bytes.constData(), static_cast<std::size_t>(member.bytes.size())) !=
                                    member.bytes.size()))
        {
            return false;
        }
    }
    if (archive_write_close(writer.get()) != ARCHIVE_OK)
    {
        return false;
    }
    QFile output(path);
    return output.open(QIODevice::WriteOnly) && output.write(buffer.constData(), static_cast<qint64>(used)) == static_cast<qint64>(used);
}

/// A ZIP of stored members whose names are exactly the bytes given, which libarchive's writers cannot produce: they
/// normalize every name (NFC on Windows and Linux, NFD on macOS) and decide the UTF-8 flag themselves. Records are
/// laid out as java.util.zip writes stored entries: no extra fields, no data descriptors, and general-purpose bit 11
/// (UTF-8 names) on every entry when @p utf8Flag is set.
inline QByteArray storedArchiveBytes(const std::vector<ArchiveEntry>& members, bool utf8Flag = true)
{
    const auto crc32 = [](const QByteArray& bytes)
    {
        quint32 crc = 0xffffffffU;
        for (const auto byte : bytes)
        {
            crc ^= static_cast<unsigned char>(byte);
            for (int bit = 0; bit < 8; ++bit)
                crc = (crc >> 1U) ^ ((crc & 1U) != 0 ? 0xedb88320U : 0U);
        }
        return crc ^ 0xffffffffU;
    };
    const auto u16 = [](QByteArray& out, quint32 value)
    {
        out.append(static_cast<char>(value & 0xffU));
        out.append(static_cast<char>((value >> 8U) & 0xffU));
    };
    const auto u32 = [&u16](QByteArray& out, quint32 value)
    {
        u16(out, value & 0xffffU);
        u16(out, value >> 16U);
    };
    QByteArray records;
    QByteArray directory;
    for (const auto& member : members)
    {
        const QByteArray name(member.name);
        const auto offset = static_cast<quint32>(records.size());
        // Shared by the local and central records: version needed, flags, method (stored), DOS time and date
        // (2000-01-01), CRC-32, compressed and uncompressed sizes, name length and extra length.
        QByteArray common;
        u16(common, 20);
        u16(common, utf8Flag ? 0x0800U : 0U);
        u16(common, 0);
        u16(common, 0);
        u16(common, 0x2821);
        u32(common, crc32(member.bytes));
        u32(common, static_cast<quint32>(member.bytes.size()));
        u32(common, static_cast<quint32>(member.bytes.size()));
        u16(common, static_cast<quint32>(name.size()));
        u16(common, 0);
        u32(records, 0x04034b50U);
        records += common + name + member.bytes;
        u32(directory, 0x02014b50U);
        u16(directory, 20); // version made by: MS-DOS attributes, as java.util.zip writes
        directory += common;
        u16(directory, 0); // comment length
        u16(directory, 0); // disk number
        u16(directory, 0); // internal attributes
        u32(directory, 0); // external attributes
        u32(directory, offset);
        directory += name;
    }
    QByteArray end;
    u32(end, 0x06054b50U);
    u16(end, 0);
    u16(end, 0);
    u16(end, static_cast<quint32>(members.size()));
    u16(end, static_cast<quint32>(members.size()));
    u32(end, static_cast<quint32>(directory.size()));
    u32(end, static_cast<quint32>(records.size()));
    u16(end, 0);
    return records + directory + end;
}

inline bool writeStoredArchive(const QString& path, const std::vector<ArchiveEntry>& members, bool utf8Flag = true)
{
    const auto bytes = storedArchiveBytes(members, utf8Flag);
    QFile output(path);
    return output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size();
}

/// The member names of @p zip exactly as its central directory stores them, in directory order.
inline std::vector<QByteArray> storedMemberNames(const QByteArray& zip)
{
    const auto u16 = [&zip](qsizetype offset)
    {
        return static_cast<qsizetype>(static_cast<unsigned char>(zip[offset])) |
               static_cast<qsizetype>(static_cast<unsigned char>(zip[offset + 1])) << 8;
    };
    std::vector<QByteArray> names;
    const auto end = zip.lastIndexOf(QByteArray("PK\x05\x06", 4));
    if (end < 0 || end + 22 > zip.size())
        return names;
    auto at = u16(end + 16) | u16(end + 18) << 16;
    for (qsizetype entry = 0, count = u16(end + 10); entry < count && at + 46 <= zip.size(); ++entry)
    {
        names.push_back(zip.mid(at + 46, u16(at + 28)));
        at += 46 + u16(at + 28) + u16(at + 30) + u16(at + 32);
    }
    return names;
}

/// Write names with '|' standing for '\'. libarchive's Windows writer stores '/' for '\', while other
/// ZIP tools store the backslash bytes that the reader sees on every platform.
inline bool
writeArchiveWithBackslashes(const QString& path, const std::vector<ArchiveEntry>& members, const char* options = "zip:compression=store")
{
    if (!writeArchive(path, members, options))
        return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadWrite))
        return false;
    auto bytes = file.readAll();
    for (const auto& member : members)
    {
        const QByteArray name(member.name);
        if (name.contains('|'))
            bytes.replace(name, QByteArray(name).replace('|', '\\'));
    }
    return file.seek(0) && file.write(bytes) == bytes.size();
}

/// Names that real Tracker archives contain but MoTrack never extracts: absolute members written by
/// OSP JarTool.compress in Tracker 6.1.x exports, and macOS Finder "Icon\r" files (Tracker 4.87).
/// The movie copies a safe "videos/a.mp4" holding duplicateMovieBytes(); the info page has no copy.
inline QByteArray duplicateMovieBytes()
{
    return "movie";
}

inline std::vector<ArchiveEntry> unsafeTrackerExportMembers()
{
    return {{.name = "C:/Users/x/AppData/Local/Temp/tracker1/html/experiment_info.html", .bytes = "<html/>"},
            {.name = "C:/Users/x/videos/a.mp4", .bytes = duplicateMovieBytes()},
            {.name = "html/Icon\r", .bytes = {}},
            {.name = "images/Icon\r", .bytes = {}}};
}

/// The member of unsafeTrackerExportMembers() that a standard save omits, when "videos/a.mp4" is present.
/// The info page has no copy and Finder icons hold real metadata, so saves keep both.
inline QStringList omittedTrackerExportMembers()
{
    return {"C:/Users/x/videos/a.mp4"};
}

/// A macOS AppleDouble resource fork header (RFC 1740 magic and version 2).
inline QByteArray appleDoubleBytes()
{
    return QByteArray::fromHex("00051607000200004d6163204f53582020202020");
}

} // namespace motrack::test
