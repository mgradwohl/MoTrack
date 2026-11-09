#include "local_media_path.hpp"
#include "paired_save.hpp"
#include "tracker_archive.hpp"
#include "tracker_writer.hpp"
#include "video_reader.hpp"
#include "workspace_document.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
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
constexpr qint64 MAX_EXTRACTED_BYTES = qint64{512} * 1024 * 1024;
constexpr std::size_t MAX_IMAGES = 4095; // One additional ZIP member holds the experiment.
constexpr std::size_t BLOCK_BYTES = std::size_t{64} * 1024;
constexpr qint64 MAX_IMAGE_PIXELS = qint64{32} * 1024 * 1024;

void require(bool valid)
{
    if (!valid)
        throw ProjectError(
            QCoreApplication::translate("TrackerImagePackage", "Cannot package this linked-media experiment safely.").toStdString());
}

void requireReadable(bool valid, const QString& source)
{
    if (!valid)
        throw ProjectError(QCoreApplication::translate("TrackerImagePackage", "Could not read a file required for the archive: %1")
                               .arg(source)
                               .toStdString());
}

void checkCancelled(const std::atomic_bool* cancelled)
{
    if (cancelled && *cancelled)
        throw ProjectError(QCoreApplication::translate("TrackerImagePackage", "Tracker media packaging was canceled.").toStdString());
}

void validateImage(const QString& stagedPath, const QString& source, const std::atomic_bool* cancelled)
{
    checkCancelled(cancelled);
    auto format = QFileInfo(stagedPath).suffix().toLatin1().toLower();
    if (format == "jpg")
        format = "jpeg";
    requireReadable(format == "png" || format == "jpeg" || format == "bmp", source);
    QImageReader reader(stagedPath, format);
    reader.setAutoDetectImageFormat(false);
    const auto size = reader.size();
    requireReadable(reader.canRead() && size.width() > 0 && size.height() > 0 &&
                        static_cast<qint64>(size.width()) * size.height() <= MAX_IMAGE_PIXELS,
                    source);
    // Decode one staged frame at a time. Header checks alone accept truncated/corrupt pixels.
    // The pixel bound and explicit raster handlers bound decoded storage without changing Qt's
    // process-wide allocation limit, which other workers may be using.
    const auto decoded = reader.read();
    requireReadable(!decoded.isNull() && decoded.size() == size, source);
    checkCancelled(cancelled);
}

qint64 copyFile(const QString& source, QIODevice& output, qint64 maximum, const std::atomic_bool* cancelled)
{
    QFile input(source);
    requireReadable(QFileInfo(source).isFile() && input.open(QIODevice::ReadOnly), source);
    const auto size = input.size();
    requireReadable(size > 0 && size <= maximum, source);
    std::array<char, BLOCK_BYTES> buffer{};
    qint64 copied = 0;
    for (;;)
    {
        checkCancelled(cancelled);
        const auto count = input.read(buffer.data(), static_cast<qint64>(buffer.size()));
        requireReadable(count >= 0, source);
        require(count <= size - copied);
        if (count == 0)
            break;
        require(output.write(buffer.data(), count) == count);
        copied += count;
    }
    require(copied == size);
    return copied;
}

struct ZipOutput
{
    QFile file;
    const std::atomic_bool* cancelled;
};

la_ssize_t writeBlock(archive*, void* context, const void* data, std::size_t size)
{
    auto& output = *static_cast<ZipOutput*>(context);
    if ((output.cancelled && *output.cancelled) || !std::in_range<qint64>(size) ||
        static_cast<qint64>(size) > MAX_TRACKER_ARCHIVE_BYTES - output.file.pos())
        return -1;
    return static_cast<la_ssize_t>(output.file.write(static_cast<const char*>(data), static_cast<qint64>(size)));
}

