#pragma once

#include <QByteArray>
#include <QFile>
#include <QString>

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
inline bool writeArchive(const QString& path, const std::vector<ArchiveEntry>& members)
{
    const std::unique_ptr<archive, decltype(&archive_write_free)> writer(archive_write_new(), &archive_write_free);
    QByteArray buffer(qsizetype{512} * 1024, Qt::Uninitialized);
    std::size_t used = 0;
    if (!writer || archive_write_set_format_zip(writer.get()) != ARCHIVE_OK ||
        archive_write_set_options(writer.get(), "zip:compression=store") != ARCHIVE_OK ||
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

} // namespace motrack::test
