#include "track_edits.hpp"

#include "measurements.hpp"
#include "track_identity.hpp"

#include <gtest/gtest.h>

#include <array>
#include <functional>
#include <limits>

namespace
{

TEST(TrackEdits, PointMassCommandsFollowIdentityAfterTheTrackOrderChanges)
{
    motrack::LegacyProject project;
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.createPointMass(project, "A"));
    ASSERT_TRUE(edits.createPointMass(project, "B"));
    const auto a = project.pointMasses.front().id;
    const auto b = project.pointMasses.back().id;
    ASSERT_TRUE(edits.markPointMass(project, 1, {.frame = 3, .x = 99, .y = 99}));
    edits.clear();
    ASSERT_TRUE(edits.setPointMassValue(project, 0, 3.5));
    ASSERT_TRUE(edits.renamePointMass(project, 0, "Target"));
    ASSERT_TRUE(edits.markPointMass(project, 0, {.frame = 3, .x = 7, .y = 8}));
    std::swap(project.pointMasses.front(), project.pointMasses.back());
    ASSERT_TRUE(edits.undo(project));
    EXPECT_TRUE(project.pointMasses.back().marks.empty());
    EXPECT_EQ(project.pointMasses.front().marks.front().x, 99);
    ASSERT_TRUE(edits.undo(project));
    EXPECT_EQ(project.pointMasses.back().name, "A");
    ASSERT_TRUE(edits.undo(project));
    EXPECT_EQ(project.pointMasses.back().mass, 1.0);
    ASSERT_TRUE(edits.redo(project));
    ASSERT_TRUE(edits.redo(project));
    ASSERT_TRUE(edits.redo(project));
    EXPECT_EQ(project.pointMasses.back().id, a);
    EXPECT_EQ(project.pointMasses.back().marks.front().x, 7);
    EXPECT_EQ(project.pointMasses.back().mass, 3.5);
    EXPECT_EQ(project.pointMasses.back().name, "Target");
    ASSERT_TRUE(edits.removePointMass(project, 1));
    ASSERT_TRUE(edits.undo(project));
    std::swap(project.pointMasses.front(), project.pointMasses.back());
    ASSERT_TRUE(edits.redo(project));
    ASSERT_EQ(project.pointMasses.size(), 1u);
    EXPECT_EQ(project.pointMasses.front().id, b);
    edits.clear();
    ASSERT_TRUE(edits.createPointMass(project, "C"));
    const auto c = project.pointMasses.back().id;
    std::swap(project.pointMasses.front(), project.pointMasses.back());
    ASSERT_TRUE(edits.undo(project));
    ASSERT_EQ(project.pointMasses.size(), 1u);
    EXPECT_EQ(project.pointMasses.front().id, b);
    ASSERT_TRUE(edits.redo(project));
    EXPECT_EQ(project.pointMasses.back().id, c);
}

TEST(TrackEdits, VectorCommandsFollowIdentityAfterTheTrackOrderChanges)
{
    motrack::LegacyProject project;
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.createVector(project, "A"));
    ASSERT_TRUE(edits.createVector(project, "B"));
    const auto a = project.vectors.front().id;
    const auto b = project.vectors.back().id;
    ASSERT_TRUE(edits.markVector(project, 1, {.frame = 3, .tailX = 99, .tailY = 0, .componentX = 1, .componentY = 2}));
    edits.clear();
    ASSERT_TRUE(edits.renameVector(project, 0, "Target"));
    ASSERT_TRUE(edits.markVector(project, 0, {.frame = 3, .tailX = 7, .tailY = 8, .componentX = 1, .componentY = 2}));
    std::swap(project.vectors.front(), project.vectors.back());
    ASSERT_TRUE(edits.undo(project));
    EXPECT_TRUE(project.vectors.back().samples.empty());
    EXPECT_EQ(project.vectors.front().samples.front().tailX, 99);
    ASSERT_TRUE(edits.undo(project));
    EXPECT_EQ(project.vectors.back().name, "A");
    ASSERT_TRUE(edits.redo(project));
    ASSERT_TRUE(edits.redo(project));
    EXPECT_EQ(project.vectors.back().id, a);
    EXPECT_EQ(project.vectors.back().name, "Target");
    EXPECT_EQ(project.vectors.back().samples.front().tailX, 7);
    ASSERT_TRUE(edits.removeVector(project, 1));
    ASSERT_TRUE(edits.undo(project));
    std::swap(project.vectors.front(), project.vectors.back());
    ASSERT_TRUE(edits.redo(project));
    ASSERT_EQ(project.vectors.size(), 1u);
    EXPECT_EQ(project.vectors.front().id, b);
    edits.clear();
    ASSERT_TRUE(edits.createVector(project, "C"));
    const auto c = project.vectors.back().id;
    std::swap(project.vectors.front(), project.vectors.back());
    ASSERT_TRUE(edits.undo(project));
    ASSERT_EQ(project.vectors.size(), 1u);
    EXPECT_EQ(project.vectors.front().id, b);
    ASSERT_TRUE(edits.redo(project));
    EXPECT_EQ(project.vectors.back().id, c);
}

