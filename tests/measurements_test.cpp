#include "measurements.hpp"

#include <gtest/gtest.h>

#include <limits>

namespace
{

TEST(Calibration, UsesLatestKeyframeAndInvertsImageAxes)
{
    motrack::Calibration calibration;
    calibration.keyframes = {{.frame = 0, .originX = 10, .originY = 20, .angleDegrees = 0, .scaleX = 2, .scaleY = 4},
                             {.frame = 5, .originX = 10, .originY = 20, .angleDegrees = 90, .scaleX = 2, .scaleY = 4}};
    const auto first = motrack::worldPosition(calibration, {.frame = 4, .x = 14, .y = 24});
    if (!first)
    {
        FAIL() << "Missing world position before the second calibration keyframe.";
    }
    EXPECT_DOUBLE_EQ(first->x, 2);
    EXPECT_DOUBLE_EQ(first->y, -1);
    const auto second = motrack::worldPosition(calibration, {.frame = 5, .x = 14, .y = 24});
    if (!second)
    {
        FAIL() << "Missing world position at the second calibration keyframe.";
    }
    EXPECT_NEAR(second->x, -1, 1e-12);
    EXPECT_NEAR(second->y, -2, 1e-12);
}

TEST(Calibration, PreservesLegacyFixedCoordinateSemantics)
{
    motrack::Calibration calibration;
    calibration.fixedOrigin = true;
    calibration.fixedAngle = true;
    calibration.fixedScale = true;
    calibration.keyframes = {{.frame = 0, .originX = 0, .originY = 0, .angleDegrees = 90, .scaleX = 2, .scaleY = 4},
                             {.frame = 5, .originX = 10, .originY = 20, .angleDegrees = 0, .scaleX = 8, .scaleY = 8}};
    const auto point = motrack::worldPosition(calibration, {.frame = 1, .x = 14, .y = 24});
    if (!point)
    {
        FAIL() << "Missing fixed world position.";
    }
    EXPECT_DOUBLE_EQ(point->x, 2);
    EXPECT_DOUBLE_EQ(point->y, -1);
}

TEST(Calibration, SkipsUndefinedWorldPositions)
{
    motrack::Calibration calibration;
    EXPECT_FALSE(motrack::worldPosition(calibration, {.frame = 0, .x = 1, .y = 2}).has_value());
    calibration.keyframes = {{.frame = 0, .originX = 0, .originY = 0, .angleDegrees = 0, .scaleX = 0, .scaleY = 4}};
    EXPECT_FALSE(motrack::worldPosition(calibration, {.frame = 0, .x = 1, .y = 2}).has_value());
    calibration.keyframes.front().scaleX = std::numeric_limits<double>::min();
    EXPECT_FALSE(motrack::worldPosition(calibration, {.frame = 0, .x = 100, .y = 2}).has_value());
}

TEST(MeasurementsCsv, UsesClipFramesAndEscapesTextFields)
{
    motrack::LegacyProject project;
    project.clip = {.firstFrame = 2, .stepSize = 2, .stepCount = 2, .mediaReferences = {}};
    project.pointMasses.push_back({.name = "Ball, \"A\"",
                                   .marks = {{.frame = 0, .x = 1, .y = 2},
                                             {.frame = 2, .x = 1, .y = 2},
                                             {.frame = 3, .x = 1, .y = 2},
                                             {.frame = 4, .x = 1, .y = 2},
                                             {.frame = 6, .x = 1, .y = 2}}});
    project.calibration =
        motrack::Calibration{.keyframes = {{.frame = 0, .originX = 0, .originY = 4, .angleDegrees = 0, .scaleX = 2, .scaleY = 4}}};
    project.lengthUnit = "m";
    EXPECT_EQ(motrack::measurementsCsv(project),
              "track,frame,image_x_px,image_y_px,world_x,world_y,length_unit\r\n"
              "\"Ball, \"\"A\"\"\",2,1,2,0.5,0.5,m\r\n"
              "\"Ball, \"\"A\"\"\",4,1,2,0.5,0.5,m\r\n");
    project.calibration.reset();
    EXPECT_NE(motrack::measurementsCsv(project).find("\",2,1,2,,,\r\n"), std::string::npos);
}

} // namespace
