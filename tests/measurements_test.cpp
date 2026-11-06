#include "measurements.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <limits>
#include <numbers>

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

TEST(MeasurementPlot, UsesRecordedTimingAndSkipsVelocityAcrossGaps)
{
    motrack::LegacyProject project;
    project.clip.firstFrame = 2;
    project.clip.stepSize = 2;
    project.clip.stepCount = 6;
    project.clip.frameDurationMs = 250;
    project.clip.startTimeMs = -1000;
    project.pointMasses.push_back({.name = "Cart",
                                   .marks = {{.frame = 2, .x = 0, .y = 1},
                                             {.frame = 4, .x = 2, .y = 2},
                                             {.frame = 6, .x = 4, .y = 3},
                                             {.frame = 10, .x = 8, .y = 5},
                                             {.frame = 12, .x = 10, .y = 6}}});
    const auto position = motrack::measurementPlot(project, 0, motrack::PlotQuantity::PositionY);
    ASSERT_EQ(position.samples.size(), 5u);
    EXPECT_TRUE(position.timed);
    EXPECT_FALSE(position.calibrated);
    EXPECT_DOUBLE_EQ(position.samples.front().horizontal, -1.0);
    EXPECT_DOUBLE_EQ(position.samples.back().vertical, 6.0);

    const auto velocityX = motrack::measurementPlot(project, 0, motrack::PlotQuantity::VelocityX);
    ASSERT_EQ(velocityX.samples.size(), 1u);
    EXPECT_EQ(velocityX.samples.front().frame, 4);
    EXPECT_DOUBLE_EQ(velocityX.samples.front().horizontal, -0.5);
    EXPECT_DOUBLE_EQ(velocityX.samples.front().vertical, 4.0);
    const auto velocityY = motrack::measurementPlot(project, 0, motrack::PlotQuantity::VelocityY);
    ASSERT_EQ(velocityY.samples.size(), 1u);
    EXPECT_DOUBLE_EQ(velocityY.samples.front().vertical, 2.0);

    project.clip.frameDurationMs.reset();
    const auto untimed = motrack::measurementPlot(project, 0, motrack::PlotQuantity::PositionX);
    ASSERT_EQ(untimed.samples.size(), 5u);
    EXPECT_FALSE(untimed.timed);
    EXPECT_DOUBLE_EQ(untimed.samples.front().horizontal, 2.0);
    EXPECT_TRUE(motrack::measurementPlot(project, 0, motrack::PlotQuantity::VelocityX).samples.empty());
    EXPECT_TRUE(motrack::measurementPlot(project, 1, motrack::PlotQuantity::PositionX).samples.empty());
}

TEST(MeasurementPlot, DifferentiatesAtTheCenterOfUnevenFrameTimes)
{
    motrack::LegacyProject project;
    project.clip.frameDurationMs = 250;
    project.pointMasses.push_back(
        {.name = "Cart",
         .marks = {
             {.frame = 0, .x = 0, .y = 1}, {.frame = 1, .x = 1, .y = 3}, {.frame = 2, .x = 9, .y = 7}, {.frame = 3, .x = 16, .y = 9}}});
    const std::array frameTimes{0.0, 1.0, 3.0};
    const auto velocity = motrack::measurementPlot(project, 0, motrack::PlotQuantity::VelocityX, frameTimes);
    ASSERT_EQ(velocity.samples.size(), 1u);
    EXPECT_DOUBLE_EQ(velocity.samples.front().horizontal, 1.0);
    EXPECT_DOUBLE_EQ(velocity.samples.front().vertical, 2.0);
    const auto speed = motrack::measurementPlot(project, 0, motrack::PlotQuantity::Speed, frameTimes);
    ASSERT_EQ(speed.samples.size(), 1u);
    EXPECT_NEAR(speed.samples.front().vertical, 2.0 * std::numbers::sqrt2, 1e-12);
    const auto accelerationX = motrack::measurementPlot(project, 0, motrack::PlotQuantity::AccelerationX, frameTimes);
    ASSERT_EQ(accelerationX.samples.size(), 1u);
    EXPECT_DOUBLE_EQ(accelerationX.samples.front().vertical, 2.0);
    const auto accelerationY = motrack::measurementPlot(project, 0, motrack::PlotQuantity::AccelerationY, frameTimes);
    ASSERT_EQ(accelerationY.samples.size(), 1u);
    EXPECT_DOUBLE_EQ(accelerationY.samples.front().vertical, 0.0);
}

TEST(LinearFit, RetainsPrecisionWithALargeHorizontalOffset)
{
    constexpr std::array SAMPLES{
        motrack::PlotSample{.frame = 0, .horizontal = 1e12, .vertical = 5},
        motrack::PlotSample{.frame = 1, .horizontal = 1e12 + 1, .vertical = 8},
        motrack::PlotSample{.frame = 2, .horizontal = 1e12 + 3, .vertical = 14},
        motrack::PlotSample{.frame = 3, .horizontal = 1e12 + 4, .vertical = std::numeric_limits<double>::quiet_NaN()}};
    const auto fit = motrack::fitStraightLine(SAMPLES);
    if (!fit || !fit->rSquared)
    {
        FAIL() << "Expected a fit with goodness of fit.";
    }
    EXPECT_NEAR(fit->slope, 3.0, 1e-4);
    EXPECT_NEAR(std::fma(fit->slope, 1e12, fit->intercept), 5.0, 1e-3);
    EXPECT_NEAR(fit->rSquared.value(), 1.0, 1e-4);
}

TEST(LinearFit, RejectsIndistinguishableXAndReportsConstantY)
{
    const std::array samples{motrack::PlotSample{.frame = 0, .horizontal = 1, .vertical = 7},
                             motrack::PlotSample{.frame = 1, .horizontal = 2, .vertical = 7}};
    EXPECT_FALSE(motrack::fitStraightLine({}));
    auto sameX = samples;
    sameX.back().horizontal = sameX.front().horizontal;
    EXPECT_FALSE(motrack::fitStraightLine(sameX));
    const auto fit = motrack::fitStraightLine(samples);
    if (!fit)
    {
        FAIL() << "Expected a horizontal fit.";
    }
    EXPECT_EQ(fit->slope, 0.0);
    EXPECT_EQ(fit->intercept, 7.0);
    EXPECT_FALSE(fit->rSquared.has_value());
}

} // namespace