TEST(TrackEdits, RejectsMissingAmbiguousAndCollidingTargetsWithoutAdvancingHistory)
{
    motrack::LegacyProject project;
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.createPointMass(project, "A"));
    ASSERT_TRUE(edits.createPointMass(project, "B"));
    ASSERT_TRUE(edits.createVector(project, "V"));
    edits.clear();
    ASSERT_TRUE(edits.markPointMass(project, 0, {.frame = 2, .x = 7, .y = 8}));
    const auto revision = edits.revision();
    const auto a = project.pointMasses.front();
    project.pointMasses.erase(project.pointMasses.begin());
    EXPECT_FALSE(edits.undo(project));
    EXPECT_EQ(edits.revision(), revision);
    EXPECT_TRUE(edits.canUndo());
    EXPECT_TRUE(project.pointMasses.front().marks.empty());
    project.pointMasses.push_back(a);
    const auto vectorId = project.vectors.front().id;
    project.vectors.front().id = a.id;
    EXPECT_FALSE(edits.undo(project));
    EXPECT_EQ(edits.revision(), revision);
    project.vectors.front().id = vectorId;
    ASSERT_TRUE(edits.undo(project));
    project.pointMasses.back().id = 0;
    EXPECT_FALSE(edits.redo(project));
    EXPECT_FALSE(edits.renamePointMass(project, 1, "invalid"));
    EXPECT_TRUE(edits.canRedo());
    project.pointMasses.back().id = a.id;
    ASSERT_TRUE(edits.redo(project));
    EXPECT_EQ(project.pointMasses.back().marks.front().x, 7);
    ASSERT_TRUE(edits.removePointMass(project, 1));
    project.pointMasses.push_back(a); // Another owner already restored this identity.
    const auto removedRevision = edits.revision();
    EXPECT_FALSE(edits.undo(project));
    EXPECT_EQ(edits.revision(), removedRevision);
    project.pointMasses.pop_back();
    ASSERT_TRUE(edits.undo(project));
    edits.clear();
    const auto counter = project.nextTrackId;
    project.nextTrackId = vectorId;
    EXPECT_FALSE(edits.createPointMass(project, "collision"));
    EXPECT_FALSE(edits.createVector(project, "collision"));
    EXPECT_EQ(project.nextTrackId, vectorId);
    EXPECT_FALSE(edits.isModified());
    project.nextTrackId = counter;
    ASSERT_TRUE(edits.renameVector(project, 0, "renamed"));
    auto vector = project.vectors.front();
    project.vectors.clear();
    EXPECT_FALSE(edits.undo(project));
    project.vectors.push_back(vector);
    ASSERT_TRUE(edits.undo(project));
    EXPECT_EQ(project.vectors.front().name, "V");
}

TEST(TrackEdits, KeepsDistinctIdentitiesAcrossRenameDeleteUndoRedoAndNewHistory)
{
    motrack::LegacyProject project;
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.createPointMass(project, "same"));
    ASSERT_TRUE(edits.createPointMass(project, "same"));
    ASSERT_TRUE(edits.createVector(project, "same"));
    const auto first = project.pointMasses.at(0).id;
    const auto second = project.pointMasses.at(1).id;
    const auto vector = project.vectors.at(0).id;
    EXPECT_NE(first, second);
    EXPECT_NE(second, vector);
    EXPECT_NE(first, vector);
    ASSERT_TRUE(edits.renamePointMass(project, 1, "renamed"));
    ASSERT_TRUE(edits.removePointMass(project, 0));
    EXPECT_EQ(project.pointMasses.front().id, second);
    ASSERT_TRUE(edits.undo(project));
    EXPECT_EQ(project.pointMasses.at(0).id, first);
    EXPECT_EQ(project.pointMasses.at(1).id, second);
    ASSERT_TRUE(edits.redo(project));
    ASSERT_TRUE(edits.undo(project));
    ASSERT_TRUE(edits.undo(project)); // Rename.
    ASSERT_TRUE(edits.undo(project)); // Vector creation.
    const auto counter = project.nextTrackId;
    ASSERT_TRUE(edits.redo(project));
    EXPECT_EQ(project.vectors.front().id, vector);
    EXPECT_EQ(project.nextTrackId, counter);
    ASSERT_TRUE(edits.undo(project));
    ASSERT_TRUE(edits.createVector(project, "replacement"));
    EXPECT_GT(project.vectors.front().id, vector);
    EXPECT_FALSE(edits.canRedo());
    edits.clear();
    EXPECT_NO_THROW(motrack::validateTrackIdentities(project));
    EXPECT_GT(project.nextTrackId, project.vectors.front().id);
}

