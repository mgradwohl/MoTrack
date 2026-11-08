#include "legacy_project.hpp"
#include "measurements.hpp"
#include "track_edits.hpp"
#include "tracker_archive.hpp"
#include "tracker_references.hpp"
#include "tracker_writer.hpp"
#include "workspace_document.hpp"

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
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>

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

} // namespace

int main(int argc, char** argv)
try
{
    const QCoreApplication application(argc, argv);
    const auto args = application.arguments();
    if (args.size() == 4 && (args[1] == "--export-archive-images" || args[1] == "--export-multitab-archive-images"))
    {
        QTemporaryDir temporary;
        const auto source = temporary.filePath("input.trz");
        const auto native = temporary.filePath("saved.motrack");
        if (!temporary.isValid() || !QFile::copy(args[2], source))
            throw motrack::ProjectError("Could not prepare archive round trip.");
        auto payload = std::make_shared<motrack::TrackerArchive>(motrack::TrackerArchive::open(source));
        const auto expected =
            args[1] == "--export-multitab-archive-images" ? QStringList{"experiment.trk", "second.trk"} : QStringList{"experiment.trk"};
        if (payload->projects() != expected)
            throw motrack::ProjectError("Expected the pinned archive experiment.");
        auto document = motrack::importTrackerDocument(payload->projectPath("experiment.trk"));
        payload->resolveMedia(document.project, "experiment.trk");
        document.archivePayload = payload;
        document.archiveMember = "experiment.trk";
        motrack::TrackEdits edits;
        if (!edits.setPointMassValue(document.project, 0, 5.0) ||
            !edits.markPointMass(document.project, 0, {.frame = 1, .x = 8.0, .y = 12.0}) || !edits.undo(document.project) ||
            !edits.redo(document.project))
            throw motrack::ProjectError("Could not edit archived image reference.");
        motrack::writeWorkspaceDocument(native, document);
        document = {};
        payload.reset();
        if (!QFile::remove(source))
            throw motrack::ProjectError("Could not remove original archive reference.");
        document = motrack::readWorkspaceDocument(native);
        motrack::writeTrackerArchiveDocument(args[3], document);
        return 0;
    }
    if (args.size() == 4 && args[1] == "--export-linked-images")
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
        motrack::writeWorkspaceDocument(native, document);
        document = {};
        if (!QFile::remove(source))
            throw motrack::ProjectError("Could not remove linked-image source.");
        document = motrack::readWorkspaceDocument(native);
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
