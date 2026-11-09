#pragma once

#include "track_edits.hpp"

#include <QString>

// Public authoring operations shared by native tests and the reference export probe.
// Expected XML and scientific values live in an independent Python oracle.
inline void editMediaVectors(motrack::LegacyProject& project, motrack::TrackEdits& edits, const QString& mode)
{
    if (project.vectors.size() != 3 || project.vectors[0].name != "vector A" || project.vectors[1].name != "vector B" ||
        project.vectors[2].name != "empty vector" || !project.vectors[2].samples.empty())
        throw motrack::ProjectError("Unexpected media vector baseline.");
    auto before = project;
    int count = 0;
    const auto apply = [&](bool result)
    {
        if (!result)
            throw motrack::ProjectError("Could not edit media vector.");
        ++count;
    };
    if (mode == "samples")
    {
        apply(edits.markVector(project, 0, {.frame = 0, .tailX = 0, .tailY = 0, .componentX = 4, .componentY = 4}));
        apply(edits.removeVectorSample(project, 0, 2));
        apply(edits.markVector(project, 0, {.frame = 3, .tailX = 0, .tailY = 12, .componentX = 4, .componentY = 4}));
        apply(edits.markVector(project, 2, {.frame = 2, .tailX = 32, .tailY = 0, .componentX = -2, .componentY = -4}));
    }
    else if (mode == "geometry")
    {
        apply(edits.setCoordinateAxes(project, {.frame = 1, .originX = 10, .originY = 18, .positiveX = 10, .positiveY = 10}));
        apply(edits.calibrateDistance(project, {.frame = 2, .imageDeltaX = 8, .imageDeltaY = 0, .knownLength = 2, .unit = "cm"}));
        apply(edits.setClipRange(project, 1, 3, 2));
    }
    else if (mode == "empty")
    {
        for (std::size_t index = 0; index < project.vectors.size(); ++index)
            while (!project.vectors[index].samples.empty())
                apply(edits.removeVectorSample(project, index, project.vectors[index].samples.back().frame));
    }
    else if (mode == "authoring")
    {
        apply(edits.renameVector(project, 0, QString::fromUtf8("élève β & <B>").toStdString()));
        apply(edits.removeVector(project, 1));
        apply(edits.createVector(project, QString::fromUtf8("nouvelle β & <C>").toStdString()));
        apply(edits.markVector(project, 2, {.frame = 3, .tailX = 32, .tailY = 0, .componentX = -2, .componentY = -4}));
        apply(edits.createVector(project, "new empty vector"));
    }
    else if (mode == "replace")
    {
        apply(edits.renameVector(project, 1, QString::fromUtf8("élève β & <B>").toStdString()));
        apply(edits.removeVector(project, 0));
        apply(edits.createVector(project, "vector A"));
        apply(edits.markVector(project, 2, {.frame = 2, .tailX = 32, .tailY = 0, .componentX = -2, .componentY = -4}));
    }
    else if (mode == "delete-all")
    {
        while (!project.vectors.empty())
            apply(edits.removeVector(project, project.vectors.size() - 1));
    }
    else
        throw motrack::ProjectError("Unknown media vector scenario.");
    const auto edited = project;
    before.nextTrackId = edited.nextTrackId;
    for (int index = 0; index < count; ++index)
        if (!edits.undo(project))
            throw motrack::ProjectError("Could not undo media vector edits.");
    if (project != before)
        throw motrack::ProjectError("Media vector undo changed original identity or data.");
    for (int index = 0; index < count; ++index)
        if (!edits.redo(project))
            throw motrack::ProjectError("Could not redo media vector edits.");
    if (project != edited)
        throw motrack::ProjectError("Media vector redo changed the edited experiment.");
}