TEST(TrackEdits, ExhaustedIdentitySpaceRejectsCreationWithoutChangingHistory)
{
    motrack::LegacyProject project;
    project.nextTrackId = std::numeric_limits<motrack::TrackId>::max() - 1;
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.createPointMass(project, "last"));
    const auto revision = edits.revision();
    EXPECT_FALSE(edits.createPointMass(project, "overflow"));
    EXPECT_FALSE(edits.createVector(project, "overflow"));
    EXPECT_EQ(edits.revision(), revision);
    EXPECT_NO_THROW(motrack::validateTrackIdentities(project));
    EXPECT_THROW(static_cast<void>(motrack::allocateTrackId(project)), motrack::ProjectError);
    ASSERT_TRUE(edits.undo(project));
    EXPECT_FALSE(edits.createVector(project, "reused"));
    ASSERT_TRUE(edits.redo(project));
    EXPECT_EQ(project.pointMasses.front().id, std::numeric_limits<motrack::TrackId>::max() - 1);
    project.nextTrackId = 0;
    EXPECT_THROW(motrack::validateTrackIdentities(project), motrack::ProjectError);
}

TEST(TrackEdits, UndoingCreationStillNeedsToSaveTheAdvancedIdentityCounter)
{
    for (const bool vector : {false, true})
    {
        motrack::LegacyProject project;
        motrack::TrackEdits edits;
        edits.markSaved();
        ASSERT_TRUE(vector ? edits.createVector(project, "A") : edits.createPointMass(project, "A"));
        ASSERT_TRUE(edits.undo(project));
        EXPECT_TRUE(edits.isModified());
        EXPECT_EQ(project.nextTrackId, 2u);
        edits.markSaved();
        EXPECT_FALSE(edits.isModified());
        ASSERT_TRUE(edits.redo(project));
        EXPECT_TRUE(edits.isModified());
        ASSERT_TRUE(edits.undo(project));
        EXPECT_FALSE(edits.isModified()); // The saved counter is now the same.
    }
}

TEST(TrackEdits, ChangesRevisionForEditsUndoRedoAndDocumentReset)
{
    motrack::LegacyProject project;
    motrack::TrackEdits edits;
    const auto initial = edits.revision();
    edits.createPointMass(project, "A");
    const auto created = edits.revision();
    EXPECT_GT(created, initial);
    edits.markSaved();
    EXPECT_EQ(edits.revision(), created);
    EXPECT_FALSE(edits.removePointMass(project, 5));
    EXPECT_EQ(edits.revision(), created);
    ASSERT_TRUE(edits.undo(project));
    const auto undone = edits.revision();
    EXPECT_GT(undone, created);
    ASSERT_TRUE(edits.redo(project));
    EXPECT_GT(edits.revision(), undone);
    const auto redone = edits.revision();
    edits.clear();
    EXPECT_GT(edits.revision(), redone);
}

TEST(TrackEdits, InvalidatesASavedCheckpointWithoutChangingTheProjectRevision)
{
    motrack::LegacyProject project;
    motrack::TrackEdits edits;
    edits.createPointMass(project, "A");
    edits.markSaved();
    edits.createPointMass(project, "B");
    const auto revision = edits.revision();
    edits.invalidateSaved();
    EXPECT_EQ(edits.revision(), revision);
    ASSERT_TRUE(edits.undo(project));
    EXPECT_TRUE(edits.isModified());
    ASSERT_TRUE(edits.redo(project));
    EXPECT_TRUE(edits.isModified());
    edits.markSaved();
    EXPECT_FALSE(edits.isModified());
}

TEST(TrackEdits, EditsVectorsAndRestoresSamplesAndTrackOrder)
{
    motrack::LegacyProject project;
    motrack::TrackEdits edits;
    EXPECT_FALSE(edits.createVector(project, ""));
    ASSERT_TRUE(edits.createVector(project, "A"));
    ASSERT_TRUE(edits.createVector(project, "B"));
    EXPECT_FALSE(edits.createVector(project, "A"));
    EXPECT_FALSE(edits.renameVector(project, 4, "C"));
    EXPECT_FALSE(edits.renameVector(project, 0, ""));
    EXPECT_FALSE(edits.renameVector(project, 0, "B"));
    ASSERT_TRUE(edits.renameVector(project, 1, "C"));
    ASSERT_TRUE(edits.markVector(project, 0, {.frame = 2, .tailX = 10, .tailY = 20, .componentX = 3, .componentY = -4}));
    ASSERT_TRUE(edits.markVector(project, 0, {.frame = 0, .tailX = 1, .tailY = 2, .componentX = 0, .componentY = 0}));
    edits.markSaved();
    ASSERT_TRUE(edits.markVector(project, 0, {.frame = 0, .tailX = 1, .tailY = 2, .componentX = 0, .componentY = 0}));
    EXPECT_FALSE(edits.isModified());
    ASSERT_TRUE(edits.markVector(project, 0, {.frame = 0, .tailX = 2, .tailY = 3, .componentX = -5, .componentY = 6}));
    ASSERT_TRUE(edits.undo(project));
    EXPECT_FALSE(edits.isModified());
    EXPECT_DOUBLE_EQ(project.vectors.front().samples.front().tailX, 1);
    ASSERT_TRUE(edits.redo(project));
    EXPECT_DOUBLE_EQ(project.vectors.front().samples.front().componentX, -5);
    EXPECT_FALSE(edits.removeVectorSample(project, 8, 0));
    EXPECT_FALSE(edits.removeVectorSample(project, 0, 1));
    ASSERT_TRUE(edits.removeVectorSample(project, 0, 0));
    EXPECT_EQ(project.vectors.front().samples.front().frame, 2);
    ASSERT_TRUE(edits.undo(project));
    EXPECT_EQ(project.vectors.front().samples.front().frame, 0);
    EXPECT_FALSE(edits.removeVector(project, 8));
    ASSERT_TRUE(edits.removeVector(project, 0));
    EXPECT_EQ(project.vectors.front().name, "C");
    ASSERT_TRUE(edits.markVector(project, 0, {.frame = 9, .tailX = 0, .tailY = 0, .componentX = 1, .componentY = 1}));
    ASSERT_TRUE(edits.undo(project));
    ASSERT_TRUE(edits.undo(project));
    ASSERT_EQ(project.vectors.size(), 2u);
    EXPECT_EQ(project.vectors.front().name, "A");
    EXPECT_EQ(project.vectors.front().samples.size(), 2u);
    EXPECT_TRUE(project.vectors.back().samples.empty());
    while (edits.undo(project))
    {
    }
    EXPECT_TRUE(project.vectors.empty());
    while (edits.redo(project))
    {
    }
    ASSERT_EQ(project.vectors.size(), 1u);
    EXPECT_EQ(project.vectors.front().name, "C");
    EXPECT_EQ(project.vectors.front().samples.front().frame, 9);
}

