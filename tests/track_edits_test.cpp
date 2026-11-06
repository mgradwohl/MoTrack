#include "track_edits.hpp"

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

} // namespace
