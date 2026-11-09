#include "workspace_storage.hpp"

#include "local_media_path.hpp"
#include "tracker_archive.hpp"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTemporaryDir>

#include <array>
#include <memory>
#include <utility>

#include <archive.h>
#include <archive_entry.h>

namespace motrack
{
namespace
{

constexpr std::size_t BLOCK_BYTES = std::size_t{64} * 1024;
constexpr qint64 MAX_BUNDLE_BYTES = MAX_WORKSPACE_JSON_BYTES + MAX_TRACKER_ARCHIVE_BYTES + qint64{8} * 1024 * 1024;

void require(bool valid)
{
    if (!valid)
    {
        throw ProjectError("Invalid or unavailable MoTrack project storage.");
    }
}

QString externalArchiveDestination(const QString& path, const TrackerArchive& source)
{
    const auto physical = physicalLocalFilePath(path);
    require(!physical.isEmpty());
    const auto parent = QFileInfo(QFileInfo(physical).absolutePath()).canonicalFilePath();
    require(!parent.isEmpty());
    for (const auto& owned : {source.root(), QFileInfo(source.sourcePath()).absolutePath()})
    {
        const auto root = QFileInfo(owned).canonicalFilePath();
        require(!root.isEmpty());
        // Compare directory identities so aliases and filesystem case rules cannot make
        // an owner-managed temporary directory appear to be a permanent save location.
        for (auto ancestor = parent;;)
        {
            require(!sameLocalFile(ancestor, root));
            const auto next = QFileInfo(ancestor).absolutePath();
            if (next == ancestor)
                break;
            ancestor = next;
        }
    }
    return physical;
}

WorkspaceStorage readBundle(const QString& path, const std::atomic_bool* cancelled)
{
    const std::unique_ptr<archive, decltype(&archive_read_free)> reader(archive_read_new(), &archive_read_free);
    require(reader && archive_read_support_format_zip_seekable(reader.get()) == ARCHIVE_OK);
#ifdef _WIN32
    require(archive_read_open_filename_w(reader.get(), QDir::toNativeSeparators(path).toStdWString().c_str(), BLOCK_BYTES) == ARCHIVE_OK);
#else
    require(archive_read_open_filename(reader.get(), QFile::encodeName(path).constData(), BLOCK_BYTES) == ARCHIVE_OK);
#endif
    QTemporaryDir payload;
    require(payload.isValid());
    const auto sourcePath = payload.filePath("source.trz");
    WorkspaceStorage result;
    bool hasJson = false;
    bool hasArchive = false;
    std::array<char, BLOCK_BYTES> buffer{};
    archive_entry* entry = nullptr;
    for (auto status = archive_read_next_header(reader.get(), &entry); status != ARCHIVE_EOF;
         status = archive_read_next_header(reader.get(), &entry))
    {
        require((!cancelled || !*cancelled) && status == ARCHIVE_OK && entry && archive_entry_filetype(entry) == AE_IFREG &&
                archive_entry_is_encrypted(entry) == 0 && !archive_entry_hardlink(entry) && !archive_entry_symlink(entry));
        const auto* name = archive_entry_pathname_utf8(entry);
        require(name != nullptr);
        const auto member = QString::fromUtf8(name);
        const bool json = member == "project.json";
        require((json && !hasJson) || (member == "tracker/source.trz" && !hasArchive));
        const auto expected = archive_entry_size(entry);
        require(expected >= 0 && expected <= (json ? MAX_WORKSPACE_JSON_BYTES : MAX_TRACKER_ARCHIVE_BYTES));
        QFile output(sourcePath);
        if (!json)
        {
            require(output.open(QIODevice::WriteOnly | QIODevice::NewOnly));
        }
        qint64 copied = 0;
        for (;;)
        {
            require(!cancelled || !*cancelled);
            const auto count = archive_read_data(reader.get(), buffer.data(), buffer.size());
            require(count >= 0 && count <= expected - copied);
            if (count == 0)
            {
                break;
            }
            if (json)
            {
                result.json.append(buffer.data(), static_cast<qsizetype>(count));
            }
            else
            {
                require(output.write(buffer.data(), count) == count);
            }
            copied += count;
        }
        require(copied == expected && (json || output.flush()));
        hasJson = hasJson || json;
        hasArchive = hasArchive || !json;
    }
    require(hasJson && hasArchive && archive_read_close(reader.get()) == ARCHIVE_OK);
    // The archive takes its own bounded snapshot before this temporary extraction is removed.
    result.archive = std::make_shared<TrackerArchive>(TrackerArchive::open(sourcePath, cancelled));
    return result;
}

la_ssize_t writeBlock(archive*, void* context, const void* data, std::size_t size)
{
    if (!std::in_range<qint64>(size))
    {
        return -1;
    }
    auto& output = *static_cast<QSaveFile*>(context);
    return static_cast<la_ssize_t>(output.write(static_cast<const char*>(data), static_cast<qint64>(size)));
}

void writeMember(archive* writer,
                 const char* name,
                 QIODevice& input,
                 qint64 size,
                 const std::atomic_bool* cancelled,
                 const std::function<void(WorkspaceWriteProgress)>& progress,
                 const QByteArray& expectedDigest = {})
{
    const std::unique_ptr<archive_entry, decltype(&archive_entry_free)> entry(archive_entry_new(), &archive_entry_free);
    require(static_cast<bool>(entry));
    archive_entry_set_pathname_utf8(entry.get(), name);
    archive_entry_set_filetype(entry.get(), AE_IFREG);
    archive_entry_set_perm(entry.get(), 0600);
    archive_entry_set_size(entry.get(), size);
    require(archive_write_header(writer, entry.get()) == ARCHIVE_OK);
    std::array<char, BLOCK_BYTES> buffer{};
    QCryptographicHash digest(QCryptographicHash::Sha256);
    qint64 copied = 0;
    for (;;)
    {
        require(!cancelled || !*cancelled);
        const auto count = input.read(buffer.data(), static_cast<qint64>(buffer.size()));
        require(count >= 0 && count <= size - copied);
        if (count == 0)
        {
            break;
        }
        require(archive_write_data(writer, buffer.data(), static_cast<std::size_t>(count)) == count);
        digest.addData(QByteArrayView(buffer.data(), count));
        copied += count;
        if (progress)
            progress(WorkspaceWriteProgress::ArchiveChunkWritten);
    }
    require(expectedDigest.isEmpty() || digest.result() == expectedDigest);
    require(copied == size && archive_write_finish_entry(writer) == ARCHIVE_OK);
}

} // namespace

WorkspaceStorage readWorkspaceStorage(const QString& path, const std::atomic_bool* cancelled)
{
    require(!cancelled || !*cancelled);
    QFile input(path);
    require(input.open(QIODevice::ReadOnly) && input.size() <= MAX_BUNDLE_BYTES);
    if (input.peek(2) == "PK")
    {
        input.close();
        return readBundle(path, cancelled);
    }
    require(input.size() <= MAX_WORKSPACE_JSON_BYTES);
    auto json = input.read(MAX_WORKSPACE_JSON_BYTES + 1);
    require(input.error() == QFileDevice::NoError && json.size() <= MAX_WORKSPACE_JSON_BYTES && (!cancelled || !*cancelled));
    return {.json = std::move(json), .archive = {}};
}

void writeWorkspaceStorage(const QString& path,
                           const QByteArray& json,
                           const std::shared_ptr<const TrackerArchive>& source,
                           const std::atomic_bool* cancelled,
                           const std::function<void(WorkspaceWriteProgress)>& progress)
{
    require((!cancelled || !*cancelled) && json.size() <= MAX_WORKSPACE_JSON_BYTES);
    QSaveFile output(source ? externalArchiveDestination(path, *source) : path);
    if (!source)
    {
        require(output.open(QIODevice::WriteOnly) && output.write(json) == json.size());
        if (progress)
            progress(WorkspaceWriteProgress::ReadyToCommit);
        require((!cancelled || !*cancelled) && output.commit());
        return;
    }
    require(!QFileInfo(source->sourcePath()).isSymLink());
    QFile payload(source->sourcePath());
    require(payload.open(QIODevice::ReadOnly) && payload.size() > 0 && payload.size() <= MAX_TRACKER_ARCHIVE_BYTES);
    const auto payloadSize = payload.size();
    QBuffer metadata;
    metadata.setData(json);
    require(metadata.open(QIODevice::ReadOnly) && output.open(QIODevice::WriteOnly));
    std::unique_ptr<archive, decltype(&archive_write_free)> writer(archive_write_new(), &archive_write_free);
    require(writer && archive_write_set_format_zip(writer.get()) == ARCHIVE_OK &&
            archive_write_set_options(writer.get(), "zip:compression=store") == ARCHIVE_OK &&
            archive_write_open(writer.get(), &output, nullptr, &writeBlock, nullptr) == ARCHIVE_OK);
    writeMember(writer.get(), "project.json", metadata, json.size(), cancelled, progress);
    writeMember(writer.get(), "tracker/source.trz", payload, payloadSize, cancelled, progress, source->sourceDigest());
    require(archive_write_close(writer.get()) == ARCHIVE_OK);
    writer.reset();
    if (progress)
        progress(WorkspaceWriteProgress::ReadyToCommit);
    require((!cancelled || !*cancelled) && output.commit());
}

} // namespace motrack