TEST(TrackEdits, RejectsNonfiniteOrOverflowingVectorEndpointsWithoutAddingHistory)
{
    motrack::LegacyProject project;
    project.vectors = {{.name = "Vector", .samples = {}, .id = motrack::allocateTrackId(project)}};
    motrack::TrackEdits edits;
    const motrack::VectorSample valid{.frame = 0, .tailX = 1, .tailY = 2, .componentX = 3, .componentY = 4};
    EXPECT_FALSE(edits.markVector(project, 1, valid));
    auto invalid = valid;
    invalid.frame = -1;
    EXPECT_FALSE(edits.markVector(project, 0, invalid));
    for (auto member : {&motrack::VectorSample::tailX,
                        &motrack::VectorSample::tailY,
                        &motrack::VectorSample::componentX,
                        &motrack::VectorSample::componentY})
    {
        invalid = valid;
        invalid.*member = std::numeric_limits<double>::infinity();
        EXPECT_FALSE(edits.markVector(project, 0, invalid));
        invalid.*member = std::numeric_limits<double>::quiet_NaN();
        EXPECT_FALSE(edits.markVector(project, 0, invalid));
    }
    invalid = valid;
    invalid.tailX = invalid.componentX = std::numeric_limits<double>::max();
    EXPECT_FALSE(edits.markVector(project, 0, invalid));
    invalid = valid;
    invalid.tailY = invalid.componentY = std::numeric_limits<double>::max();
    EXPECT_FALSE(edits.markVector(project, 0, invalid));
    EXPECT_FALSE(edits.canUndo());
    EXPECT_FALSE(edits.isModified());
    EXPECT_TRUE(project.vectors.front().samples.empty());
}

TEST(TrackEdits, RestoresDeletedTracksAndMarksInTheirOriginalOrder)
{
    motrack::LegacyProject project;
    project.pointMasses = {{.name = "First", .marks = {{.frame = 0, .x = 3, .y = 4}}, .id = motrack::allocateTrackId(project)},
                           {.name = "Second", .marks = {{.frame = 2, .x = 7, .y = 8}}, .id = motrack::allocateTrackId(project)}};
    motrack::TrackEdits edits;
    EXPECT_FALSE(edits.renamePointMass(project, 0, "Second"));
    EXPECT_FALSE(edits.renamePointMass(project, 0, ""));
    EXPECT_FALSE(edits.removePointMass(project, 2));
    EXPECT_FALSE(edits.removeMark(project, 2, 0));
    EXPECT_FALSE(edits.removeMark(project, 0, 3));
    ASSERT_TRUE(edits.removeMark(project, 0, 0));
    ASSERT_TRUE(edits.renamePointMass(project, 1, "Target"));
    ASSERT_TRUE(edits.removePointMass(project, 0));
    ASSERT_EQ(project.pointMasses.size(), 1u);
    EXPECT_EQ(project.pointMasses.front().name, "Target");
    ASSERT_TRUE(edits.markPointMass(project, 0, {.frame = 3, .x = 10, .y = 10}));
    ASSERT_TRUE(edits.undo(project));
    ASSERT_TRUE(edits.undo(project));
    ASSERT_TRUE(edits.undo(project));
    ASSERT_TRUE(edits.undo(project));
    ASSERT_EQ(project.pointMasses.size(), 2u);
    EXPECT_EQ(project.pointMasses.front().name, "First");
    EXPECT_EQ(project.pointMasses.front().marks.front().frame, 0);
    EXPECT_EQ(project.pointMasses.back().name, "Second");
    EXPECT_EQ(project.pointMasses.back().marks.size(), 1u);
    ASSERT_TRUE(edits.redo(project));
    ASSERT_TRUE(edits.redo(project));
    ASSERT_TRUE(edits.redo(project));
    ASSERT_TRUE(edits.redo(project));
    ASSERT_EQ(project.pointMasses.size(), 1u);
    EXPECT_EQ(project.pointMasses.front().name, "Target");
    EXPECT_EQ(project.pointMasses.front().marks.back().frame, 3);
}

