#include "track_edits.hpp"

#include "measurements.hpp"

#include <gtest/gtest.h>

#include <limits>

namespace
{

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
