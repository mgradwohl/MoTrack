#pragma once

#include "legacy_project.hpp"
#include "model/families.hpp"
#include "model/tracker_document.hpp"
#include "model/tracker_loaders.hpp"
#include "track_edits.hpp"
#include "tracker_compose.hpp"

#include <algorithm>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

/// The edits B2.3's corpus comparison applies to every corpus case and fixture (tests/tracker_compose_test.cpp and
/// `motrack_corpus_probe --compose-scenarios`): each one a user makes in MoTrack, through the history as the application
/// applies it, exported by B1's composition and by the descriptor composition.
namespace motrack::test
{

struct ComposeScenario
{
    std::string_view name;
    /// False when the edit does not apply to the project.
    std::function<bool(TrackEdits&, LegacyProject&)> apply;
};

inline int lastMarkFrame(const LegacyProject& project)
{
    int last = project.clip.firstFrame;
    for (const auto& track : project.pointMasses)
        for (const auto& mark : track.marks)
            last = (std::max)(last, mark.frame);
    return last;
}

inline const std::vector<ComposeScenario>& composeScenarios()
{
    static const std::vector<ComposeScenario> list{
        {.name = "unedited", .apply = [](TrackEdits&, LegacyProject&) { return true; }},
        {.name = "mass",
         .apply = [](TrackEdits& edits, LegacyProject& project)
         { return !project.pointMasses.empty() && edits.setPointMassValue(project, 0, project.pointMasses[0].mass * 2.0 + 0.25); }},
        {.name = "rename",
         .apply = [](TrackEdits& edits, LegacyProject& project) { return edits.renamePointMass(project, 0, "renamed mass"); }},
        {.name = "rename vector",
         .apply = [](TrackEdits& edits, LegacyProject& project) { return edits.renameVector(project, 0, "renamed vector"); }},
        {.name = "move mark",
         .apply =
             [](TrackEdits& edits, LegacyProject& project)
         {
             if (project.pointMasses.empty() || project.pointMasses[0].marks.empty())
                 return false;
             auto mark = project.pointMasses[0].marks.front();
             mark.x += 1.5;
             mark.y -= 2.25;
             return edits.markPointMass(project, 0, mark);
         }},
        {.name = "add mark",
         .apply = [](TrackEdits& edits, LegacyProject& project)
         { return edits.markPointMass(project, 0, {.frame = lastMarkFrame(project) + 1, .x = 12.5, .y = 34.75}); }},
        {.name = "remove mark",
         .apply =
             [](TrackEdits& edits, LegacyProject& project)
         {
             return !project.pointMasses.empty() && !project.pointMasses[0].marks.empty() &&
                    edits.removeMark(project, 0, project.pointMasses[0].marks.front().frame);
         }},
        {.name = "remove every mark",
         .apply =
             [](TrackEdits& edits, LegacyProject& project)
         {
             if (project.pointMasses.empty() || project.pointMasses[0].marks.empty())
                 return false;
             while (!project.pointMasses[0].marks.empty())
                 if (!edits.removeMark(project, 0, project.pointMasses[0].marks.front().frame))
                     return false;
             return true;
         }},
        {.name = "move sample",
         .apply =
             [](TrackEdits& edits, LegacyProject& project)
         {
             if (project.vectors.empty() || project.vectors[0].samples.empty())
                 return false;
             auto sample = project.vectors[0].samples.front();
             sample.componentX += 3.0;
             return edits.markVector(project, 0, sample);
         }},
        {.name = "add sample",
         .apply =
             [](TrackEdits& edits, LegacyProject& project)
         {
             return edits.markVector(
                 project, 0, {.frame = lastMarkFrame(project) + 2, .tailX = 40.0, .tailY = 50.0, .componentX = 30.0, .componentY = -20.0});
         }},
        {.name = "mass unit", .apply = [](TrackEdits& edits, LegacyProject& project) { return edits.setMassUnit(project, "g"); }},
        {.name = "time unit", .apply = [](TrackEdits& edits, LegacyProject& project) { return edits.setTimeUnit(project, "min"); }},
        {.name = "clip range",
         .apply =
             [](TrackEdits& edits, LegacyProject& project)
         {
             const auto count = project.clip.stepCount.value_or(0);
             const auto last = project.clip.firstFrame + (count - 1) * project.clip.stepSize;
             return count >= 2 && edits.setClipRange(project, project.clip.firstFrame + project.clip.stepSize, last, project.clip.stepSize);
         }},
        {.name = "clip stride",
         .apply =
             [](TrackEdits& edits, LegacyProject& project)
         {
             const auto count = project.clip.stepCount.value_or(0);
             const auto last = project.clip.firstFrame + (count - 1) * project.clip.stepSize;
             return count >= 3 && edits.setClipRange(project, project.clip.firstFrame, last, project.clip.stepSize * 2);
         }},
        {.name = "clip timing",
         .apply = [](TrackEdits& edits, LegacyProject& project)
         { return edits.setClipTiming(project, project.clip.frameDurationMs.value_or(100.0) * 2.0, project.clip.startTimeMs + 5.0); }},
        {.name = "calibrate",
         .apply =
             [](TrackEdits& edits, LegacyProject& project)
         {
             return edits.calibrateDistance(project,
                                            {.frame = project.clip.firstFrame,
                                             .imageDeltaX = 40.0,
                                             .imageDeltaY = 30.0,
                                             .knownLength = 2.5,
                                             .defaultOriginX = 0.0,
                                             .defaultOriginY = 0.0,
                                             .unit = "cm"});
         }},
        {.name = "axes",
         .apply =
             [](TrackEdits& edits, LegacyProject& project)
         {
             return edits.setCoordinateAxes(
                 project, {.frame = project.clip.firstFrame, .originX = 20.0, .originY = 30.0, .positiveX = 60.0, .positiveY = 10.0});
         }},
        {.name = "create mass",
         .apply =
             [](TrackEdits& edits, LegacyProject& project)
         {
             return edits.createPointMass(project, "created mass") &&
                    edits.markPointMass(project, project.pointMasses.size() - 1, {.frame = project.clip.firstFrame, .x = 3.0, .y = 4.0});
         }},
        {.name = "create empty mass",
         .apply = [](TrackEdits& edits, LegacyProject& project) { return edits.createPointMass(project, "empty mass"); }},
        {.name = "create vector",
         .apply =
             [](TrackEdits& edits, LegacyProject& project)
         {
             return edits.createVector(project, "created vector") &&
                    edits.markVector(
                        project,
                        project.vectors.size() - 1,
                        {.frame = project.clip.firstFrame, .tailX = 70.0, .tailY = 90.0, .componentX = 15.0, .componentY = 25.0});
         }},
        {.name = "remove mass", .apply = [](TrackEdits& edits, LegacyProject& project) { return edits.removePointMass(project, 0); }},
        {.name = "remove last mass",
         .apply = [](TrackEdits& edits, LegacyProject& project)
         { return project.pointMasses.size() >= 2 && edits.removePointMass(project, project.pointMasses.size() - 1); }},
        {.name = "remove vector", .apply = [](TrackEdits& edits, LegacyProject& project) { return edits.removeVector(project, 0); }},
        {.name = "rename and move",
         .apply =
             [](TrackEdits& edits, LegacyProject& project)
         {
             return edits.renamePointMass(project, 0, "moved and renamed") && edits.setPointMassValue(project, 0, 3.5) &&
                    edits.markPointMass(project, 0, {.frame = project.clip.firstFrame, .x = 1.0, .y = 2.0});
         }},
        {.name = "replace mass",
         .apply =
             [](TrackEdits& edits, LegacyProject& project)
         {
             const auto name = project.pointMasses.empty() ? std::string("mass") : project.pointMasses[0].name;
             return edits.removePointMass(project, 0) && edits.createPointMass(project, name);
         }},
        {.name = "edit and undo",
         .apply =
             [](TrackEdits& edits, LegacyProject& project)
         {
             return edits.renamePointMass(project, 0, "undone") && edits.setMassUnit(project, "mg") && edits.undo(project) &&
                    edits.undo(project);
         }},
    };
    return list;
}

/// The linked media of a corpus document, with decoded frame times consistent with its clip control for a movie.
struct ComposeMedia
{
    LinkedMedia kind = LinkedMedia::None;
    std::vector<double> frameTimes;
};

inline ComposeMedia composeMedia(const model::TrackerDocument& document, const LegacyProject& project)
{
    using model::Role;
    ComposeMedia media;
    if (project.clip.mediaReferences.empty())
        return media;
    const auto* video = document.singleton(Role::Video);
    if (!video || video->className != tracker::XUGGLE_VIDEO_CLASS)
    {
        media.kind = LinkedMedia::Images;
        return media;
    }
    media.kind = LinkedMedia::Movie;
    const auto* control = document.singleton(Role::ClipControl);
    const auto* timing = control ? std::get_if<model::ClipControlState>(control->value.get()) : nullptr;
    const auto* clip = document.singleton(Role::Clip);
    const auto* range = clip ? std::get_if<model::ClipState>(clip->value.get()) : nullptr;
    const auto duration = timing ? model::finiteValue(timing->deltaT).value_or(0.0) : 0.0;
    const auto frames = range && range->frameCount ? *range->frameCount : lastMarkFrame(project) + 1;
    for (int frame = 0; duration > 0.0 && frame < (std::min)(frames, 100000); ++frame)
        media.frameTimes.push_back(static_cast<double>(frame) * duration / 1000.0);
    return media;
}

/// The outcome of B1's composition, without the observer.
inline detail::CompositionOutcome legacyComposition(const LegacyProject& project, const TrackerSource& source, const ExportMedia& media)
{
    try
    {
        return detail::composeLegacyExport(project, source, media);
    }
    catch (const ProjectError& error)
    {
        return detail::CompositionRefusal{.message = error.what()};
    }
}

/// One scenario's comparison over one route: the media-free route, then the document's own linked-media route.
struct ScenarioComparison
{
    std::string_view scenario;
    LinkedMedia route = LinkedMedia::None;
    detail::CompositionComparison comparison;
};

/// Every applicable scenario of one imported document, compared over each route.
inline std::vector<ScenarioComparison> compareScenarios(const ImportedTrackerProject& imported, const model::TrackerDocument& typed)
{
    std::vector<ScenarioComparison> result;
    const auto media = composeMedia(typed, imported.project);
    for (const auto& scenario : composeScenarios())
    {
        auto project = imported.project;
        TrackEdits edits;
        if (!scenario.apply(edits, project))
            continue;
        for (const auto kind : {LinkedMedia::None, media.kind})
        {
            const ExportMedia route{.kind = kind,
                                    .frameTimes =
                                        kind == LinkedMedia::Movie ? std::span<const double>(media.frameTimes) : std::span<const double>{}};
            result.push_back({.scenario = scenario.name,
                              .route = kind,
                              .comparison = detail::compareWithDescriptorComposition(
                                  project, imported.source, route, legacyComposition(project, imported.source, route))});
            if (kind == media.kind)
                break;
        }
    }
    return result;
}

} // namespace motrack::test