TEST(TrackEdits, ChangesMassWithUndoAndRejectsInvalidValues)
{
    motrack::LegacyProject project;
    motrack::TrackEdits edits;
    EXPECT_FALSE(edits.setPointMassValue(project, 0, 2.0));
    edits.createPointMass(project, "Cart");
    EXPECT_FALSE(edits.setPointMassValue(project, 0, 0.0));
    EXPECT_FALSE(edits.setPointMassValue(project, 0, std::numeric_limits<double>::infinity()));
    EXPECT_TRUE(edits.setPointMassValue(project, 0, 2.5));
    EXPECT_TRUE(edits.setPointMassValue(project, 0, 2.5));
    EXPECT_DOUBLE_EQ(project.pointMasses.front().mass, 2.5);
    EXPECT_TRUE(edits.undo(project));
    EXPECT_DOUBLE_EQ(project.pointMasses.front().mass, 1.0);
    EXPECT_TRUE(edits.redo(project));
    EXPECT_DOUBLE_EQ(project.pointMasses.front().mass, 2.5);
}

TEST(TrackEdits, ChangesTheClipWithoutRenumberingMarks)
{
    motrack::LegacyProject project;
    motrack::TrackEdits edits;
    EXPECT_FALSE(edits.setClipRange(project, 0, std::numeric_limits<int>::max(), 1));
    project.clip.sourceFrameCount = 12;
    project.pointMasses.push_back({.name = "Ball", .marks = {{.frame = 4, .x = 2, .y = 3}}, .id = motrack::allocateTrackId(project)});
    EXPECT_FALSE(edits.setClipRange(project, 7, 6, 1));
    EXPECT_FALSE(edits.setClipRange(project, 1, 12, 1));
    EXPECT_TRUE(edits.setClipRange(project, 1, 10, 3));
    EXPECT_EQ(project.clip.stepCount, 4);
    EXPECT_FALSE(edits.setClipRange(project, 1, 10, 3));
    EXPECT_EQ(project.pointMasses.front().marks.front().frame, 4);
    EXPECT_TRUE(edits.undo(project));
    EXPECT_EQ(project.clip.firstFrame, 0);
    EXPECT_FALSE(project.clip.stepCount.has_value());
    EXPECT_TRUE(edits.redo(project));
    EXPECT_EQ(project.clip.firstFrame, 1);
}

TEST(TrackEdits, ChangesTimingWithHistoryAndRescalesDerivedMotionAtOriginalFrames)
{
    motrack::LegacyProject project;
    project.clip.firstFrame = 2;
    project.clip.stepSize = 2;
    project.clip.stepCount = 3;
    project.pointMasses.push_back({.name = "Cart",
                                   .marks = {{.frame = 2, .x = 0, .y = 0}, {.frame = 4, .x = 2, .y = 0}, {.frame = 6, .x = 8, .y = 0}},
                                   .mass = 3,
                                   .id = motrack::allocateTrackId(project)});
    const auto original = project;
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.setClipTiming(project, 500, -500));
    edits.markSaved();
    const auto savedRevision = edits.revision();
    EXPECT_FALSE(edits.setClipTiming(project, 500, -500));
    EXPECT_EQ(edits.revision(), savedRevision);
    EXPECT_DOUBLE_EQ(motrack::measurementPlot(project, 0, motrack::PlotQuantity::VelocityX).samples.at(0).vertical, 4);
    ASSERT_TRUE(edits.setClipTiming(project, 250, -500));
    const std::array quantities{motrack::PlotQuantity::VelocityX,
                                motrack::PlotQuantity::AccelerationX,
                                motrack::PlotQuantity::MomentumX,
                                motrack::PlotQuantity::KineticEnergy};
    const std::array values{8.0, 16.0, 24.0, 96.0};
    for (std::size_t index = 0; index < quantities.size(); ++index)
    {
        const auto plot = motrack::measurementPlot(project, 0, quantities[index]);
        ASSERT_EQ(plot.samples.size(), 1u);
        EXPECT_DOUBLE_EQ(plot.samples[0].horizontal, 0);
        EXPECT_DOUBLE_EQ(plot.samples[0].vertical, values[index]);
    }
    EXPECT_EQ(project.pointMasses, original.pointMasses);
    EXPECT_EQ(motrack::clipRange(project.clip), motrack::clipRange(original.clip));
    ASSERT_TRUE(edits.undo(project));
    EXPECT_FALSE(edits.isModified());
    ASSERT_TRUE(edits.undo(project));
    EXPECT_EQ(project, original); // Undo restores an omitted duration, not an invented default.
    ASSERT_TRUE(edits.redo(project));
    ASSERT_TRUE(edits.redo(project));
    project.clip.mediaReferences = {"movie.mp4"};
    EXPECT_FALSE(edits.undo(project));
    EXPECT_TRUE(edits.canUndo());
    project.clip.mediaReferences.clear();
    ASSERT_TRUE(edits.undo(project));
    if (!project.clip.frameDurationMs)
    {
        FAIL() << "Undo lost the earlier duration";
    }
    EXPECT_DOUBLE_EQ(project.clip.frameDurationMs.value(), 500);
}

