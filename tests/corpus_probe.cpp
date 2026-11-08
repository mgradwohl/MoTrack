#include "legacy_project.hpp"
#include "measurements.hpp"
#include "track_edits.hpp"
#include "tracker_references.hpp"
#include "tracker_writer.hpp"
#include "workspace_document.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStringList>
#include <QTemporaryDir>

#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <limits>

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
    if (args.size() == 4 && (args[1] == "--export-mass" || args[1] == "--export-marks" || args[1] == "--export-empty-marks" ||
                             args[1] == "--export-first-mark" || args[1] == "--export-vectors" || args[1] == "--export-empty-vectors" ||
                             args[1] == "--export-first-vector" || args[1] == "--export-clip" || args[1] == "--export-snap-origin" ||
                             args[1] == "--export-snap-vector" || args[1] == "--export-length-unit" || args[1] == "--export-calibration" ||
                             args[1] == "--export-clip-boundary" || args[1] == "--export-keyed-calibration" ||
                             args[1] == "--export-mass-unit" || args[1] == "--export-name" || args[1] == "--export-unselected-name"))
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
        if (args[1] == "--export-name" || args[1] == "--export-unselected-name")
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
        else if (args[1] == "--export-keyed-calibration")
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
    if (args.size() != 3 && !references)
    {
        std::fputs("Usage: motrack_corpus_probe [--references] input.trk output.json | --export-<case> input.trk output.trk\n", stderr);
        return 1;
    }
    const auto& inputArgument = args[references ? 2 : 1];
#ifdef _WIN32
    const std::filesystem::path input(inputArgument.toStdWString());
#else
    const std::filesystem::path input(inputArgument.toStdString());
#endif
    const auto bytes =
        QJsonDocument(references ? referenceSnapshot(motrack::readTrackerProject(input)) : snapshot(motrack::readLegacyProject(input)))
            .toJson();
    QSaveFile output(args[references ? 3 : 2]);
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
