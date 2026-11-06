#include "legacy_project.hpp"
#include "measurements.hpp"
#include "track_edits.hpp"
#include "tracker_writer.hpp"
#include "workspace_document.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTemporaryDir>

#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <limits>

namespace
{

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
                             args[1] == "--export-first-vector"))
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
        if (args[1] == "--export-mass")
        {
            if (document.project.pointMasses.empty() ||
                !edits.setPointMassValue(document.project, 0, document.project.pointMasses.front().mass * 2.5))
            {
                throw motrack::ProjectError("Could not edit round-trip mass.");
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
        motrack::writeTrackerProject(args[3], document.project, *document.trackerSource);
        return 0;
    }
    if (args.size() != 3)
    {
        std::fputs("Usage: motrack_corpus_probe input.trk output.json | --export-<case> input.trk output.trk\n", stderr);
        return 1;
    }
#ifdef _WIN32
    const std::filesystem::path input(args[1].toStdWString());
#else
    const std::filesystem::path input(args[1].toStdString());
#endif
    const auto bytes = QJsonDocument(snapshot(motrack::readLegacyProject(input))).toJson();
    QSaveFile output(args[2]);
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