TEST(TrackEdits, RefusesInvalidOrUnrepresentableTimingWithoutHistoryChanges)
{
    motrack::LegacyProject project;
    project.clip.stepCount = 10;
    motrack::TrackEdits edits;
    const auto original = project;
    for (const auto duration : {0.0,
                                -1.0,
                                std::numeric_limits<double>::infinity(),
                                std::numeric_limits<double>::quiet_NaN(),
                                std::numeric_limits<double>::denorm_min()})
    {
        EXPECT_FALSE(edits.setClipTiming(project, duration, 0));
    }
    for (const auto origin : {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN(), 1e20})
    {
        EXPECT_FALSE(edits.setClipTiming(project, 1, origin));
    }
    EXPECT_FALSE(edits.setClipTiming(project, 1e308, 1e308));
    EXPECT_EQ(project, original);
    EXPECT_FALSE(edits.canUndo());
    const std::vector<std::function<void(motrack::Clip&)>> changes{[](auto& clip) { clip.stepCount.reset(); },
                                                                   [](auto& clip) { clip.stepCount = 0; },
                                                                   [](auto& clip) { clip.firstFrame = -1; },
                                                                   [](auto& clip) { clip.stepSize = 0; },
                                                                   [](auto& clip) { clip.stepCount = 500002; },
                                                                   [](auto& clip) { clip.sourceFrameCount = 3; },
                                                                   [](auto& clip)
                                                                   {
                                                                       clip.mediaReferences = {"frames/one.png"};
                                                                   }};
    for (const auto& change : changes)
    {
        project = original;
        change(project.clip);
        const auto before = project;
        EXPECT_FALSE(edits.setClipTiming(project, 250, -500));
        EXPECT_EQ(project, before);
        EXPECT_FALSE(edits.canUndo());
    }
    project = original;
    project.clip.firstFrame = 500000;
    project.clip.stepCount = 1;
    EXPECT_TRUE(edits.setClipTiming(project, 250, -500));
}

TEST(TrackEdits, PlacesAxesWithoutChangingAnisotropicScale)
{
    motrack::LegacyProject project;
    project.lengthUnit = "m";
    project.calibration =
        motrack::Calibration{.keyframes = {{.frame = 0, .originX = 0, .originY = 0, .angleDegrees = 0, .scaleX = 2, .scaleY = 4}},
                             .fixedOrigin = false,
                             .fixedAngle = true,
                             .fixedScale = true};
    motrack::TrackEdits edits;
    EXPECT_FALSE(edits.setCoordinateAxes(project, {.frame = 5, .originX = 10, .originY = 20, .positiveX = 10, .positiveY = 20}));
    ASSERT_TRUE(edits.setCoordinateAxes(project, {.frame = 5, .originX = 10, .originY = 20, .positiveX = 12, .positiveY = 16}));
    if (!project.calibration)
    {
        FAIL() << "Coordinate system was not recorded.";
    }
    const auto point = motrack::worldPosition(project.calibration.value(), {.frame = 5, .x = 12, .y = 16});
    if (!point)
    {
        FAIL() << "No world position at the edited frame.";
    }
    EXPECT_NEAR(point->x, 1.4142135623730951, 1e-12);
    EXPECT_NEAR(point->y, 0.0, 1e-12);
    EXPECT_EQ(project.lengthUnit, "m");
    EXPECT_EQ(project.calibration.value().keyframes.front().originX, 0.0);
    EXPECT_TRUE(edits.undo(project));
    if (!project.calibration)
    {
        FAIL() << "Undo did not restore the coordinate system.";
    }
    EXPECT_EQ(project.calibration.value().keyframes.size(), 1u);

    project.calibration.reset();
    ASSERT_TRUE(edits.setCoordinateAxes(project, {.frame = 0, .originX = 2, .originY = 2, .positiveX = 2, .positiveY = 1}));
    if (!project.calibration)
    {
        FAIL() << "Default coordinate system was not recorded.";
    }
    EXPECT_EQ(project.lengthUnit, "px");
    EXPECT_DOUBLE_EQ(project.calibration.value().keyframes.front().angleDegrees, 90.0);
}

