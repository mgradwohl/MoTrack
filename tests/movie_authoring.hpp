#pragma once

#include "track_edits.hpp"

#include <QString>

// Exercise the same public edits for every standard save route. Expected file/scientific
// results are authored independently in the Python reference oracle, not derived here.
inline void editMovieTracks(motrack::LegacyProject& project, motrack::TrackEdits& edits, const QString& mode)
{
    if (project.pointMasses.size() != 2 || project.pointMasses[0].name != "movie mass" || project.pointMasses[1].name != "other mass")
        throw motrack::ProjectError("Unexpected movie authoring baseline.");
    auto before = project;
    int count = 0;
    const auto apply = [&](bool result)
    {
        if (!result)
            throw motrack::ProjectError("Could not edit movie track identity or membership.");
        ++count;
    };
    if (mode == "selected-name" || mode == "unselected-name")
        apply(edits.renamePointMass(project, mode == "selected-name" ? 0 : 1, QString::fromUtf8("élève β & <B>").toStdString()));
    else if (mode == "create")
    {
        apply(edits.createPointMass(project, QString::fromUtf8("nouvelle β & <C>").toStdString()));
        apply(edits.setPointMassValue(project, 2, 4));
        apply(edits.markPointMass(project, 2, {.frame = 0, .x = 8, .y = 4}));
        apply(edits.markPointMass(project, 2, {.frame = 3, .x = 16, .y = 8}));
        apply(edits.createPointMass(project, "empty movie track"));
    }
    else if (mode == "delete-selected" || mode == "delete-unselected")
        apply(edits.removePointMass(project, mode == "delete-selected" ? 0 : 1));
    else if (mode == "delete-all")
    {
        apply(edits.removePointMass(project, 1));
        apply(edits.removePointMass(project, 0));
    }
    else if (mode == "replace")
    {
        apply(edits.removePointMass(project, 0));
        apply(edits.createPointMass(project, "movie mass"));
        apply(edits.setPointMassValue(project, 1, 7));
        apply(edits.markPointMass(project, 1, {.frame = 3, .x = 14, .y = 0}));
    }
    else
        throw motrack::ProjectError("Unknown movie authoring scenario.");
    const auto edited = project;
    before.nextTrackId = edited.nextTrackId; // IDs remain reserved even after creation is undone.
    for (int index = 0; index < count; ++index)
        if (!edits.undo(project))
            throw motrack::ProjectError("Could not undo movie authoring.");
    if (project != before)
        throw motrack::ProjectError("Movie authoring undo changed original track identity or data.");
    for (int index = 0; index < count; ++index)
        if (!edits.redo(project))
            throw motrack::ProjectError("Could not redo movie authoring.");
    if (project != edited)
        throw motrack::ProjectError("Movie authoring redo changed the edited experiment.");
}
