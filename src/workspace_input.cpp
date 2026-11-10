#include "workspace_input.hpp"

#include "archive_group_codec.hpp"
#include "local_media_path.hpp"
#include "tracker_archive.hpp"
#include "workspace_document_codec.hpp"
#include "workspace_storage.hpp"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>

#include <optional>
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
        std::optional<ArchiveGroup> group;
        for (const auto& member : archive->projects())
        {
            checkCancellation(cancelled);
            auto loaded = importTrackerDocument(archive->projectPath(member));
            archive->resolveMedia(loaded.project, member);
            loaded.archivePayload = archive;
            loaded.archiveMember = member.toStdString();
            loaded.alignMovieToClip = true;
            if (group)
                (void)appendArchiveGroupDocument(*group, std::move(loaded));
            else
                group = makeArchiveGroup(std::move(loaded));
        }
        checkCancellation(cancelled);
        if (!group)
            throw ProjectError("Tracker archive contains no experiments.");
        result.group = std::make_unique<ArchiveGroupSession>();
        result.group->replace(std::move(*group), path, ArchiveGroupSource::Tracker);
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
    result.document = std::make_unique<DocumentSession>();
    result.document->replace(std::move(document), path);
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