TEST(TrackEdits, InsertsReplacesAndRestoresMarksInFrameOrder)
{
    motrack::LegacyProject project;
    motrack::TrackEdits edits;
    EXPECT_FALSE(edits.undo(project));
    EXPECT_FALSE(edits.redo(project));
    EXPECT_FALSE(edits.markPointMass(project, 0, {.frame = 0, .x = 1, .y = 2}));
    edits.createPointMass(project, "Ball");
    ASSERT_EQ(project.pointMasses.size(), 1u);
    EXPECT_EQ(project.pointMasses.front().name, "Ball");
    EXPECT_FALSE(edits.markPointMass(project, 0, {.frame = -1, .x = 1, .y = 2}));
    EXPECT_FALSE(edits.markPointMass(project, 0, {.frame = 0, .x = std::numeric_limits<double>::infinity(), .y = 2}));
    EXPECT_TRUE(edits.markPointMass(project, 0, {.frame = 2, .x = 3, .y = 4}));
    EXPECT_TRUE(edits.markPointMass(project, 0, {.frame = 0, .x = 1, .y = 2}));
    EXPECT_TRUE(edits.markPointMass(project, 0, {.frame = 0, .x = 7, .y = 8}));
    EXPECT_TRUE(edits.markPointMass(project, 0, {.frame = 0, .x = 7, .y = 8}));
    ASSERT_EQ(project.pointMasses.front().marks.size(), 2u);
    EXPECT_EQ(project.pointMasses.front().marks.front().frame, 0);
    EXPECT_DOUBLE_EQ(project.pointMasses.front().marks.front().x, 7);

    EXPECT_TRUE(edits.undo(project));
    EXPECT_DOUBLE_EQ(project.pointMasses.front().marks.front().x, 1);
    EXPECT_TRUE(edits.undo(project));
    ASSERT_EQ(project.pointMasses.front().marks.size(), 1u);
    EXPECT_EQ(project.pointMasses.front().marks.front().frame, 2);
    EXPECT_TRUE(edits.undo(project));
    EXPECT_TRUE(edits.undo(project));
    EXPECT_TRUE(project.pointMasses.empty());
    EXPECT_FALSE(edits.canUndo());

    EXPECT_TRUE(edits.redo(project));
    ASSERT_EQ(project.pointMasses.size(), 1u);
    EXPECT_TRUE(edits.redo(project));
    EXPECT_TRUE(edits.canRedo());
    EXPECT_TRUE(edits.markPointMass(project, 0, {.frame = 4, .x = 9, .y = 10}));
    EXPECT_FALSE(edits.canRedo());
    edits.clear();
    EXPECT_FALSE(edits.canUndo());
    EXPECT_FALSE(edits.canRedo());
    EXPECT_EQ(project.pointMasses.front().marks.size(), 2u);
}

TEST(TrackEdits, MassUnitHistoryInterleavesWithMassValuesWithoutRescaling)
{
    motrack::LegacyProject project;
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.createPointMass(project, "A"));
    ASSERT_TRUE(edits.setPointMassValue(project, 0, 2.5));
    edits.clear();
    const auto original = project;
    const auto revision = edits.revision();
    EXPECT_FALSE(edits.setMassUnit(project, ""));
    EXPECT_TRUE(edits.setMassUnit(project, project.massUnit));
    EXPECT_EQ(edits.revision(), revision);
    EXPECT_FALSE(edits.isModified());
    ASSERT_TRUE(edits.setMassUnit(project, "g"));
    EXPECT_EQ(project.massUnit, "g");
    EXPECT_EQ(project.pointMasses, original.pointMasses);
    edits.markSaved();
    ASSERT_TRUE(edits.setPointMassValue(project, 0, 4));
    ASSERT_TRUE(edits.setMassUnit(project, "mg"));
    ASSERT_TRUE(edits.undo(project));
    EXPECT_EQ(project.massUnit, "g");
    EXPECT_DOUBLE_EQ(project.pointMasses.front().mass, 4);
    ASSERT_TRUE(edits.undo(project));
    EXPECT_FALSE(edits.isModified());
    EXPECT_DOUBLE_EQ(project.pointMasses.front().mass, 2.5);
    ASSERT_TRUE(edits.undo(project));
    EXPECT_EQ(project, original);
    EXPECT_TRUE(edits.isModified());
    ASSERT_TRUE(edits.redo(project));
    EXPECT_FALSE(edits.isModified());
    ASSERT_TRUE(edits.redo(project));
    ASSERT_TRUE(edits.redo(project));
    EXPECT_EQ(project.massUnit, "mg");
    EXPECT_DOUBLE_EQ(project.pointMasses.front().mass, 4);
    ASSERT_TRUE(edits.undo(project));
    ASSERT_TRUE(edits.setMassUnit(project, "tonne"));
    EXPECT_FALSE(edits.canRedo());
}

