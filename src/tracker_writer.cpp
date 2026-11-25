#include "tracker_writer.hpp"

#include "local_media_path.hpp"
#include "tracker_archive.hpp"
#include "tracker_archive_writer.hpp"
#include "tracker_compose.hpp"
#include "workspace_document.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QString>

#include <atomic>
#include <cstdint>
#include <exception>
#include <span>
#include <unordered_set>
#include <utility>

namespace motrack
{
namespace
{

void require(bool valid)
{
    if (!valid)
    {
        throw ProjectError("This Tracker export cannot preserve all edits or source associations.");
    }
}

std::atomic<detail::CompositionObserver> observer{nullptr};

/// The production composition (B1's until B2.4b), reported to an installed observer with its outcome.
std::string renderTrackerProjectImpl(const LegacyProject& project,
                                     const TrackerSource& source,
                                     LinkedMedia media,
                                     std::span<const double> frameTimes = {})
{
    const ExportMedia exportMedia{.kind = media, .frameTimes = frameTimes};
    const auto report = observer.load();
    if (!report)
    {
        return detail::composeLegacyExport(project, source, exportMedia);
    }
    detail::CompositionOutcome outcome;
    std::exception_ptr failure;
    try
    {
        outcome = detail::composeLegacyExport(project, source, exportMedia);
    }
    catch (const ProjectError& error)
    {
        outcome = detail::CompositionRefusal{.message = error.what()};
        failure = std::current_exception();
    }
    report(project, source, exportMedia, outcome);
    if (failure)
    {
        std::rethrow_exception(failure);
    }
    return std::get<std::string>(std::move(outcome));
}

void writeXml(const QString& path, const std::string& xml)
{
    require(path.endsWith(".trk", Qt::CaseInsensitive));
    QSaveFile output(path);
    const auto size = static_cast<qint64>(xml.size());
    if (!output.open(QIODevice::WriteOnly) || output.write(xml.data(), size) != size || !output.commit())
    {
        throw ProjectError("Could not write Tracker project: " + output.errorString().toStdString());
    }
}

void requireLocalReference(const std::string& reference)
{
    const auto local = QString::fromStdString(reference);
    require(!local.isEmpty() && local.size() <= 4096 && local.toStdString() == reference && !local.contains("!/") &&
            !local.contains("://") && !local.startsWith(':') && !local.contains("]]>"));
    for (const auto point : local.toUcs4())
        require(QChar::category(point) != QChar::Other_Control && QChar::category(point) != QChar::Other_Format);
}

} // namespace

void detail::setCompositionObserver(CompositionObserver installed) noexcept
{
    observer.store(installed);
}

detail::CompositionObserver detail::compositionObserver() noexcept
{
    return observer.load();
}

std::string renderTrackerProject(const LegacyProject& project, const TrackerSource& source)
{
    return renderTrackerProjectImpl(project, source, LinkedMedia::None);
}

void writeTrackerProject(const QString& path, const LegacyProject& project, const TrackerSource& source)
{
    writeXml(path, renderTrackerProject(project, source));
}

namespace
{
/// Renders @p document as the standard file at @p path, with its media references rebased to that file.
/// @p sourceReferences, when given, are the references exactly as the source XML spells them, in the same order:
/// the caller has checked that each still names the same media, and they are kept instead of the rebased spelling,
/// so the exported XML leaves them untouched.
std::string prepareTrackerDocument(const QString& path,
                                   const WorkspaceDocument& document,
                                   const QString& mediaProjectPath,
                                   const std::vector<std::string>* sourceReferences = nullptr)
{
    if (!document.trackerSource)
        throw ProjectError("Tracker export requires its original source.");
    if (document.trackerReadingOutdated)
        throw ProjectError("Tracker export is unavailable: an earlier MoTrack saved these measurements without the Tracker file's frame "
                           "shift or duplicate calibration keys, and they could not be updated safely. Import the Tracker file again.");
    const auto& source = *document.trackerSource;
    require(!document.useDisplayOrientation && !document.archivePayload && !document.archiveMember && document.archiveReference.empty());
    require(document.movie ? document.alignMovieToClip && !document.movieFrameTimes.empty() : document.movieFrameTimes.empty());
    auto project = document.project;
    const auto media = document.movie ? LinkedMedia::Movie : project.clip.mediaReferences.empty() ? LinkedMedia::None : LinkedMedia::Images;
    if (media != LinkedMedia::None)
    {
        require(project.clip.mediaReferences.size() <= 10000 && !mediaProjectPath.isEmpty() && QFileInfo(mediaProjectPath).isAbsolute());
        // A file symlink can be opened under two different project directories. Require a
        // regular destination; directory aliases are resolved consistently by the reader.
        require(!QFileInfo(path).isSymbolicLink());
        const auto targetParent = localProjectDirectory(path);
        require(!targetParent.isEmpty());
        const QDir targetDirectory(targetParent);
        for (auto& reference : project.clip.mediaReferences)
        {
            requireLocalReference(reference);
            const auto resolved = workspaceMediaPath(document, mediaProjectPath, reference);
            require(!resolved.isEmpty() && !sameLocalFile(resolved, targetDirectory.filePath(QFileInfo(path).fileName())));
            QFile file(resolved);
            if (!QFileInfo(resolved).isFile() || !file.open(QIODevice::ReadOnly))
                throw ProjectError("Linked media is missing or unreadable: " + resolved.toStdString());
            reference = QDir::fromNativeSeparators(targetDirectory.relativeFilePath(resolved)).toStdString();
            requireLocalReference(reference); // Rebasing can introduce a forbidden marker from a parent directory.
            // Compare file identity: a destination spelled with another case on Windows keeps its spelling (#297).
            require(sameLocalFile(resolveLocalMediaPath(path, reference), resolved));
        }
        // Native frame bounds are runtime metadata, not an edit to the preserved standard clip.
        if (project.clip.sourceFrameCount)
        {
            const auto count = *project.clip.sourceFrameCount;
            const auto clipEnd = static_cast<std::int64_t>(project.clip.firstFrame) +
                                 (static_cast<std::int64_t>(project.clip.stepCount.value_or(0)) - 1) * project.clip.stepSize + 1;
            require(count > 0 &&
                    (media == LinkedMedia::Images ? std::cmp_equal(count, project.clip.mediaReferences.size())
                                                  : std::cmp_equal(count, document.movieFrameTimes.size()) || count == clipEnd));
            project.clip.sourceFrameCount.reset();
        }
        if (sourceReferences)
        {
            require(sourceReferences->size() == project.clip.mediaReferences.size());
            project.clip.mediaReferences = *sourceReferences;
        }
    }
    return renderTrackerProjectImpl(project, source, media, document.movieFrameTimes);
}
} // namespace

void writeTrackerDocument(const QString& path, const WorkspaceDocument& document, const QString& mediaProjectPath)
{
    writeXml(path, prepareTrackerDocument(path, document, mediaProjectPath));
}

namespace
{
std::string prepareArchiveMember(const WorkspaceDocument& document, const std::atomic_bool* cancelled)
{
    require(!cancelled || !cancelled->load());
    if (!document.archivePayload || !document.archiveMember || !document.trackerSource)
        throw ProjectError("Tracker archive export requires its original archive and selected project.");
    // Tracker 6.3.5 does not extract archived AVI files for Xuggle (#140). Limit this
    // adapter to the MOV container covered by actual archive load/save/reopen acceptance.
    if (document.movie)
        require(
            document.project.clip.mediaReferences.size() == 1 &&
            QFileInfo(QString::fromStdString(document.project.clip.mediaReferences.front())).suffix().compare("mov", Qt::CaseInsensitive) ==
                0);
    require(document.archiveReference.empty());
    const auto& archive = *document.archivePayload;
    const auto member = QString::fromStdString(*document.archiveMember);
    require(member.toStdString() == *document.archiveMember);
    require(archive.memberMatchesSource(member, cancelled));
    const auto originalPath = archive.projectPath(member);
    QFile original(originalPath);
    require(original.open(QIODevice::ReadOnly) && original.size() <= static_cast<qint64>(MAX_TRACKER_XML_BYTES) &&
            original.readAll().toStdString() == document.trackerSource->xml);
    auto baseline = parseLegacyProject(document.trackerSource->xml);
    // Tracker finds an archived file only when the reference spells the member's stored name exactly: OSP looks the
    // name up among java.util.zip's entry names, which are never normalized. MoTrack's member identities are NFC
    // (#308), and a reference rebuilt from an identity can differ from the stored bytes (a decomposed name from
    // macOS, a "./" or "archive.trz!/" prefix). Archived media cannot be replaced here, so every reference is
    // written back exactly as the source spells it.
    const auto sourceReferences = baseline.clip.mediaReferences;
    archive.resolveMedia(baseline, member);
    require(baseline.clip.mediaReferences == document.project.clip.mediaReferences);
    std::unordered_set<std::string> checkedMedia;
    for (const auto& reference : document.project.clip.mediaReferences)
        if (checkedMedia.insert(reference).second && !archive.memberMatchesSource(QString::fromStdString(reference), cancelled))
            throw ProjectError("Tracker archive media changed after import; unchanged-media export cannot preserve these edits.");
    auto snapshot = document;
    snapshot.archivePayload.reset();
    snapshot.archiveMember.reset();
    // Imported archive references are validated archive-root-relative members, independent of
    // the original archive's location or the companion document's later location.
    const auto xml = prepareTrackerDocument(originalPath, snapshot, QDir(archive.root()).filePath("media-base.motrack"), &sourceReferences);
    auto exported = parseLegacyProject(xml);
    require(exported.clip.mediaReferences == sourceReferences);
    archive.resolveMedia(exported, member);
    require(exported.clip.mediaReferences == baseline.clip.mediaReferences);
    return xml;
}
} // namespace

void writeTrackerArchiveDocument(const QString& path, const WorkspaceDocument& document, const std::atomic_bool* cancelled)
{
    writeTrackerArchiveDocuments(path, std::span(&document, 1), cancelled);
}

void writeTrackerArchiveDocuments(const QString& path, std::span<const WorkspaceDocument> documents, const std::atomic_bool* cancelled)
{
    require(!cancelled || !cancelled->load());
    require(!documents.empty() && documents.front().archivePayload);
    const auto& source = *documents.front().archivePayload;
    require(documents.size() <= static_cast<std::size_t>(source.projects().size()));
    std::unordered_set<std::string> members;
    const QFileInfo target(path);
    const auto parent = QFileInfo(target.absolutePath()).canonicalFilePath();
    require(!parent.isEmpty() && !target.isSymLink());
    const auto destination = parent + '/' + target.fileName();
    for (const auto& document : documents)
    {
        require(!cancelled || !cancelled->load());
        require(document.archivePayload && document.archiveMember && document.archivePayload->sourceDigest() == source.sourceDigest() &&
                members.insert(archiveMemberKey(*document.archiveMember)).second);
        require(!QFileInfo(document.archivePayload->sourcePath()).isSymLink());
        // Independently reopened companions own separate source/extraction trees. Protect every
        // owner's paths, even though the common original bytes are read only from the first owner.
        for (const auto& owned : {document.archivePayload->root(), QFileInfo(document.archivePayload->sourcePath()).absolutePath()})
        {
            const auto root = QFileInfo(owned).canonicalFilePath();
            require(!root.isEmpty() && destination != root && !destination.startsWith(root + '/'));
        }
    }
    std::vector<std::string> rendered;
    rendered.reserve(documents.size());
    qint64 remaining = MAX_TRACKER_EXTRACTED_BYTES;
    for (const auto& document : documents)
    {
        auto xml = prepareArchiveMember(document, cancelled);
        require(xml.size() <= MAX_TRACKER_XML_BYTES && std::cmp_less_equal(xml.size(), remaining));
        remaining -= static_cast<qint64>(xml.size());
        rendered.push_back(std::move(xml));
    }
    // Create borrowed views only once their owners can no longer move (including small strings).
    std::vector<TrackerArchiveReplacement> replacements;
    replacements.reserve(documents.size());
    for (std::size_t i = 0; i < documents.size(); ++i)
    {
        const auto& member = documents[i].archiveMember;
        if (!member)
            throw ProjectError("Tracker archive export requires a selected project.");
        replacements.push_back({.member = QString::fromStdString(*member), .xml = rendered[i]});
    }
    writeTrackerArchive(path, source, replacements, cancelled);
}

} // namespace motrack
