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
    if (args.size() == 4 && args[1] == "--export-mass")
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
        if (document.project.pointMasses.empty() ||
            !edits.setPointMassValue(document.project, 0, document.project.pointMasses.front().mass * 2.5))
        {
            throw motrack::ProjectError("Could not edit round-trip mass.");
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
        std::fputs("Usage: motrack_corpus_probe input.trk output.json | --export-mass input.trk output.trk\n", stderr);
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
