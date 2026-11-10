#include "archive_group.hpp"
#include "archive_group_session.hpp"
#include "legacy_project.hpp"
#include "measurements.hpp"
#include "media_vectors.hpp"
#include "movie_authoring.hpp"
#include "paired_save.hpp"
#include "track_edits.hpp"
#include "tracker_archive.hpp"
#include "tracker_references.hpp"
#include "tracker_writer.hpp"
#include "video_reader.hpp"
#include "workspace_document.hpp"
#include "workspace_session.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStringList>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>

namespace
{

QJsonObject referenceSnapshot(const motrack::ImportedTrackerProject& imported)
{
    const auto index = motrack::indexTrackerReferences(imported.source, imported.project);
    QJsonArray tracks;
    for (const auto& track : index.tracks)
    {
        tracks.append(QJsonObject{{"source_index", static_cast<qint64>(track.sourceIndex)},
                                  {"class", QString::fromStdString(track.className)},
                                  {"name", track.originalName ? QJsonValue(QString::fromStdString(*track.originalName)) : QJsonValue::Null},
                                  {"native_id", track.nativeId ? QJsonValue(QString::number(*track.nativeId)) : QJsonValue::Null}});
    }
    QJsonArray references;
    const QStringList kinds{"selected_track",
                            "reference_frame",
                            "plot",
                            "table",
                            "table_column_owner",
                            "table_format_owner",
                            "plot_owner",
                            "center_of_mass_member",
                            "vector_sum_member",
                            "plot_guest",
                            "table_dataset_owner",
                            "attachment",
                            "circle_step_attachment",
                            "model_booster",
                            "dynamic_system_member"};
    const QStringList states{"resolved", "missing", "ambiguous", "no_target", "unclassified"};
    for (const auto& reference : index.references)
    {
        QJsonArray targets;
        for (const auto target : reference.candidateSourceIndices)
        {
            targets.append(static_cast<qint64>(target));
        }
        references.append(QJsonObject{{"kind", kinds.at(static_cast<int>(reference.kind))},
                                      {"path", QString::fromStdString(reference.path)},
                                      {"name", QString::fromStdString(reference.originalName)},
                                      {"resolution", states.at(static_cast<int>(reference.resolution))},
                                      {"targets", targets}});
    }
    QJsonArray malformed;
    for (const auto& path : index.malformedPaths)
    {
        malformed.append(QString::fromStdString(path));
    }
    return {{"tracks", tracks}, {"references", references}, {"malformed_paths", malformed}};
}

QJsonValue world(const motrack::LegacyProject& project, int frame, double x, double y)
{
    if (project.calibration)
    {
        if (const auto point = motrack::worldPosition(*project.calibration, {.frame = frame, .x = x, .y = y}))
        {
            return QJsonArray{point->x, point->y};
        }
    }
    return QJsonValue::Null;
}

QJsonObject snapshot(const motrack::LegacyProject& project)
{
    QJsonArray masses;
    for (const auto& track : project.pointMasses)
    {
        QJsonArray samples;
        for (const auto& mark : track.marks)
        {
            samples.append(QJsonObject{
                {"frame", mark.frame}, {"image", QJsonArray{mark.x, mark.y}}, {"world", world(project, mark.frame, mark.x, mark.y)}});
        }
        masses.append(QJsonObject{{"name", QString::fromStdString(track.name)}, {"mass", track.mass}, {"samples", samples}});
    }
    QJsonArray vectors;
    for (const auto& track : project.vectors)
    {
        QJsonArray samples;
        for (const auto& sample : track.samples)
        {
            samples.append(QJsonObject{{"frame", sample.frame},
                                       {"tail", QJsonArray{sample.tailX, sample.tailY}},
                                       {"components", QJsonArray{sample.componentX, sample.componentY}}});
        }
        vectors.append(QJsonObject{{"name", QString::fromStdString(track.name)}, {"samples", samples}});
    }
    QJsonArray unsupported;
    for (const auto& type : project.unsupportedTrackTypes)
    {
        unsupported.append(QString::fromStdString(type));
    }
    const auto& clip = project.clip;
    const auto count = clip.stepCount.value_or(0);
    const auto last = count == 0 ? std::int64_t{clip.firstFrame} : clip.firstFrame + std::int64_t{clip.stepSize} * (count - 1);
    if (last > std::numeric_limits<int>::max())
    {
        throw motrack::ProjectError("Corpus clip frame overflow.");
    }
    QJsonArray coordinates;
    for (const auto frame : {clip.firstFrame, static_cast<int>(last)})
    {
        coordinates.append(
            QJsonObject{{"frame", frame}, {"image", QJsonArray{13.5, 17.25}}, {"world", world(project, frame, 13.5, 17.25)}});
    }
    return {{"source_version", QString::fromStdString(project.version)},
            {"length_unit", QString::fromStdString(project.lengthUnit)},
            {"mass_unit", QString::fromStdString(project.massUnit)},
            {"time_unit", QString::fromStdString(project.timeUnit)},
            {"clip",
             QJsonObject{{"first_frame", clip.firstFrame},
                         {"step_size", clip.stepSize},
                         {"step_count", clip.stepCount ? QJsonValue(*clip.stepCount) : QJsonValue::Null},
                         {"start_ms", clip.startTimeMs}}},
            {"point_masses", masses},
            {"vectors", vectors},
            {"unsupported_tracks", unsupported},
            {"coordinate_samples", coordinates}};
}

void saveAndRecoverPair(const QString& path, const motrack::WorkspaceDocument& document, const QString& mediaPath)
{
    std::atomic_bool cancelled{false};
    const auto partial =
        motrack::writePairedDocuments(path,
                                      document,
                                      mediaPath,
                                      &cancelled,
                                      [&cancelled](auto state) { cancelled = state == motrack::PairedSaveProgress::StandardSaved; });
    if (partial.status != motrack::PairedSaveStatus::RecoveryRequired || !partial.standardCurrent || partial.nativeCurrent ||
        partial.recoveryPath.isEmpty())
        throw motrack::ProjectError("Paired corpus save did not retain the interrupted standard/native pair.");
    const auto recovered = motrack::recoverPairedDocuments(partial.recoveryPath);
    if (recovered.status != motrack::PairedSaveStatus::Saved || !recovered.standardCurrent || !recovered.nativeCurrent ||
        QFileInfo::exists(partial.recoveryPath))
        throw motrack::ProjectError("Paired corpus recovery did not complete both outputs.");
    auto expected = document;
    motrack::rebaseWorkspaceMedia(expected, mediaPath, recovered.nativePath);
    const auto reopened = motrack::readWorkspaceDocument(recovered.nativePath);
    if (reopened.project != expected.project || reopened.archiveMember != expected.archiveMember || !reopened.trackerSource ||
        !expected.trackerSource || reopened.trackerSource->xml != expected.trackerSource->xml || reopened.movie != expected.movie ||
        reopened.movieFrameTimes != expected.movieFrameTimes || reopened.alignMovieToClip != expected.alignMovieToClip ||
        reopened.useDisplayOrientation != expected.useDisplayOrientation)
        throw motrack::ProjectError("Recovered native companion changed the edited project or original source.");
}

void editMovieCalibration(motrack::WorkspaceDocument& document, motrack::TrackEdits& edits)
{
    const auto before = document.project;
    if (!edits.setCoordinateAxes(document.project, {.frame = 1, .originX = 10, .originY = 18, .positiveX = 10, .positiveY = 10}) ||
        !edits.calibrateDistance(document.project, {.frame = 2, .imageDeltaX = 8, .imageDeltaY = 0, .knownLength = 2, .unit = "cm"}))
        throw motrack::ProjectError("Could not calibrate the movie reference.");
    const auto edited = document.project;
    if (!edits.undo(document.project) || !edits.undo(document.project) || document.project != before || !edits.redo(document.project) ||
        !edits.redo(document.project) || document.project != edited)
        throw motrack::ProjectError("Movie calibration history did not preserve the experiment.");
}

void editKeyedMovieCalibration(motrack::WorkspaceDocument& document, motrack::TrackEdits& edits)
{
    const auto before = document.project;
    if (!edits.calibrateDistance(document.project, {.frame = 2, .imageDeltaX = 8, .imageDeltaY = 0, .knownLength = 1, .unit = "cm"}) ||
        !edits.setCoordinateAxes(document.project, {.frame = 2, .originX = 12, .originY = 16, .positiveX = 20, .positiveY = 16}) ||
        !edits.setCoordinateAxes(document.project, {.frame = 1, .originX = 6, .originY = 14, .positiveX = 6, .positiveY = 22}))
        throw motrack::ProjectError("Could not edit keyed movie calibration.");
    const auto edited = document.project;
    for (int index = 0; index < 3; ++index)
        if (!edits.undo(document.project))
            throw motrack::ProjectError("Could not undo keyed movie calibration.");
    if (document.project != before)
        throw motrack::ProjectError("Undo changed the original keyed movie project.");
    for (int index = 0; index < 3; ++index)
        if (!edits.redo(document.project))
            throw motrack::ProjectError("Could not redo keyed movie calibration.");
    if (document.project != edited)
        throw motrack::ProjectError("Redo changed the edited keyed movie project.");
}

} // namespace

