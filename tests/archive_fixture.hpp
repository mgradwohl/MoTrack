#pragma once

#include <QByteArray>
#include <QFile>
#include <QString>
#include <QStringList>

#include <memory>
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
/// Names are stored exactly as given, so tests can reproduce unsafe names from real Tracker exports.
inline bool writeArchive(const QString& path, const std::vector<ArchiveEntry>& members, const char* options = "zip:compression=store")
{
    const std::unique_ptr<archive, decltype(&archive_write_free)> writer(archive_write_new(), &archive_write_free);
    QByteArray buffer(qsizetype{512} * 1024, Qt::Uninitialized);
    std::size_t used = 0;
    if (!writer || archive_write_set_format_zip(writer.get()) != ARCHIVE_OK ||
        archive_write_set_options(writer.get(), options) != ARCHIVE_OK ||
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
