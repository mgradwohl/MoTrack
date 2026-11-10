#include "workspace_input.hpp"

#include "archive_group_codec.hpp"
#include "local_media_path.hpp"
#include "tracker_archive.hpp"
#include "video_reader.hpp"
#include "workspace_document_codec.hpp"
#include "workspace_storage.hpp"

#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonDocument>

#include <algorithm>
#include <cmath>
#include <utility>

namespace motrack
{
namespace
{

void checkCancellation(const std::atomic_bool* cancelled)
{
    if (cancelled && cancelled->load())
        throw ProjectError("Workspace opening canceled.");
}

void prepareMovieMetadata(WorkspaceDocument& document, const QString& mediaProjectPath, const std::atomic_bool* cancelled)
{
    checkCancellation(cancelled);
    auto& clip = document.project.clip;
    if (!document.movieFrameTimes.empty() || clip.mediaReferences.size() != 1)
        return;
    const auto path = workspaceMediaPath(document, mediaProjectPath, clip.mediaReferences.front());
    const QFileInfo media(path);
    if (!media.isFile() || (!document.movie && (QImageReader::supportedImageFormats().contains(media.suffix().toLatin1().toLower()) ||
                                                QImageReader(path).canRead())))
        return;
    const std::atomic_bool running{false};
    auto index = buildMovieIndex(
        path, document.useDisplayOrientation ? VideoOrientation::Display : VideoOrientation::Encoded, cancelled ? *cancelled : running);
    checkCancellation(cancelled);
    if (index.frameCount <= 0)
        throw ProjectError("The project's movie could not be indexed.");
    document.movie = true;
    document.movieTimeline =
        retainMovieTimeline(path, document.useDisplayOrientation ? VideoOrientation::Display : VideoOrientation::Encoded, index);
    document.movieFrameTimes = std::move(index.frameTimes);
    // Match desktop index adoption: retain the saved clip and never shrink its source-frame extent.
    const auto steps = clip.stepCount.value_or(0);
    const auto last = static_cast<std::int64_t>(clip.firstFrame) + static_cast<std::int64_t>(steps - 1) * clip.stepSize;
    if (steps == 0 || last < index.frameCount)
        clip.sourceFrameCount = std::max(clip.sourceFrameCount.value_or(0), index.frameCount);
}

} // namespace

PreparedWorkspaceInput prepareWorkspaceInput(const QString& path, const std::atomic_bool* cancelled)
{
    checkCancellation(cancelled);
    const auto physical = QFileInfo(path).canonicalFilePath();
    if (physical.isEmpty() || !QFileInfo(physical).isFile())
        throw ProjectError("Workspace project file is unavailable.");
    PreparedWorkspaceInput result;
    result.mediaProjectPath = physical;
    WorkspaceDocument document;
    if (path.endsWith(".trz", Qt::CaseInsensitive))
    {
        const auto archive = std::make_shared<TrackerArchive>(TrackerArchive::open(physical, cancelled));
        ArchiveGroup group{.id = QUuid::createUuid(), .archive = archive, .members = {}};
        group.members.reserve(static_cast<std::size_t>(archive->projects().size()));
        result.mediaProjectPath = QDir(archive->root()).absoluteFilePath("archive.trk");
        for (const auto& member : archive->projects())
        {
            checkCancellation(cancelled);
            auto loaded = importTrackerDocument(archive->projectPath(member));
            archive->resolveMedia(loaded.project, member);
            loaded.archivePayload = archive;
            loaded.archiveMember = member.toStdString();
            loaded.alignMovieToClip = true;
            prepareMovieMetadata(loaded, result.mediaProjectPath, cancelled);
            group.members.push_back({.id = QUuid::createUuid(), .member = member.toStdString(), .document = std::move(loaded)});
        }
        checkCancellation(cancelled);
        if (group.members.empty())
            throw ProjectError("Tracker archive contains no experiments.");
        result.group = std::make_unique<ArchiveGroupSession>();
        // Validate the complete group once; incremental append revalidates every preceding member.
        result.group->replace(std::move(group), path, ArchiveGroupSource::Tracker);
        return result;
    }
    if (path.endsWith(".motrack", Qt::CaseInsensitive))
    {
        auto storage = readWorkspaceStorage(physical, cancelled);
        checkCancellation(cancelled);
        const auto metadata = QJsonDocument::fromJson(storage.json).object();
        if (metadata.value("kind") == "archive-group")
        {
            auto group = detail::decodeArchiveGroup(metadata, std::move(storage.archive), cancelled);
            const auto mediaPath = QDir(group.archive->root()).absoluteFilePath("archive.trk");
            for (auto& member : group.members)
                prepareMovieMetadata(member.document, mediaPath, cancelled);
            result.group = std::make_unique<ArchiveGroupSession>();
            result.group->replace(std::move(group), path);
            return result;
        }
        document = detail::decodeWorkspaceDocument(metadata, std::move(storage.archive));
        if (!document.archivePayload && !document.archiveReference.empty())
        {
            const auto source = resolveLocalMediaPath(physical, document.archiveReference);
            document.archivePayload = std::make_shared<TrackerArchive>(TrackerArchive::open(source, cancelled));
        }
        if (document.archivePayload)
        {
            // Native media references were already resolved against the archive root at import.
            document.archivePayload->resolveMedia(document.project, {});
            document.archiveReference.clear();
            result.mediaProjectPath = QDir(document.archivePayload->root()).absoluteFilePath("archive.trk");
        }
    }
    else if (path.endsWith(".trk", Qt::CaseInsensitive))
    {
        document = importTrackerDocument(physical);
        document.alignMovieToClip = true;
    }
    else
        throw ProjectError("Choose a Tracker or MoTrack project file.");
    checkCancellation(cancelled);
    if (!document.archivePayload)
        retainWorkspaceMediaLocations(document, physical);
    prepareMovieMetadata(document, result.mediaProjectPath, cancelled);
    result.document = std::make_unique<DocumentSession>();
    result.document->replace(std::move(document), path);
    return result;
}

PreparedWorkspaceInput prepareWorkspaceMovie(const QString& path, const std::atomic_bool* cancelled)
{
    checkCancellation(cancelled);
    const auto physical = QFileInfo(path).canonicalFilePath();
    if (physical.isEmpty() || !QFileInfo(physical).isFile())
        throw ProjectError("Movie file is unavailable.");
    const std::atomic_bool running{false};
    auto index = buildMovieIndex(physical, VideoOrientation::Display, cancelled ? *cancelled : running);
    checkCancellation(cancelled);
    if (index.frameCount <= 0)
        throw ProjectError("The movie could not be indexed.");
    WorkspaceDocument document;
    document.movie = true;
    document.useDisplayOrientation = true;
    document.movieTimeline = retainMovieTimeline(physical, VideoOrientation::Display, index);
    document.movieFrameTimes = std::move(index.frameTimes);
    document.project.clip.stepCount = index.frameCount;
    document.project.clip.sourceFrameCount = index.frameCount;
    document.project.clip.mediaReferences.push_back(QFileInfo(physical).fileName().toStdString());
    retainWorkspaceMediaLocations(document, physical);
    PreparedWorkspaceInput result;
    result.mediaProjectPath = physical;
    result.document = std::make_unique<DocumentSession>();
    result.document->replace(std::move(document), physical, DocumentSourceKind::Media);
    return result;
}

PreparedWorkspaceInput prepareWorkspaceImages(const QStringList& paths, double frameRate, const std::atomic_bool* cancelled)
{
    checkCancellation(cancelled);
    const auto duration = frameRate > 0.0 ? 1000.0 / frameRate : 0.0;
    if (paths.isEmpty() || !std::in_range<int>(paths.size()) || !std::isfinite(frameRate) || frameRate < 0.0 || !std::isfinite(duration))
        throw ProjectError("Invalid image sequence or frame rate.");
    const auto physical = QFileInfo(paths.front()).canonicalFilePath();
    if (physical.isEmpty())
        throw ProjectError("An image file is unavailable.");
    const QDir directory(localProjectDirectory(physical));
    WorkspaceDocument document;
    auto& clip = document.project.clip;
    clip.stepCount = static_cast<int>(paths.size());
    clip.sourceFrameCount = clip.stepCount;
    if (duration > 0.0)
        clip.frameDurationMs = duration;
    for (const auto& path : paths)
    {
        checkCancellation(cancelled);
        const auto image = QFileInfo(path).canonicalFilePath();
        if (image.isEmpty() || !QImageReader(image).canRead())
            throw ProjectError("An image file could not be read.");
        clip.mediaReferences.push_back(directory.relativeFilePath(image).toStdString());
    }
    if (QImageReader(physical).read().isNull())
        throw ProjectError("The first image could not be decoded.");
    checkCancellation(cancelled);
    retainWorkspaceMediaLocations(document, physical);
    PreparedWorkspaceInput result;
    result.mediaProjectPath = physical;
    result.document = std::make_unique<DocumentSession>();
    result.document->replace(std::move(document), physical, DocumentSourceKind::Media);
    return result;
}

WorkspaceSession::OpenedSource
adoptWorkspaceInput(WorkspaceSession& workspace, PreparedWorkspaceInput& incoming, WorkspaceActivation activation)
{
    if (static_cast<bool>(incoming.document) == static_cast<bool>(incoming.group))
        throw ProjectError("A prepared workspace input requires exactly one source.");
    return incoming.group ? workspace.addGroup(incoming.group, activation)
                          : workspace.addDocument(incoming.document, incoming.mediaProjectPath, activation);
}

} // namespace motrack