int main(int argc, char** argv)
try
{
    const QCoreApplication application(argc, argv);
    const auto args = application.arguments();
    if (args.size() == 4 && args[1] == "--interrupt-paired-save")
    {
        // Run only as a short-lived test child: simulate abrupt process loss without destructors.
        // The input is standalone, so the recovery files are the only owned temporary artifacts.
        const auto document = motrack::importTrackerDocument(args[2]);
        const auto result = motrack::writePairedDocuments(args[3],
                                                          document,
                                                          args[2],
                                                          nullptr,
                                                          [](auto state)
                                                          {
                                                              if (state == motrack::PairedSaveProgress::StandardSaved)
                                                                  std::_Exit(0);
                                                          });
        throw motrack::ProjectError("Interruption probe did not reach the first commit: " + result.diagnostic.toStdString());
    }
    if (args.size() == 4 && args[1].startsWith("--export-media-vectors-"))
    {
        const auto mode = args[1].sliced(QStringLiteral("--export-media-vectors-").size());
        const auto kind = mode.section('-', 0, 0);
        const auto route = mode.section('-', 1, 1);
        const auto scenario = mode.section('-', 2);
        const bool movie = kind == "movie";
        const bool archive = route == "archive" || route == "paired";
        const bool packaged = route == "packaged";
        if ((kind != "movie" && kind != "images") || (route != "linked" && !archive && !packaged))
            throw motrack::ProjectError("Unknown media vector route.");
        QTemporaryDir temporary;
        const auto source = temporary.filePath(archive ? "input.trz" : "input.trk");
        const auto native = temporary.filePath("saved.motrack");
        const QDir originalDirectory(QFileInfo(args[2]).absolutePath());
        const auto mediaDirectory = QDir(QFileInfo(args[3]).absolutePath()).filePath(QString::fromUtf8("media é & vectors"));
        if (!temporary.isValid() || !QFile::copy(args[2], source))
            throw motrack::ProjectError("Could not prepare media vector round trip.");
        motrack::WorkspaceDocument document;
        std::shared_ptr<motrack::TrackerArchive> payload;
        QString moviePath;
        QStringList copiedMedia;
        if (archive)
        {
            payload = std::make_shared<motrack::TrackerArchive>(motrack::TrackerArchive::open(source));
            if (payload->projects() != QStringList{"experiment.trk", "second.trk"})
                throw motrack::ProjectError("Expected both vector archive experiments.");
            document = motrack::importTrackerDocument(payload->projectPath("experiment.trk"));
            payload->resolveMedia(document.project, "experiment.trk");
            document.archivePayload = payload;
            document.archiveMember = "experiment.trk";
            moviePath = QDir(payload->root()).filePath("constant-rate.mov");
        }
        else
        {
            document = motrack::importTrackerDocument(source);
            if (!QDir().mkdir(mediaDirectory))
                throw motrack::ProjectError("Could not prepare vector media relocation.");
            for (auto& reference : document.project.clip.mediaReferences)
            {
                const auto original = originalDirectory.filePath(QString::fromStdString(reference));
                const auto copy = QDir(mediaDirectory).filePath(QFileInfo(original).fileName());
                if (!QFile::copy(original, copy))
                    throw motrack::ProjectError("Could not relocate vector media.");
                copiedMedia.append(copy);
                reference = QDir(temporary.path()).relativeFilePath(copy).toStdString();
            }
            moviePath = QDir(mediaDirectory).filePath("constant-rate.mov");
        }
        if (movie)
        {
            const std::atomic_bool cancelled{false};
            const auto index = motrack::buildMovieIndex(moviePath, motrack::VideoOrientation::Encoded, cancelled);
            if (index.frameCount != 4 || index.frameTimes != std::vector<double>{0.0, 0.125, 0.25, 0.375})
                throw motrack::ProjectError("Media vector movie timing differs from the independent reference.");
            document.movie = true;
            document.alignMovieToClip = true;
            document.movieFrameTimes = index.frameTimes;
            document.project.clip.sourceFrameCount = index.frameCount;
        }
        motrack::TrackEdits edits;
        editMediaVectors(document.project, edits, scenario);
        motrack::writeWorkspaceDocument(native, document);
        const auto expectedProject = document.project;
        document = {};
        payload.reset();
        if (!QFile::remove(source))
            throw motrack::ProjectError("Could not remove original media vector project.");
        document = motrack::readWorkspaceDocument(native);
        if (document.project != expectedProject)
            throw motrack::ProjectError("Native reopen changed the edited media vectors.");
        if (route == "paired")
            saveAndRecoverPair(args[3], document, native);
        else if (archive)
            motrack::writeTrackerArchiveDocument(args[3], document);
        else if (packaged)
        {
            if (movie)
                motrack::writeTrackerMoviePackage(args[3], document, native);
            else
                motrack::writeTrackerImagePackage(args[3], document, native);
            for (const auto& path : copiedMedia)
                if (!QFile::remove(path))
                    throw motrack::ProjectError("Could not remove external packaged vector media.");
            if (!QDir().rmdir(mediaDirectory))
                throw motrack::ProjectError("Could not remove vector media directory.");
        }
        else
            motrack::writeTrackerDocument(args[3], document, native);
        return 0;
    }
    if (args.size() == 4 && (args[1] == "--export-archive-group-forward" || args[1] == "--export-archive-group-reverse"))
    {
        QTemporaryDir temporary;
        const auto source = temporary.filePath("original.trz");
        const auto relocated = temporary.filePath(QString::fromUtf8("réouverture"));
        if (!temporary.isValid() || !QDir().mkdir(relocated) || !QFile::copy(args[2], source))
            throw motrack::ProjectError("Could not prepare the multi-member archive round trip.");
        auto payload = std::make_shared<motrack::TrackerArchive>(motrack::TrackerArchive::open(source));
        const QStringList members{"experiment.trk", "second.trk"};
        if (payload->projects() != members)
            throw motrack::ProjectError("Expected both independently pinned archive experiments.");
        std::vector<motrack::LegacyProject> expected;
        std::optional<motrack::ArchiveGroup> nativeGroup;
        for (int i = 0; i < members.size(); ++i)
        {
            auto document = motrack::importTrackerDocument(payload->projectPath(members[i]));
            payload->resolveMedia(document.project, members[i]);
            document.archivePayload = payload;
            document.archiveMember = members[i].toStdString();
            if (document.project.clip.mediaReferences == std::vector<std::string>{"constant-rate.mov"})
            {
                const std::atomic_bool cancelled{false};
                const auto index = motrack::buildMovieIndex(
                    QDir(payload->root()).filePath("constant-rate.mov"), motrack::VideoOrientation::Encoded, cancelled);
                if (index.frameCount != 4 || index.frameTimes != std::vector<double>{0.0, 0.125, 0.25, 0.375})
                    throw motrack::ProjectError("Multi-member movie timing differs from the independent reference.");
                document.movie = true;
                document.alignMovieToClip = true;
                document.movieFrameTimes = index.frameTimes;
                document.project.clip.sourceFrameCount = index.frameCount;
            }
            motrack::TrackEdits edits;
            if (!edits.setPointMassValue(document.project, 0, i == 0 ? 5.0 : 7.0) ||
                !edits.markPointMass(document.project, 0, {.frame = 1, .x = 8.0 + i, .y = 12.0 - i}) || !edits.undo(document.project) ||
                !edits.redo(document.project))
                throw motrack::ProjectError("Could not edit both archived experiments.");
            expected.push_back(document.project);
            if (nativeGroup)
                (void)motrack::appendArchiveGroupDocument(*nativeGroup, document);
            else
                nativeGroup = motrack::makeArchiveGroup(document);
            const auto native = temporary.filePath(QString("saved-%1.motrack").arg(i));
            motrack::writeWorkspaceDocument(native, document);
            if (!QFile::copy(native, QDir(relocated).filePath(QFileInfo(native).fileName())) || !QFile::remove(native))
                throw motrack::ProjectError("Could not relocate the native archive companions.");
        }
        if (!nativeGroup)
            throw motrack::ProjectError("Expected a shared native archive group.");
        const auto groupId = nativeGroup->id;
        const auto originalDigest = nativeGroup->archive->sourceDigest();
        std::vector<QUuid> documentIds;
        documentIds.reserve(nativeGroup->members.size());
        for (const auto& member : nativeGroup->members)
            documentIds.push_back(member.id);
        const auto originalGroupPath = temporary.filePath("saved-group.motrack");
        const auto relocatedGroupPath = QDir(relocated).filePath("shared-group.motrack");
        motrack::writeArchiveGroup(originalGroupPath, *nativeGroup);
        if (!QFile::copy(originalGroupPath, relocatedGroupPath) || !QFile::remove(originalGroupPath))
            throw motrack::ProjectError("Could not relocate the shared native archive group.");
        nativeGroup.reset();
        payload.reset();
        if (!QFile::remove(source))
            throw motrack::ProjectError("Could not remove the original multi-member archive.");
        std::vector<motrack::WorkspaceDocument> documents;
        for (std::size_t i = 0; i < expected.size(); ++i)
        {
            auto document = motrack::readWorkspaceDocument(QDir(relocated).filePath(QString("saved-%1.motrack").arg(i)));
            if (document.project != expected[i])
                throw motrack::ProjectError("Native reopen changed an edited archive experiment.");
            documents.push_back(std::move(document));
        }
        if (args[1].endsWith("-reverse"))
            std::ranges::reverse(documents);
        motrack::writeTrackerArchiveDocuments(args[3], documents);
        QFile separateExport(args[3]);
        if (!separateExport.open(QIODevice::ReadOnly) || separateExport.size() > motrack::MAX_TRACKER_ARCHIVE_BYTES)
            throw motrack::ProjectError("Could not check the independent-companion export.");
        const auto separateBytes = separateExport.readAll();
        if (separateExport.error() != QFileDevice::NoError)
            throw motrack::ProjectError("Could not read the independent-companion export.");
        separateExport.close();
        documents.clear();
        for (std::size_t i = 0; i < expected.size(); ++i)
            if (!QFile::remove(QDir(relocated).filePath(QString("saved-%1.motrack").arg(i))))
                throw motrack::ProjectError("Could not remove the independent native companions.");
        auto reopened = motrack::readArchiveGroup(relocatedGroupPath);
        if (reopened.id != groupId || reopened.archive->sourceDigest() != originalDigest || reopened.members.size() != expected.size())
            throw motrack::ProjectError("Native group reopen changed its identity or immutable source.");
        for (std::size_t i = 0; i < expected.size(); ++i)
        {
            const auto& member = reopened.members[i];
            if (member.id != documentIds[i] || member.member != members[static_cast<qsizetype>(i)].toStdString() ||
                member.document.project != expected[i] || member.document.archivePayload != reopened.archive)
                throw motrack::ProjectError("Native group reopen changed an experiment or its shared ownership.");
            documents.push_back(member.document);
        }
        if (args[1].endsWith("-reverse"))
            std::ranges::reverse(documents);
        motrack::writeTrackerArchiveDocuments(args[3], documents);
        QFile groupedExport(args[3]);
        if (!groupedExport.open(QIODevice::ReadOnly) || groupedExport.readAll() != separateBytes ||
            groupedExport.error() != QFileDevice::NoError)
            throw motrack::ProjectError("Shared native persistence changed the independently verified archive export.");
        groupedExport.close();
        if (args[1].endsWith("-reverse"))
            std::ranges::reverse(reopened.members);
        std::atomic_bool cancelled{false};
        const auto partial = motrack::writePairedArchiveGroup(
            args[3], reopened, &cancelled, [&](auto state) { cancelled = state == motrack::PairedSaveProgress::StandardSaved; });
        if (partial.status != motrack::PairedSaveStatus::RecoveryRequired || !partial.standardCurrent || partial.nativeCurrent)
            throw motrack::ProjectError("Expected an interrupted shared-group pair after standard saving.");
        documents.clear();
        reopened = {};
        if (!QFile::remove(relocatedGroupPath))
            throw motrack::ProjectError("Could not remove the previous shared native group.");
        const auto recovered = motrack::recoverPairedDocuments(partial.recoveryPath, nullptr, {}, partial.transactionId);
        if (recovered.status != motrack::PairedSaveStatus::Saved || !recovered.standardCurrent || !recovered.nativeCurrent)
            throw motrack::ProjectError("Could not recover the complete shared archive group.");
        const auto recoveredGroup = motrack::readArchiveGroup(recovered.nativePath);
        if (recoveredGroup.id != groupId || recoveredGroup.archive->sourceDigest() != originalDigest ||
            recoveredGroup.members.size() != expected.size())
            throw motrack::ProjectError("Shared-group recovery changed its identity or immutable source.");
        for (std::size_t i = 0; i < recoveredGroup.members.size(); ++i)
        {
            const auto index = args[1].endsWith("-reverse") ? expected.size() - 1 - i : i;
            const auto& member = recoveredGroup.members[i];
            if (member.id != documentIds[index] || member.member != members[static_cast<qsizetype>(index)].toStdString() ||
                member.document.project != expected[index] || member.document.archivePayload != recoveredGroup.archive)
                throw motrack::ProjectError("Shared-group recovery changed an experiment or its owner.");
        }
        if (!groupedExport.open(QIODevice::ReadOnly) || groupedExport.readAll() != separateBytes ||
            groupedExport.error() != QFileDevice::NoError)
            throw motrack::ProjectError("Shared paired recovery changed the independently verified archive export.");
        groupedExport.close();
        auto liveGroup = std::make_unique<motrack::ArchiveGroupSession>();
        liveGroup->replace(recoveredGroup, recovered.nativePath);
        motrack::WorkspaceSession live;
        const auto opened = live.addGroup(liveGroup);
        const auto liveId = opened.documents.front();
        const auto otherId = opened.documents.back();
        const auto tracking = live.beginJob(liveId, motrack::WorkspaceJob::AutoTrack);
        live.showDocument(otherId);
        live.hideDocument(otherId);
        live.cancelJobs(otherId);
        if (!live.accepts(tracking))
            throw motrack::ProjectError("Switching or closing another experiment invalidated the first experiment's job.");
        const auto staleRequest = live.beginSave(liveId);
        if (!staleRequest)
            throw motrack::ProjectError("Could not capture the workspace group save.");
        if (!live.history(liveId).setPointMassValue(live.project(liveId), 0, live.project(liveId).pointMasses.front().mass + 1.0))
            throw motrack::ProjectError("Could not prepare an edit while the group snapshot was pending.");
        const auto staleSaved = motrack::writeWorkspacePair(*staleRequest, args[3]);
        if (live.completePairedSave(*staleRequest, staleSaved) != motrack::SaveOutcome::Stale || !live.isModified(opened.source) ||
            !live.history(liveId).undo(live.project(liveId)) || !live.isModified(opened.source))
            throw motrack::ProjectError("A stale group completion cleared a newer edit or its invalidated checkpoint.");
        const auto liveRequest = live.beginSave(otherId);
        if (!liveRequest)
            throw motrack::ProjectError("Could not capture the hidden experiment's complete group save.");
        const auto livePartial = motrack::writeWorkspacePair(
            *liveRequest,
            args[3],
            [&](auto state) { liveRequest->operation.cancelled->store(state == motrack::PairedSaveProgress::StandardSaved); });
        if (live.completePairedSave(*liveRequest, livePartial) != motrack::SaveOutcome::Partial || !live.isModified(opened.source))
            throw motrack::ProjectError("An interrupted live group was incorrectly marked saved.");
        const auto recoveryRequest = live.beginRecovery(liveId);
        if (!recoveryRequest || recoveryRequest->transaction != livePartial.transactionId)
            throw motrack::ProjectError("Workspace recovery did not capture the interrupted transaction.");
        const auto liveRecovery = motrack::recoverWorkspacePair(*recoveryRequest);
        if (live.completeRecovery(*recoveryRequest, liveRecovery) != motrack::SaveOutcome::Recovered || !live.isModified(opened.source) ||
            !live.recoveryPath(opened.source).isEmpty())
            throw motrack::ProjectError("Group recovery did not preserve the requirement for a fresh save.");
        const auto finalRequest = live.beginSave(otherId);
        if (!finalRequest)
            throw motrack::ProjectError("Could not capture the final workspace save.");
        const auto finalSaved = motrack::writeWorkspacePair(*finalRequest, args[3]);
        if (live.completePairedSave(*finalRequest, finalSaved) != motrack::SaveOutcome::Saved || live.isModified(opened.source) ||
            !live.accepts(tracking))
            throw motrack::ProjectError("Could not save the current complete group after recovery.");
        const auto finalGroup = motrack::readArchiveGroup(finalSaved.nativePath);
        if (finalGroup.id != recoveredGroup.id || finalGroup.members.size() != recoveredGroup.members.size())
            throw motrack::ProjectError("Live group coordination changed persisted membership.");
        for (std::size_t i = 0; i < finalGroup.members.size(); ++i)
            if (finalGroup.members[i].id != recoveredGroup.members[i].id ||
                finalGroup.members[i].document.project != recoveredGroup.members[i].document.project)
                throw motrack::ProjectError("Live group coordination changed an independently checked experiment.");
        if (!groupedExport.open(QIODevice::ReadOnly) || groupedExport.readAll() != separateBytes ||
            groupedExport.error() != QFileDevice::NoError)
            throw motrack::ProjectError("Live group saving changed the independently verified archive export.");
        return 0;
    }
    if (args.size() == 4 &&
        (args[1] == "--export-archive-images" || args[1] == "--export-paired-archive-images" ||
         args[1] == "--export-multitab-archive-images" || args[1] == "--export-archive-image-clip" || args[1] == "--export-archive-movie" ||
         args[1] == "--export-archive-movie-clip" || args[1] == "--export-archive-movie-clip-single" ||
         args[1] == "--export-paired-archive-movie" || args[1] == "--export-archive-movie-calibration" ||
         args[1] == "--export-paired-archive-movie-calibration" || args[1] == "--export-archive-movie-keyed-calibration" ||
         args[1] == "--export-paired-archive-movie-keyed-calibration" || args[1].startsWith("--export-archive-movie-tracks-") ||
         args[1].startsWith("--export-paired-archive-movie-tracks-")))
    {
        const bool movie = args[1].contains("movie");
        QTemporaryDir temporary;
        const auto source = temporary.filePath("input.trz");
        const auto native = temporary.filePath("saved.motrack");
        if (!temporary.isValid() || !QFile::copy(args[2], source))
            throw motrack::ProjectError("Could not prepare archive round trip.");
        auto payload = std::make_shared<motrack::TrackerArchive>(motrack::TrackerArchive::open(source));
        const auto expected = movie || args[1] == "--export-multitab-archive-images" ? QStringList{"experiment.trk", "second.trk"}
                                                                                     : QStringList{"experiment.trk"};
        if (payload->projects() != expected)
            throw motrack::ProjectError("Expected the pinned archive experiment.");
        auto document = motrack::importTrackerDocument(payload->projectPath("experiment.trk"));
        payload->resolveMedia(document.project, "experiment.trk");
        document.archivePayload = payload;
        document.archiveMember = "experiment.trk";
        if (movie)
        {
            if (document.project.clip.mediaReferences != std::vector<std::string>{"constant-rate.mov"})
                throw motrack::ProjectError("Expected the pinned archived movie.");
            const std::atomic_bool cancelled{false};
            const auto index = motrack::buildMovieIndex(
                QDir(payload->root()).filePath("constant-rate.mov"), motrack::VideoOrientation::Encoded, cancelled);
            if (index.frameCount != 4 || index.frameTimes != std::vector<double>{0.0, 0.125, 0.25, 0.375})
                throw motrack::ProjectError("The archived movie index differs from the independent timing values.");
            document.movie = true;
            document.alignMovieToClip = true;
            document.movieFrameTimes = index.frameTimes;
            document.project.clip.sourceFrameCount = index.frameCount;
        }
        motrack::TrackEdits edits;
        if (!edits.setPointMassValue(document.project, 0, 5.0) ||
            !edits.markPointMass(document.project, 0, {.frame = 1, .x = 8.0, .y = 12.0}) || !edits.undo(document.project) ||
            !edits.redo(document.project))
            throw motrack::ProjectError("Could not edit archived media reference.");
        if (args[1].endsWith("-movie-calibration"))
            editMovieCalibration(document, edits);
        if (args[1].endsWith("-movie-keyed-calibration"))
            editKeyedMovieCalibration(document, edits);
        if (args[1].contains("-movie-tracks-"))
            editMovieTracks(document.project, edits, args[1].section("-movie-tracks-", 1, 1));
        if (args[1] == "--export-archive-image-clip" || args[1] == "--export-archive-movie-clip" ||
            args[1] == "--export-archive-movie-clip-single")
        {
            const bool single = args[1] == "--export-archive-movie-clip-single";
            if (!edits.setClipRange(document.project, single ? 2 : 1, single ? 2 : 3, single ? 1 : 2) || !edits.undo(document.project) ||
                !edits.redo(document.project))
                throw motrack::ProjectError("Could not edit the archived media clip.");
        }
        motrack::writeWorkspaceDocument(native, document);
        document = {};
        payload.reset();
        if (!QFile::remove(source))
            throw motrack::ProjectError("Could not remove original archive reference.");
        document = motrack::readWorkspaceDocument(native);
        if (args[1] == "--export-paired-archive-images" || args[1].startsWith("--export-paired-archive-movie"))
            saveAndRecoverPair(args[3], document, native);
        else
            motrack::writeTrackerArchiveDocument(args[3], document);
        return 0;
    }
    if (args.size() == 4 &&
        (args[1] == "--export-linked-movie" || args[1] == "--export-linked-movie-clip" || args[1] == "--export-linked-movie-clip-single" ||
         args[1] == "--export-packaged-movie" || args[1] == "--export-packaged-movie-clip" ||
         args[1] == "--export-packaged-movie-clip-single" || args[1] == "--export-linked-movie-calibration" ||
         args[1] == "--export-packaged-movie-calibration" || args[1] == "--export-linked-movie-keyed-calibration" ||
         args[1] == "--export-packaged-movie-keyed-calibration" || args[1].startsWith("--export-linked-movie-tracks-") ||
         args[1].startsWith("--export-packaged-movie-tracks-")))
    {
        const bool packaged = args[1].startsWith("--export-packaged-movie");
        QTemporaryDir temporary;
        const auto inputDirectory = temporary.filePath("source");
        const auto source = inputDirectory + "/input.trk";
        const auto native = temporary.filePath("saved.motrack");
        const QDir originalDirectory(QFileInfo(args[2]).absolutePath());
        const QDir destinationDirectory(QFileInfo(args[3]).absolutePath());
        const auto mediaDirectory = destinationDirectory.absoluteFilePath(QString::fromUtf8("média & movie"));
        const bool keyed = args[1].endsWith("-movie-keyed-calibration");
        const bool authoring = args[1].contains("-movie-tracks-");
        const auto name = packaged || keyed || authoring ? QStringLiteral("constant-rate.mov") : QStringLiteral("constant-rate.avi");
        const auto movie = mediaDirectory + '/' + name;
        if (!temporary.isValid() || !QDir().mkdir(inputDirectory) || !QDir().mkdir(mediaDirectory) || !QFile::copy(args[2], source) ||
            !QFile::copy(originalDirectory.filePath(name), movie))
            throw motrack::ProjectError("Could not prepare linked-movie round trip.");
        auto document = motrack::importTrackerDocument(source);
        if (document.project.clip.mediaReferences != std::vector<std::string>{name.toStdString()})
            throw motrack::ProjectError("Expected the pinned movie reference.");
        const std::atomic_bool cancelled{false};
        const auto index = motrack::buildMovieIndex(movie, motrack::VideoOrientation::Encoded, cancelled);
        if (index.frameCount != 4 || index.frameTimes != std::vector<double>{0.0, 0.125, 0.25, 0.375})
            throw motrack::ProjectError("The native movie index differs from the independent timing values.");
        document.movie = true;
        document.alignMovieToClip = true;
        document.movieFrameTimes = index.frameTimes;
        document.project.clip.sourceFrameCount = index.frameCount;
        document.project.clip.mediaReferences.front() = QDir(inputDirectory).relativeFilePath(movie).toStdString();
        motrack::TrackEdits edits;
        if (!edits.setPointMassValue(document.project, 0, 5.0) ||
            !edits.markPointMass(document.project, 0, {.frame = 1, .x = 8.0, .y = 12.0}) || !edits.undo(document.project) ||
            !edits.redo(document.project))
            throw motrack::ProjectError("Could not edit linked-movie reference.");
        if (args[1].endsWith("-movie-calibration"))
            editMovieCalibration(document, edits);
        if (keyed)
            editKeyedMovieCalibration(document, edits);
        if (authoring)
            editMovieTracks(document.project, edits, args[1].section("-movie-tracks-", 1, 1));
        if (args[1].contains("-clip"))
        {
            const bool single = args[1].endsWith("-single");
            if (!edits.setClipRange(document.project, single ? 2 : 1, single ? 2 : 3, single ? 1 : 2) || !edits.undo(document.project) ||
                !edits.redo(document.project))
                throw motrack::ProjectError("Could not edit the movie clip range.");
        }
        motrack::rebaseWorkspaceMedia(document, source, native);
        motrack::writeWorkspaceDocument(native, document);
        document = {};
        if (!QFile::remove(source) || !QDir().rmdir(inputDirectory))
            throw motrack::ProjectError("Could not remove the original movie project directory.");
        document = motrack::readWorkspaceDocument(native);
        if (packaged)
        {
            motrack::writeTrackerMoviePackage(args[3], document, native);
            if (!QFile::remove(movie) || !QDir().rmdir(mediaDirectory))
                throw motrack::ProjectError("Could not remove original packaged movie.");
        }
        else
            motrack::writeTrackerDocument(args[3], document, native);
        return 0;
    }
    if (args.size() == 4 && (args[1] == "--export-linked-images" || args[1] == "--export-paired-linked-images" ||
                             args[1] == "--export-packaged-images" || args[1] == "--export-linked-image-clip" ||
                             args[1] == "--export-linked-image-clip-single" || args[1] == "--export-packaged-image-clip"))
    {
        QTemporaryDir temporary;
        const auto source = temporary.filePath("input.trk");
        const auto native = temporary.filePath("saved.motrack");
        const QDir originalDirectory(QFileInfo(args[2]).absolutePath());
        const QDir destinationDirectory(QFileInfo(args[3]).absolutePath());
        const auto mediaDirectory = destinationDirectory.absoluteFilePath(QString::fromUtf8("media é & frames"));
        if (!temporary.isValid() || !QFile::copy(args[2], source) || !QDir().mkdir(mediaDirectory))
            throw motrack::ProjectError("Could not prepare linked-image round trip.");
        auto document = motrack::importTrackerDocument(source);
        if (document.project.clip.mediaReferences.size() != 4)
            throw motrack::ProjectError("Expected four reference images.");
        for (int frame = 0; frame < 4; ++frame)
        {
            const auto name = "frame" + QString::number(frame) + ".png";
            const auto original = "images/" + name;
            if (document.project.clip.mediaReferences[static_cast<std::size_t>(frame)] != original.toStdString() ||
                !QFile::copy(originalDirectory.absoluteFilePath(original), mediaDirectory + "/" + name))
                throw motrack::ProjectError("Could not relocate the reference image.");
            document.project.clip.mediaReferences[static_cast<std::size_t>(frame)] =
                QDir(temporary.path()).relativeFilePath(mediaDirectory + "/" + name).toStdString();
        }
        motrack::TrackEdits edits;
        if (!edits.setPointMassValue(document.project, 0, 5.0) ||
            !edits.markPointMass(document.project, 0, {.frame = 1, .x = 8.0, .y = 12.0}) || !edits.undo(document.project) ||
            !edits.redo(document.project))
            throw motrack::ProjectError("Could not edit linked-image reference.");
        if (args[1] == "--export-linked-image-clip" || args[1] == "--export-linked-image-clip-single" ||
            args[1] == "--export-packaged-image-clip")
        {
            const bool single = args[1] != "--export-linked-image-clip";
            if (!edits.setClipRange(document.project, single ? 2 : 1, single ? 2 : 3, single ? 1 : 2) || !edits.undo(document.project) ||
                !edits.redo(document.project))
                throw motrack::ProjectError("Could not edit the linked-image clip range.");
        }
        motrack::writeWorkspaceDocument(native, document);
        document = {};
        if (!QFile::remove(source))
            throw motrack::ProjectError("Could not remove linked-image source.");
        document = motrack::readWorkspaceDocument(native);
        if (args[1] == "--export-paired-linked-images")
            saveAndRecoverPair(args[3], document, native);
        else if (args[1] == "--export-packaged-images" || args[1] == "--export-packaged-image-clip")
        {
            motrack::writeTrackerImagePackage(args[3], document, native);
            for (int frame = 0; frame < 4; ++frame)
                if (!QFile::remove(mediaDirectory + "/frame" + QString::number(frame) + ".png"))
                    throw motrack::ProjectError("Could not remove original packaged media.");
            if (!QDir().rmdir(mediaDirectory))
                throw motrack::ProjectError("Could not remove original packaged-media directory.");
        }
        else
            motrack::writeTrackerDocument(args[3], document, native);
        return 0;
    }
    if (args.size() == 4 &&
        (args[1] == "--export-mass" || args[1] == "--export-marks" || args[1] == "--export-empty-marks" ||
         args[1] == "--export-first-mark" || args[1] == "--export-vectors" || args[1] == "--export-empty-vectors" ||
         args[1] == "--export-first-vector" || args[1] == "--export-clip" || args[1] == "--export-snap-origin" ||
         args[1] == "--export-snap-vector" || args[1] == "--export-length-unit" || args[1] == "--export-calibration" ||
         args[1] == "--export-clip-boundary" || args[1] == "--export-keyed-calibration" || args[1] == "--export-mixed-calibration" ||
         args[1] == "--export-mass-unit" || args[1] == "--export-time-unit" || args[1] == "--export-name" ||
         args[1] == "--export-unselected-name" || args[1] == "--export-vector-name" || args[1] == "--export-plot-owner-name" ||
         args[1] == "--export-plot-guest-name" || args[1] == "--export-center-member-name" || args[1] == "--export-attachment-first-name" ||
         args[1] == "--export-attachment-last-name" || args[1] == "--export-model-booster-name" || args[1] == "--export-clip-timing" ||
         args[1] == "--export-clip-timing-range" || args[1] == "--export-create-mass" || args[1] == "--export-create-empty-mass" ||
         args[1] == "--export-create-vector" || args[1] == "--export-create-empty-vector" || args[1] == "--export-delete-mass" ||
         args[1] == "--export-delete-vector" || args[1] == "--export-delete-all-vectors" || args[1] == "--export-delete-selected-mass" ||
         args[1] == "--export-delete-all-masses" || args[1] == "--export-delete-plot-owner" || args[1] == "--export-delete-plot-guest" ||
         args[1] == "--export-delete-first-plot-guest" || args[1] == "--export-delete-first-table-owner" ||
         args[1] == "--export-delete-selected-table-owner" || args[1] == "--export-delete-last-table-owner" ||
         args[1] == "--export-delete-all-table-owners"))
    {
        QTemporaryDir temporary;
        const auto source = temporary.filePath("input.trk");
        const auto native = temporary.filePath("saved.motrack");
        if (!temporary.isValid() || !QFile::copy(args[2], source))
        {
            throw motrack::ProjectError("Could not prepare round-trip source.");
        }
        auto document = motrack::importTrackerDocument(source);
        motrack::TrackEdits edits;
        if (args[1] == "--export-delete-first-table-owner" || args[1] == "--export-delete-selected-table-owner" ||
            args[1] == "--export-delete-last-table-owner" || args[1] == "--export-delete-all-table-owners")
        {
            if (document.project.pointMasses.size() != 3)
                throw motrack::ProjectError("Expected the round-trip table owners.");
            const std::size_t index = args[1] == "--export-delete-selected-table-owner" ? 1
                                      : args[1] == "--export-delete-last-table-owner"   ? 2
                                                                                        : 0;
            const int count = args[1] == "--export-delete-all-table-owners" ? 3 : 1;
            for (int i = 0; i < count; ++i)
                if (!edits.removePointMass(document.project, index))
                    throw motrack::ProjectError("Could not delete the round-trip table owner.");
            if (!edits.undo(document.project) || !edits.redo(document.project))
                throw motrack::ProjectError("Could not replay the round-trip table deletion.");
        }
        else if (args[1] == "--export-delete-plot-owner" || args[1] == "--export-delete-plot-guest" ||
                 args[1] == "--export-delete-first-plot-guest")
        {
            const std::size_t index = args[1] == "--export-delete-plot-owner" ? 0 : args[1] == "--export-delete-plot-guest" ? 1 : 2;
            if (document.project.pointMasses.size() != 3 || !edits.removePointMass(document.project, index) ||
                !edits.undo(document.project) || !edits.redo(document.project))
                throw motrack::ProjectError("Could not replay the round-trip plot deletion.");
        }
        else if (args[1] == "--export-delete-mass" || args[1] == "--export-delete-vector" || args[1] == "--export-delete-all-vectors" ||
                 args[1] == "--export-delete-selected-mass" || args[1] == "--export-delete-all-masses")
        {
            const bool mass =
                args[1] == "--export-delete-mass" || args[1] == "--export-delete-selected-mass" || args[1] == "--export-delete-all-masses";
            const std::size_t massIndex = args[1] == "--export-delete-mass" ? 0 : 1;
            if ((mass && (document.project.pointMasses.size() != 2 || !edits.removePointMass(document.project, massIndex))) ||
                (!mass && (document.project.vectors.size() != 2 || !edits.removeVector(document.project, 0))))
                throw motrack::ProjectError("Could not delete the round-trip track.");
            if (args[1] == "--export-delete-all-vectors" && !edits.removeVector(document.project, 0))
                throw motrack::ProjectError("Could not delete the final round-trip vector.");
            if (args[1] == "--export-delete-all-masses" && !edits.removePointMass(document.project, 0))
                throw motrack::ProjectError("Could not delete the final round-trip mass.");
            if (!edits.undo(document.project) || !edits.redo(document.project))
                throw motrack::ProjectError("Could not replay the round-trip deletion.");
        }
        else if (args[1] == "--export-create-vector" || args[1] == "--export-create-empty-vector")
        {
            const auto name = QString::fromUtf8("vecteur β & <C>").toStdString();
            if (document.project.vectors.size() != 2 || !edits.createVector(document.project, "discarded") ||
                !edits.undo(document.project) || !edits.createVector(document.project, name))
                throw motrack::ProjectError("Could not create the round-trip vector.");
            if (args[1] == "--export-create-vector")
                for (const auto& sample :
                     std::array<motrack::VectorSample, 3>{{{.frame = 1, .tailX = 100, .tailY = 120, .componentX = 4, .componentY = -8},
                                                           {.frame = 6, .tailX = 200, .tailY = 180, .componentX = 0, .componentY = 0},
                                                           {.frame = 8, .tailX = 160, .tailY = 100, .componentX = -6, .componentY = 12}}})
                    if (!edits.markVector(document.project, 2, sample))
                        throw motrack::ProjectError("Could not mark the new round-trip vector.");
            if (!edits.undo(document.project) || !edits.redo(document.project) || !edits.removeVector(document.project, 2) ||
                !edits.undo(document.project))
                throw motrack::ProjectError("Could not restore the new round-trip vector.");
            std::ranges::reverse(document.project.vectors);
        }
        else if (args[1] == "--export-create-mass" || args[1] == "--export-create-empty-mass")
        {
            const auto name = QString::fromUtf8("nouvelle β & <C>").toStdString();
            if (document.project.pointMasses.size() != 2 || !edits.createPointMass(document.project, "discarded") ||
                !edits.undo(document.project) || !edits.createPointMass(document.project, name) ||
                !edits.setPointMassValue(document.project, 2, 3))
                throw motrack::ProjectError("Could not create the round-trip point mass.");
            if (args[1] == "--export-create-mass")
                for (const auto& mark : std::array<motrack::Mark, 3>{
                         {{.frame = 1, .x = 330, .y = 220}, {.frame = 4, .x = 340, .y = 210}, {.frame = 8, .x = 360, .y = 230}}})
                    if (!edits.markPointMass(document.project, 2, mark))
                        throw motrack::ProjectError("Could not mark the new round-trip point mass.");
            if (!edits.undo(document.project) || !edits.redo(document.project) || !edits.removePointMass(document.project, 2) ||
                !edits.undo(document.project))
                throw motrack::ProjectError("Could not restore the new round-trip point mass.");
            std::ranges::reverse(document.project.pointMasses);
        }
        else if (args[1] == "--export-model-booster-name")
        {
            const auto name = QString::fromUtf8("élève β & <B>").toStdString();
            if (document.project.pointMasses.size() != 1 || !edits.renamePointMass(document.project, 0, "temporary rename") ||
                !edits.renamePointMass(document.project, 0, name) || !edits.undo(document.project) || !edits.redo(document.project))
            {
                throw motrack::ProjectError("Could not rename the round-trip model booster.");
            }
        }
        else if (args[1] == "--export-attachment-first-name" || args[1] == "--export-attachment-last-name")
        {
            const std::size_t index = args[1] == "--export-attachment-first-name" ? 0 : 1;
            const auto name = QString::fromUtf8("élève β & <B>").toStdString();
            if (document.project.pointMasses.size() != 2 || !edits.renamePointMass(document.project, index, "temporary rename") ||
                !edits.renamePointMass(document.project, index, name) || !edits.undo(document.project) || !edits.redo(document.project))
            {
                throw motrack::ProjectError("Could not rename the round-trip attachment target.");
            }
        }
        else if (args[1] == "--export-center-member-name")
        {
            const auto name = QString::fromUtf8("élève β & <B>").toStdString();
            if (document.project.pointMasses.size() != 2 || !edits.renamePointMass(document.project, 0, "temporary rename") ||
                !edits.renamePointMass(document.project, 0, name) || !edits.undo(document.project) || !edits.redo(document.project))
            {
                throw motrack::ProjectError("Could not rename the round-trip center constituent.");
            }
        }
        else if (args[1] == "--export-plot-owner-name" || args[1] == "--export-plot-guest-name")
        {
            const std::size_t index = args[1] == "--export-plot-owner-name" ? 0 : 1;
            const auto name = QString::fromUtf8("élève β & <B>").toStdString();
            if (document.project.pointMasses.size() != 3 || !edits.renamePointMass(document.project, index, "temporary rename") ||
                !edits.renamePointMass(document.project, index, name) || !edits.undo(document.project) || !edits.redo(document.project))
            {
                throw motrack::ProjectError("Could not rename the round-trip comparison track.");
            }
        }
        else if (args[1] == "--export-vector-name")
        {
            const auto name = QString::fromUtf8("élève β & <B>").toStdString();
            if (document.project.vectors.size() != 2 || !edits.renameVector(document.project, 1, "temporary rename") ||
                !edits.renameVector(document.project, 1, name) || !edits.undo(document.project) || !edits.redo(document.project))
            {
                throw motrack::ProjectError("Could not rename the round-trip vector.");
            }
        }
        else if (args[1] == "--export-name" || args[1] == "--export-unselected-name")
        {
            const std::size_t index = args[1] == "--export-name" ? 1 : 0;
            const auto name = QString::fromUtf8("élève β & <B>").toStdString();
            if (document.project.pointMasses.size() != 2 || !edits.renamePointMass(document.project, index, "temporary rename") ||
                !edits.renamePointMass(document.project, index, name) || !edits.undo(document.project) || !edits.redo(document.project))
            {
                throw motrack::ProjectError("Could not rename the round-trip point mass.");
            }
        }
        else if (args[1] == "--export-mass")
        {
            if (document.project.pointMasses.empty() ||
                !edits.setPointMassValue(document.project, 0, document.project.pointMasses.front().mass * 2.5))
            {
                throw motrack::ProjectError("Could not edit round-trip mass.");
            }
        }
        else if (args[1] == "--export-time-unit")
        {
            if (!edits.setTimeUnit(document.project, "temporary") ||
                !edits.setTimeUnit(document.project, QString::fromUtf8("τ & <tick>").toStdString()) || !edits.undo(document.project) ||
                !edits.redo(document.project))
            {
                throw motrack::ProjectError("Could not edit round-trip time unit.");
            }
        }
        else if (args[1] == "--export-mass-unit")
        {
            if (!edits.setMassUnit(document.project, "g"))
            {
                throw motrack::ProjectError("Could not edit round-trip mass unit.");
            }
        }
        else if (args[1] == "--export-length-unit")
        {
            if (!edits.calibrateDistance(document.project,
                                         {.frame = 0,
                                          .imageDeltaX = 4,
                                          .imageDeltaY = 0,
                                          .knownLength = 2,
                                          .defaultOriginX = 320,
                                          .defaultOriginY = 240,
                                          .unit = "\xC2\xB5m"}))
            {
                throw motrack::ProjectError("Could not edit round-trip length unit.");
            }
        }
        else if (args[1] == "--export-keyed-calibration" || args[1] == "--export-mixed-calibration")
        {
            if (!edits.calibrateDistance(document.project,
                                         {.frame = 4,
                                          .imageDeltaX = 8,
                                          .imageDeltaY = 0,
                                          .knownLength = 1,
                                          .defaultOriginX = 0,
                                          .defaultOriginY = 0,
                                          .unit = document.project.lengthUnit}) ||
                !edits.setCoordinateAxes(document.project,
                                         {.frame = 4, .originX = 120, .originY = 140, .positiveX = 128, .positiveY = 140}) ||
                !edits.setCoordinateAxes(document.project, {.frame = 3, .originX = 80, .originY = 90, .positiveX = 80, .positiveY = 98}))
            {
                throw motrack::ProjectError("Could not edit round-trip calibration keys.");
            }
        }
        else if (args[1] == "--export-calibration")
        {
            if (!edits.calibrateDistance(document.project,
                                         {.frame = 0,
                                          .imageDeltaX = 8,
                                          .imageDeltaY = 0,
                                          .knownLength = 2,
                                          .defaultOriginX = 320,
                                          .defaultOriginY = 240,
                                          .unit = document.project.lengthUnit}) ||
                !edits.setCoordinateAxes(document.project,
                                         {.frame = 0, .originX = 100, .originY = 120, .positiveX = 100, .positiveY = 112}))
            {
                throw motrack::ProjectError("Could not edit round-trip calibration.");
            }
        }
        else if (args[1] == "--export-clip-timing" || args[1] == "--export-clip-timing-range")
        {
            if ((args[1] == "--export-clip-timing-range" && !edits.setClipRange(document.project, 2, 8, 2)) ||
                !edits.setClipTiming(document.project, 250, -500) || !edits.undo(document.project) || !edits.redo(document.project))
            {
                throw motrack::ProjectError("Could not edit round-trip clip timing.");
            }
        }
        else if (args[1] == "--export-clip" || args[1] == "--export-clip-boundary")
        {
            const bool boundary = args[1] == "--export-clip-boundary";
            if (!edits.setClipRange(document.project, boundary ? 500000 : 2, boundary ? 500000 : 8, boundary ? 1 : 2))
            {
                throw motrack::ProjectError("Could not edit round-trip clip range.");
            }
        }
        else if (args[1].startsWith("--export-snap-"))
        {
            const bool origin = args[1] == "--export-snap-origin";
            if (document.project.vectors.size() != 2 ||
                !edits.markVector(document.project,
                                  origin ? 0 : 1,
                                  origin ? motrack::VectorSample{.frame = 0, .tailX = 321, .tailY = 240, .componentX = 3, .componentY = -4}
                                         : motrack::VectorSample{.frame = 2, .tailX = 14, .tailY = 16, .componentX = 0, .componentY = 0}))
            {
                throw motrack::ProjectError("Could not prepare rejected snapping export.");
            }
        }
        else if (args[1].contains("vector"))
        {
            if (document.project.vectors.size() != 2)
            {
                throw motrack::ProjectError("Expected the pinned two-vector case.");
            }
            if (args[1] == "--export-vectors")
            {
                if (!edits.markVector(
                        document.project, 0, {.frame = 0, .tailX = 22.5, .tailY = 16.75, .componentX = -6.5, .componentY = 8.25}) ||
                    !edits.markVector(
                        document.project, 0, {.frame = 3, .tailX = 100.125, .tailY = 200.5, .componentX = 0, .componentY = -2.5}) ||
                    !edits.removeVectorSample(document.project, 0, 5) || !edits.removeVectorSample(document.project, 1, 2) ||
                    !edits.markVector(document.project, 1, {.frame = 7, .tailX = 50, .tailY = 60, .componentX = -10, .componentY = 20}))
                {
                    throw motrack::ProjectError("Could not edit round-trip vectors.");
                }
            }
            else if (args[1] == "--export-empty-vectors")
            {
                const auto original = document.project.vectors.front().samples;
                for (const auto& sample : original)
                {
                    if (!edits.removeVectorSample(document.project, 0, sample.frame))
                    {
                        throw motrack::ProjectError("Could not clear round-trip vector samples.");
                    }
                }
            }
            else if (!document.project.vectors.front().samples.empty() ||
                     !edits.markVector(document.project, 0, {.frame = 4, .tailX = 11.25, .tailY = 22.5, .componentX = 0, .componentY = 0}))
            {
                throw motrack::ProjectError("Could not add the first round-trip vector sample.");
            }
        }
        else if (args[1] == "--export-marks")
        {
            if (document.project.pointMasses.size() != 2 || document.project.pointMasses.front().marks.empty())
            {
                throw motrack::ProjectError("Expected the pinned two-point-mass mark case.");
            }
            const auto original = document.project.pointMasses.front().marks.front();
            if (!edits.markPointMass(document.project, 0, {.frame = original.frame, .x = original.x + 12.5, .y = original.y - 3.25}) ||
                !edits.markPointMass(document.project, 0, {.frame = 3, .x = 100.125, .y = 200.5}) ||
                !edits.removeMark(document.project, 1, 0) || !edits.markPointMass(document.project, 1, {.frame = 7, .x = 50, .y = 60}))
            {
                throw motrack::ProjectError("Could not edit round-trip marks.");
            }
        }
        else
        {
            if (document.project.pointMasses.size() != 2)
            {
                throw motrack::ProjectError("Expected the pinned empty-mark case.");
            }
            if (args[1] == "--export-empty-marks")
            {
                const auto original = document.project.pointMasses.front().marks;
                for (const auto& mark : original)
                {
                    if (!edits.removeMark(document.project, 0, mark.frame))
                    {
                        throw motrack::ProjectError("Could not clear round-trip marks.");
                    }
                }
            }
            else if (!document.project.pointMasses.front().marks.empty() ||
                     !edits.markPointMass(document.project, 0, {.frame = 4, .x = 11.25, .y = 22.5}))
            {
                throw motrack::ProjectError("Could not add the first round-trip mark.");
            }
        }
        motrack::writeWorkspaceDocument(native, document);
        document = {};
        if (!QFile::remove(source))
        {
            throw motrack::ProjectError("Could not remove round-trip source copy.");
        }
        document = motrack::readWorkspaceDocument(native);
        if (!document.trackerSource)
        {
            throw motrack::ProjectError("Native reopen lost Tracker source.");
        }
        if (args[1].startsWith("--export-snap-"))
        {
            if (!QFile::copy(args[2], args[3]))
            {
                throw motrack::ProjectError("Could not prepare existing destination.");
            }
            bool rejected = false;
            try
            {
                motrack::writeTrackerProject(args[3], document.project, *document.trackerSource);
            }
            catch (const motrack::ProjectError&)
            {
                rejected = true;
            }
            QFile output(args[3]);
            if (!rejected || !output.open(QIODevice::ReadOnly) || output.readAll().toStdString() != document.trackerSource->xml)
            {
                throw motrack::ProjectError("Snapping export was not rejected without replacing its destination.");
            }
        }
        else
        {
            motrack::writeTrackerProject(args[3], document.project, *document.trackerSource);
        }
        return 0;
    }
    const bool references = args.size() == 4 && args[1] == "--references";
    const bool timing = args.size() == 4 && args[1] == "--timing";
    if (args.size() != 3 && !references && !timing)
    {
        std::fputs("Usage: motrack_corpus_probe [--references|--timing] input.trk output.json | --export-<case> input.trk output.trk\n",
                   stderr);
        return 1;
    }
    const auto& inputArgument = args[references || timing ? 2 : 1];
#ifdef _WIN32
    const std::filesystem::path input(inputArgument.toStdWString());
#else
    const std::filesystem::path input(inputArgument.toStdString());
#endif
    const auto report = [&]() -> QJsonObject
    {
        if (references)
        {
            return referenceSnapshot(motrack::readTrackerProject(input));
        }
        const auto project = motrack::readLegacyProject(input);
        if (!timing)
        {
            return snapshot(project);
        }
        const auto& clip = project.clip;
        if (!clip.frameDurationMs || !clip.stepCount || !motrack::validConstantClipTiming(clip))
        {
            throw motrack::ProjectError("Expected valid constant clip timing.");
        }
        const auto last = clip.firstFrame + (*clip.stepCount - 1) * clip.stepSize;
        QJsonArray times;
        for (const auto frame : {clip.firstFrame, last})
        {
            times.append(QJsonObject{{"frame", frame}, {"time_ms", clip.startTimeMs + (frame - clip.firstFrame) * *clip.frameDurationMs}});
        }
        return {{"frame_duration_ms", *clip.frameDurationMs}, {"time_samples", times}};
    }();
    const auto bytes = QJsonDocument(report).toJson();
    QSaveFile output(args[references || timing ? 3 : 2]);
    if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit())
    {
        throw motrack::ProjectError("Could not write corpus snapshot.");
    }
    return 0;
}
catch (const std::exception& error)
{
    std::fputs(error.what(), stderr);
    std::fputc('\n', stderr);
    return 1;
}