TEST(TrackEdits, TimeLabelHistoryPreservesTimingMarksAndMassEdits)
{
    motrack::LegacyProject project;
    project.clip.frameDurationMs = 125;
    project.clip.startTimeMs = -750;
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.createPointMass(project, "A"));
    ASSERT_TRUE(edits.markPointMass(project, 0, {.frame = 2, .x = 4, .y = 5}));
    edits.clear();
    const auto original = project;
    const auto revision = edits.revision();
    EXPECT_FALSE(edits.setTimeUnit(project, ""));
    EXPECT_TRUE(edits.setTimeUnit(project, "s"));
    EXPECT_EQ(edits.revision(), revision);
    EXPECT_FALSE(edits.isModified());
    ASSERT_TRUE(edits.setTimeUnit(project, "tick"));
    EXPECT_EQ(project.clip, original.clip);
    EXPECT_EQ(project.pointMasses, original.pointMasses);
    edits.markSaved();
    ASSERT_TRUE(edits.setPointMassValue(project, 0, 2.5));
    ASSERT_TRUE(edits.setTimeUnit(project, "beat"));
    ASSERT_TRUE(edits.undo(project));
    EXPECT_EQ(project.timeUnit, "tick");
    EXPECT_DOUBLE_EQ(project.pointMasses[0].mass, 2.5);
    ASSERT_TRUE(edits.undo(project));
    EXPECT_FALSE(edits.isModified());
    ASSERT_TRUE(edits.undo(project));
    EXPECT_EQ(project, original);
    ASSERT_TRUE(edits.redo(project));
    EXPECT_FALSE(edits.isModified());
    ASSERT_TRUE(edits.redo(project));
    ASSERT_TRUE(edits.redo(project));
    EXPECT_EQ(project.timeUnit, "beat");
    EXPECT_EQ(project.clip, original.clip);
    ASSERT_TRUE(edits.undo(project));
    ASSERT_TRUE(edits.setTimeUnit(project, "cycle"));
    EXPECT_FALSE(edits.canRedo());
}

TEST(TrackEdits, CreatesDistanceCalibrationAndRestoresThePriorUnit)
{
    motrack::LegacyProject project;
    project.lengthUnit = "m";
    motrack::TrackEdits edits;
    motrack::CalibrationDistance distance{
        .frame = 0, .imageDeltaX = 8, .imageDeltaY = 0, .knownLength = 2, .defaultOriginX = 10, .defaultOriginY = 20, .unit = "cm"};
    EXPECT_TRUE(edits.calibrateDistance(project, distance));
    EXPECT_EQ(project.lengthUnit, "cm");
    if (!project.calibration)
    {
        FAIL() << "Calibration was not created.";
    }
    const auto point = motrack::worldPosition(*project.calibration, {.frame = 0, .x = 18, .y = 16});
    if (!point)
    {
        FAIL() << "Calibrated mark was not transformed.";
    }
    EXPECT_DOUBLE_EQ(point->x, 2);
    EXPECT_DOUBLE_EQ(point->y, 1);
    EXPECT_TRUE(edits.undo(project));
    EXPECT_FALSE(project.calibration.has_value());
    EXPECT_EQ(project.lengthUnit, "m");
    EXPECT_TRUE(edits.redo(project));
    EXPECT_EQ(project.lengthUnit, "cm");
}

TEST(TrackEdits, ChangesScaleAtTheSelectedKeyframeAndPreservesAnisotropy)
{
    motrack::LegacyProject project;
    project.calibration =
        motrack::Calibration{.keyframes = {{.frame = 0, .originX = 10, .originY = 20, .angleDegrees = 15, .scaleX = 2, .scaleY = 4},
                                           {.frame = 10, .originX = 10, .originY = 20, .angleDegrees = 15, .scaleX = 8, .scaleY = 16}}};
    motrack::TrackEdits edits;
    motrack::CalibrationDistance distance{
        .frame = 5, .imageDeltaX = 8, .imageDeltaY = 0, .knownLength = 2, .defaultOriginX = 0, .defaultOriginY = 0, .unit = "m"};
    EXPECT_TRUE(edits.calibrateDistance(project, distance));
    distance.knownLength = 1;
    EXPECT_TRUE(edits.calibrateDistance(project, distance));
    if (!project.calibration)
    {
        FAIL() << "Calibration was lost.";
    }
    const auto& keys = project.calibration->keyframes;
    ASSERT_EQ(keys.size(), 3u);
    EXPECT_EQ(keys.at(1).frame, 5);
    EXPECT_DOUBLE_EQ(keys.at(1).scaleX, 8);
    EXPECT_DOUBLE_EQ(keys.at(1).scaleY, 16);
    EXPECT_DOUBLE_EQ(keys.at(1).angleDegrees, 15);
    EXPECT_DOUBLE_EQ(keys.front().scaleX, 2);
    EXPECT_DOUBLE_EQ(keys.back().scaleX, 8);
}

TEST(TrackEdits, RejectsDegenerateCalibrationDistances)
{
    motrack::LegacyProject project;
    motrack::TrackEdits edits;
    motrack::CalibrationDistance distance{
        .frame = 0, .imageDeltaX = 0, .imageDeltaY = 0, .knownLength = 1, .defaultOriginX = 0, .defaultOriginY = 0, .unit = "m"};
    EXPECT_FALSE(edits.calibrateDistance(project, distance));
    distance.knownLength = 0;
    EXPECT_FALSE(edits.calibrateDistance(project, distance));
    EXPECT_FALSE(edits.canUndo());
    EXPECT_FALSE(project.calibration.has_value());
}

} // namespace