void writeMember(archive* writer, const QString& name, const QString& path, const std::atomic_bool* cancelled)
{
    QFile input(path);
    require(input.open(QIODevice::ReadOnly));
    const auto size = input.size();
    const std::unique_ptr<archive_entry, decltype(&archive_entry_free)> entry(archive_entry_new(), &archive_entry_free);
    require(static_cast<bool>(entry));
    const auto member = name.toUtf8();
    archive_entry_set_pathname_utf8(entry.get(), member.constData());
    archive_entry_set_filetype(entry.get(), AE_IFREG);
    archive_entry_set_perm(entry.get(), 0644);
    archive_entry_set_size(entry.get(), size);
    archive_entry_set_mtime(entry.get(), 315619200, 0); // 1980-01-02 UTC, inside ZIP's DOS date range in every timezone.
    require(archive_write_header(writer, entry.get()) == ARCHIVE_OK);
    std::array<char, BLOCK_BYTES> buffer{};
    qint64 copied = 0;
    for (;;)
    {
        checkCancelled(cancelled);
        const auto count = input.read(buffer.data(), static_cast<qint64>(buffer.size()));
        require(count >= 0 && count <= size - copied);
        if (count == 0)
            break;
        require(archive_write_data(writer, buffer.data(), static_cast<std::size_t>(count)) == count);
        copied += count;
    }
    require(copied == size && archive_write_finish_entry(writer) == ARCHIVE_OK);
}
} // namespace

void writeTrackerImagePackageCopy(const QString& path,
                                  const WorkspaceDocument& document,
                                  const QString& mediaProjectPath,
                                  const std::atomic_bool* cancelled)
{
    if (!path.endsWith(".trz", Qt::CaseInsensitive) || QFileInfo(path).isSymbolicLink())
        throw ProjectError("Choose a regular Tracker archive filename.");
    withSaveDestinationLock(
        path, cancelled, [&](const auto& physical) { writeTrackerImagePackage(physical, document, mediaProjectPath, cancelled); });
}

void writeTrackerMoviePackageCopy(const QString& path,
                                  const WorkspaceDocument& document,
                                  const QString& mediaProjectPath,
                                  const std::atomic_bool* cancelled)
{
    if (!path.endsWith(".trz", Qt::CaseInsensitive) || QFileInfo(path).isSymbolicLink())
        throw ProjectError("Choose a regular Tracker archive filename.");
    withSaveDestinationLock(
        path, cancelled, [&](const auto& physical) { writeTrackerMoviePackage(physical, document, mediaProjectPath, cancelled); });
}

namespace
{
void writeTrackerMediaPackage(
    const QString& path, const WorkspaceDocument& document, const QString& mediaProjectPath, bool movie, const std::atomic_bool* cancelled)
{
    checkCancelled(cancelled);
    require(path.endsWith(".trz", Qt::CaseInsensitive) && !QFileInfo(path).isSymbolicLink());
    require(document.trackerSource && !document.archivePayload && !document.archiveMember && document.archiveReference.empty() &&
            document.movie == movie && !document.useDisplayOrientation);
    require(movie ? document.alignMovieToClip && !document.movieFrameTimes.empty() && document.project.clip.mediaReferences.size() == 1
                  : document.movieFrameTimes.empty());
    require(!document.project.clip.mediaReferences.empty() && document.project.clip.mediaReferences.size() <= MAX_IMAGES &&
            QFileInfo(mediaProjectPath).isAbsolute());
    const auto directory = localProjectDirectory(path);
    require(!directory.isEmpty());
    const auto destination = QDir(directory).filePath(QFileInfo(path).fileName());
    QTemporaryDir temporary;
    require(temporary.isValid() && QDir(temporary.path()).mkdir(movie ? "media" : "images"));
    auto snapshot = document;
    QStringList members;
    qint64 remaining = MAX_EXTRACTED_BYTES;
    for (auto& reference : snapshot.project.clip.mediaReferences)
    {
        checkCancelled(cancelled);
        const auto logical = QString::fromStdString(reference);
        require(logical.toStdString() == reference && logical.size() <= 4096);
        const auto source = workspaceMediaPath(document, mediaProjectPath, reference);
        require(!source.isEmpty() && !sameLocalFile(source, destination));
        const auto suffix = QFileInfo(logical).suffix();
        require(!suffix.isEmpty() && suffix.size() <= 10);
        for (const auto character : suffix)
            require((character >= QLatin1Char('a') && character <= QLatin1Char('z')) ||
                    (character >= QLatin1Char('A') && character <= QLatin1Char('Z')) ||
                    (character >= QLatin1Char('0') && character <= QLatin1Char('9')));
        // Only MOV has actual Tracker archive extraction acceptance; never disguise other containers.
        require(!movie || suffix.compare("mov", Qt::CaseInsensitive) == 0);
        const auto member = movie ? QStringLiteral("media/movie.mov")
                                  : QStringLiteral("images/frame-%1.%2").arg(members.size(), 6, 10, QLatin1Char('0')).arg(suffix);
        QFile image(temporary.filePath(member));
        require(image.open(QIODevice::WriteOnly | QIODevice::NewOnly));
        remaining -= copyFile(source, image, remaining, cancelled);
        require(image.flush());
        members.append(member);
        reference = member.toStdString();
    }
    snapshot.mediaLocations.clear(); // Generated members now resolve inside the private package.
    const auto projectMember = QStringLiteral("experiment.trk");
    const auto projectPath = temporary.filePath(projectMember);
    writeTrackerDocument(projectPath, snapshot, projectPath);
    require(QFileInfo(projectPath).size() <= remaining);
    members.prepend(projectMember);
    const auto staged = temporary.filePath("package.trz");
    ZipOutput output{.file = QFile(staged), .cancelled = cancelled};
    require(output.file.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    std::unique_ptr<archive, decltype(&archive_write_free)> writer(archive_write_new(), &archive_write_free);
    // libarchive streams a data descriptor after each member. Java ZipInputStream (used by Tracker)
    // accepts descriptors only for DEFLATED entries, even when all uncompressed sizes are known.
    require(writer && archive_write_set_format_zip(writer.get()) == ARCHIVE_OK &&
            archive_write_set_bytes_per_block(writer.get(), 0) == ARCHIVE_OK &&
            archive_write_set_options(writer.get(), "zip:compression=deflate,zip:compression-level=1") == ARCHIVE_OK &&
            archive_write_open(writer.get(), &output, nullptr, &writeBlock, nullptr) == ARCHIVE_OK);
    for (const auto& member : members)
        writeMember(writer.get(), member, temporary.filePath(member), cancelled);
    require(archive_write_close(writer.get()) == ARCHIVE_OK);
    writer.reset();
    require(output.file.flush());
    output.file.close();
    // Validate the same bounded archive reader and media resolution used by normal project opening.
    const auto verified = TrackerArchive::open(staged, cancelled);
    require(verified.projects() == QStringList{projectMember});
    auto reopened = importTrackerDocument(verified.projectPath(projectMember));
    verified.resolveMedia(reopened.project, projectMember);
    require(reopened.project.clip.mediaReferences == snapshot.project.clip.mediaReferences);
    if (movie)
    {
        // Decode/index the immutable staged copy, including frames excluded from the clip.
        // A stale document index or truncated/replaced source must not produce a portable archive.
        const std::atomic_bool running{false};
        const auto index =
            buildMovieIndex(QDir(verified.root()).filePath(members.back()), VideoOrientation::Encoded, cancelled ? *cancelled : running);
        requireReadable(index.frameCount > 0 && document.project.clip.sourceFrameCount == index.frameCount &&
                            index.frameTimes == document.movieFrameTimes,
                        workspaceMediaPath(document, mediaProjectPath, document.project.clip.mediaReferences.front()));
    }
    else
    {
        for (std::size_t index = 0; index < snapshot.project.clip.mediaReferences.size(); ++index)
            validateImage(QDir(verified.root()).filePath(QString::fromStdString(snapshot.project.clip.mediaReferences[index])),
                          workspaceMediaPath(document, mediaProjectPath, document.project.clip.mediaReferences[index]),
                          cancelled);
    }
    checkCancelled(cancelled);
    require(!QFileInfo(destination).isSymbolicLink());
    QSaveFile finalOutput(destination);
    require(finalOutput.open(QIODevice::WriteOnly));
    static_cast<void>(copyFile(staged, finalOutput, MAX_TRACKER_ARCHIVE_BYTES, cancelled));
    checkCancelled(cancelled);
    require(finalOutput.commit());
}
} // namespace

void writeTrackerImagePackage(const QString& path,
                              const WorkspaceDocument& document,
                              const QString& mediaProjectPath,
                              const std::atomic_bool* cancelled)
{
    writeTrackerMediaPackage(path, document, mediaProjectPath, false, cancelled);
}

void writeTrackerMoviePackage(const QString& path,
                              const WorkspaceDocument& document,
                              const QString& mediaProjectPath,
                              const std::atomic_bool* cancelled)
{
    writeTrackerMediaPackage(path, document, mediaProjectPath, true, cancelled);
}
} // namespace motrack
