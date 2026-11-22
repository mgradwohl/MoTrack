#include "measurements.hpp"

#include "science/motion/motion_derivatives.hpp"
#include "science/motion/motion_types.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <span>
#include <vector>

namespace
{

TEST(ClipMapping, MapsOriginalFramesAndStepsWithinTheClip)
{
    const motrack::Clip clip{.firstFrame = 4, .stepSize = 3, .stepCount = 3};
    EXPECT_EQ(motrack::clipStepOfFrame(clip, 4), 0);
    EXPECT_EQ(motrack::clipStepOfFrame(clip, 10), 2);
    for (const auto frame : {3, 5, 13, -2})
    {
        EXPECT_EQ(motrack::clipStepOfFrame(clip, frame), std::nullopt) << frame;
    }
    EXPECT_EQ(motrack::clipFrameOfStep(clip, 0), 4);
    EXPECT_EQ(motrack::clipFrameOfStep(clip, 2), 10);
    EXPECT_EQ(motrack::clipFrameOfStep(clip, 3), std::nullopt);
    EXPECT_EQ(motrack::clipFrameOfStep(clip, -1), std::nullopt);

    const motrack::Clip open{.firstFrame = 1, .stepSize = 2};
    EXPECT_EQ(motrack::clipStepOfFrame(open, 1001), 500);
    EXPECT_EQ(motrack::clipFrameOfStep(open, 500), 1001);
    EXPECT_EQ(motrack::clipFrameOfStep(open, std::numeric_limits<int>::max()), std::nullopt);
    for (const motrack::Clip& invalid : {motrack::Clip{.firstFrame = -1}, motrack::Clip{.stepSize = 0}})
    {
        EXPECT_EQ(motrack::clipStepOfFrame(invalid, 0), std::nullopt);
        EXPECT_EQ(motrack::clipFrameOfStep(invalid, 0), std::nullopt);
    }
}

TEST(ClipMapping, TimesFramesOnlyWithAPositiveSavedDuration)
{
    motrack::Clip clip{.firstFrame = 2, .stepSize = 1, .startTimeMs = -250.0};
    EXPECT_EQ(motrack::clipFrameTimeMs(clip, 2), std::nullopt);
    for (const auto duration : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN()})
    {
        clip.frameDurationMs = duration;
        EXPECT_EQ(motrack::clipFrameTimeMs(clip, 3), std::nullopt) << duration;
    }
    clip.frameDurationMs = 125.0;
    EXPECT_EQ(motrack::clipFrameTimeMs(clip, 2), -250.0);
    EXPECT_EQ(motrack::clipFrameTimeMs(clip, 6), 250.0);
    EXPECT_EQ(motrack::clipFrameTimeMs(clip, 0), -500.0); // Defined before the clip too: it is the saved timebase.
}

TEST(Measurements, EscapesPlotLabelsAndExportsUntimedSamplesByFrame)
{
    const motrack::PlotSeries series{.samples = {{.frame = 3, .horizontal = 3.0, .vertical = 1.5}}};
    EXPECT_EQ(motrack::plotCsv(series, "position, \"x\""), "frame,\"position, \"\"x\"\"\"\r\n3,1.5\r\n");
}

TEST(Measurements, TimeLabelsDoNotRescaleMotionAndAreEscapedInCsvHeaders)
{
    motrack::LegacyProject project;
    project.clip.frameDurationMs = 500;
    project.clip.startTimeMs = -500;
    project.pointMasses.push_back(
        {.name = "A", .marks = {{.frame = 0, .x = 0, .y = 0}, {.frame = 1, .x = 2, .y = 0}, {.frame = 2, .x = 8, .y = 0}}, .mass = 3});
    const std::array quantities{motrack::PlotQuantity::VelocityX, motrack::PlotQuantity::MomentumX, motrack::PlotQuantity::KineticEnergy};
    const std::array expected{8.0, 24.0, 96.0};
    project.timeUnit = "tick, beat";
    // Tracker's five-point acceleration needs five consecutive marks (#207); three leave it blank.
    EXPECT_TRUE(motrack::measurementPlot(project, 0, motrack::PlotQuantity::AccelerationX).samples.empty());
    for (std::size_t index = 0; index < quantities.size(); ++index)
    {
        const auto series = motrack::measurementPlot(project, 0, quantities[index]);
        ASSERT_EQ(series.samples.size(), 1u);
        EXPECT_EQ(series.timeUnit, project.timeUnit);
        EXPECT_DOUBLE_EQ(series.samples[0].horizontal, 0.0);
        EXPECT_DOUBLE_EQ(series.samples[0].vertical, expected[index]);
        EXPECT_TRUE(motrack::plotCsv(series, "value").starts_with("frame,\"time_tick, beat\",value\r\n"));
    }
    const auto positions = motrack::measurementPlot(project, 0, motrack::PlotQuantity::PositionX);
    ASSERT_EQ(positions.samples.size(), 3u);
    EXPECT_DOUBLE_EQ(positions.samples.front().horizontal, -0.5);
    EXPECT_DOUBLE_EQ(positions.samples.back().horizontal, 0.5);
    project.timeUnit = "s";
    EXPECT_TRUE(
        motrack::plotCsv(motrack::measurementPlot(project, 0, motrack::PlotQuantity::PositionX), "x").starts_with("frame,time_s,x\r\n"));
}

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

TEST(SpatialMeasurement, MeasuresDistanceAndInteriorAngles)
{
    const motrack::WorldPoint origin{.x = 0.0, .y = 0.0};
    const motrack::WorldPoint right{.x = 3.0, .y = 0.0};
    const motrack::WorldPoint up{.x = 0.0, .y = 4.0};
    EXPECT_EQ(motrack::pointDistance(right, up), 5.0);
    EXPECT_EQ(motrack::interiorAngleDegrees(origin, right, up), 90.0);
    EXPECT_EQ(motrack::interiorAngleDegrees(origin, right, {.x = -3.0, .y = 0.0}), 180.0);
    EXPECT_FALSE(motrack::interiorAngleDegrees(origin, origin, up));
    EXPECT_FALSE(motrack::pointDistance(origin, {.x = std::numeric_limits<double>::infinity(), .y = 0.0}));
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
    const auto shifted = motrack::measurementPlot(project, 0, motrack::PlotQuantity::VelocityX, frameTimes, -0.75);
    ASSERT_EQ(shifted.samples.size(), 1u);
    EXPECT_DOUBLE_EQ(shifted.samples.front().horizontal, 0.25);
    EXPECT_DOUBLE_EQ(shifted.samples.front().vertical, 2.0);
    const auto speed = motrack::measurementPlot(project, 0, motrack::PlotQuantity::Speed, frameTimes);
    ASSERT_EQ(speed.samples.size(), 1u);
    EXPECT_NEAR(speed.samples.front().vertical, 2.0 * std::numbers::sqrt2, 1e-12);
    // Three timed samples have no five-step acceleration window, as in Tracker (#207).
    EXPECT_TRUE(motrack::measurementPlot(project, 0, motrack::PlotQuantity::AccelerationX, frameTimes).samples.empty());
    EXPECT_TRUE(motrack::measurementPlot(project, 0, motrack::PlotQuantity::AccelerationY, frameTimes).samples.empty());
    project.pointMasses.front().mass = 4.0;
    const auto momentumX = motrack::measurementPlot(project, 0, motrack::PlotQuantity::MomentumX, frameTimes);
    ASSERT_EQ(momentumX.samples.size(), 1u);
    EXPECT_DOUBLE_EQ(momentumX.samples.front().vertical, 8.0);
    const auto momentumY = motrack::measurementPlot(project, 0, motrack::PlotQuantity::MomentumY, frameTimes);
    ASSERT_EQ(momentumY.samples.size(), 1u);
    EXPECT_DOUBLE_EQ(momentumY.samples.front().vertical, 8.0);
    const auto energy = motrack::measurementPlot(project, 0, motrack::PlotQuantity::KineticEnergy, frameTimes);
    ASSERT_EQ(energy.samples.size(), 1u);
    EXPECT_NEAR(energy.samples.front().vertical, 16.0, 1e-12);
}

/// The plotted samples of a series by original frame.
std::map<int, double> valuesByFrame(const motrack::PlotSeries& series)
{
    std::map<int, double> values;
    for (const auto& sample : series.samples)
    {
        values.emplace(sample.frame, sample.vertical);
    }
    return values;
}

/// A media-free point mass with one mark per value, on consecutive clip steps from frame 0.
motrack::LegacyProject consecutiveMarks(std::span<const double> x, std::span<const double> y, double frameDurationMs)
{
    motrack::LegacyProject project;
    project.clip.stepCount = static_cast<int>(x.size());
    project.clip.frameDurationMs = frameDurationMs;
    motrack::PointMass mass{.name = "A"};
    for (std::size_t step = 0; step < x.size(); ++step)
    {
        mass.marks.push_back({.frame = static_cast<int>(step), .x = x[step], .y = y[step]});
    }
    project.pointMasses.push_back(mass);
    return project;
}

TEST(MeasurementPlot, AccelerationUsesTrackersFiveStepWindowForThreeToSevenMarks)
{
    // Noisy x and linear y at a 0.5 s step. Tracker's Finite Difference (#207): v = (x[k+1] - x[k-1]) / (2 dt)
    // and a = (2x[k-2] - x[k-1] - 2x[k] - x[k+1] + 2x[k+2]) / (7 dt²). Worked by hand: at k = 2 of
    // 0, 3, 2, 7, 8 it is (0 - 3 - 4 - 7 + 16) / 7 / 0.25 = 8/7, where a three-point estimate gives 24.
    constexpr std::array X{0.0, 3.0, 2.0, 7.0, 8.0, 15.0, 14.0};
    constexpr std::array Y{1.0, 0.0, -1.0, -2.0, -3.0, -4.0, -5.0};
    struct Expected
    {
        std::size_t marks;
        std::map<int, double> velocity;
        std::map<int, double> acceleration;
    };
    const std::array cases{
        Expected{.marks = 3, .velocity = {{1, 2.0}}, .acceleration = {}},
        Expected{.marks = 4, .velocity = {{1, 2.0}, {2, 4.0}}, .acceleration = {}},
        Expected{.marks = 5, .velocity = {{1, 2.0}, {2, 4.0}, {3, 6.0}}, .acceleration = {{2, 8.0 / 7.0}}},
        Expected{.marks = 7,
                 .velocity = {{1, 2.0}, {2, 4.0}, {3, 6.0}, {4, 8.0}, {5, 6.0}},
                 .acceleration = {{2, 8.0 / 7.0}, {3, 48.0 / 7.0}, {4, -24.0 / 7.0}}},
    };
    for (const auto& expected : cases)
    {
        const auto project = consecutiveMarks(std::span(X).first(expected.marks), std::span(Y).first(expected.marks), 500.0);
        const auto velocity = valuesByFrame(motrack::measurementPlot(project, 0, motrack::PlotQuantity::VelocityX));
        const auto acceleration = valuesByFrame(motrack::measurementPlot(project, 0, motrack::PlotQuantity::AccelerationX));
        ASSERT_EQ(velocity.size(), expected.velocity.size()) << expected.marks;
        ASSERT_EQ(acceleration.size(), expected.acceleration.size()) << expected.marks;
        for (const auto& [frame, value] : expected.velocity)
        {
            EXPECT_DOUBLE_EQ(velocity.at(frame), value) << expected.marks << " marks, frame " << frame;
        }
        for (const auto& [frame, value] : expected.acceleration)
        {
            EXPECT_DOUBLE_EQ(acceleration.at(frame), value) << expected.marks << " marks, frame " << frame;
        }
        // A zero acceleration is a measured value at the same steps, not a substitute for a missing one.
        const auto accelerationY = motrack::measurementPlot(project, 0, motrack::PlotQuantity::AccelerationY);
        ASSERT_EQ(accelerationY.samples.size(), expected.acceleration.size()) << expected.marks;
        for (const auto& sample : accelerationY.samples)
        {
            EXPECT_TRUE(expected.acceleration.contains(sample.frame));
            EXPECT_EQ(sample.vertical, 0.0);
            EXPECT_DOUBLE_EQ(sample.horizontal, 0.5 * sample.frame);
        }
    }
}

TEST(MeasurementPlot, FivePointAccelerationAttenuatesAlternatingNoise)
{
    // Free fall at 30 fps with ±1 mm alternating noise. Both stencils are exact for the parabola; on the
    // noise (-1)^k ε the five-point parabola gives (4/7)(-1)^k ε / dt², the three-point difference
    // -4 (-1)^k ε / dt² (seven times larger, opposite sign; 4.1 m/s² apart here).
    constexpr double STEP = 1.0 / 30.0;
    constexpr double NOISE = 0.001;
    std::vector<double> x;
    std::vector<double> y;
    for (int step = 0; step < 12; ++step)
    {
        const double t = step * STEP;
        x.push_back(0.5 + 1.25 * t);
        y.push_back(2.0 - 4.9 * t * t + (step % 2 == 0 ? NOISE : -NOISE));
    }
    const auto project = consecutiveMarks(x, y, 1000.0 / 30.0);
    const auto accelerationX = valuesByFrame(motrack::measurementPlot(project, 0, motrack::PlotQuantity::AccelerationX));
    const auto accelerationY = valuesByFrame(motrack::measurementPlot(project, 0, motrack::PlotQuantity::AccelerationY));
    ASSERT_EQ(accelerationY.size(), 8u); // Steps 2..9.
    ASSERT_EQ(accelerationX.size(), 8u);
    for (const auto& [frame, value] : accelerationY)
    {
        EXPECT_GE(frame, 2);
        EXPECT_LE(frame, 9);
        const double sign = frame % 2 == 0 ? 1.0 : -1.0;
        EXPECT_NEAR(value, -9.8 + sign * (4.0 / 7.0) * NOISE / (STEP * STEP), 1e-9) << frame;
        EXPECT_NEAR(accelerationX.at(frame), 0.0, 1e-9) << frame;
    }
}

TEST(MeasurementPlot, AGapOfOneStepRemovesEveryWindowThatContainsIt)
{
    // Eleven clip steps from frame 4 at stride 2 and 100 ms frames (0.2 s per step), step 5 unmarked.
    // Free fall: acceleration only at the complete five-step windows (steps 2 and 8); velocity at steps
    // whose neighbours are both marked (1-3 and 7-9).
    motrack::LegacyProject project;
    project.clip = {.firstFrame = 4, .stepSize = 2, .stepCount = 11, .frameDurationMs = 100.0, .startTimeMs = 0.0};
    motrack::PointMass mass{.name = "ball"};
    for (int step = 0; step < 11; ++step)
    {
        if (step != 5)
        {
            const double t = 0.2 * step;
            mass.marks.push_back({.frame = 4 + 2 * step, .x = 3.0 * t, .y = -4.9 * t * t});
        }
    }
    project.pointMasses.push_back(mass);
    const auto acceleration = valuesByFrame(motrack::measurementPlot(project, 0, motrack::PlotQuantity::AccelerationY));
    ASSERT_EQ(acceleration.size(), 2u);
    EXPECT_NEAR(acceleration.at(8), -9.8, 1e-9);
    EXPECT_NEAR(acceleration.at(20), -9.8, 1e-9);
    const auto velocity = valuesByFrame(motrack::measurementPlot(project, 0, motrack::PlotQuantity::VelocityY));
    ASSERT_EQ(velocity.size(), 6u);
    for (const auto step : {1, 2, 3, 7, 8, 9})
    {
        EXPECT_NEAR(velocity.at(4 + 2 * step), -9.8 * 0.2 * step, 1e-9) << step;
    }
    EXPECT_EQ(motrack::measurementPlot(project, 0, motrack::PlotQuantity::Speed).samples.size(), 6u);
}

TEST(MeasurementPlot, ClipEdgesAndMarksOutsideTheClipLeaveDerivativesBlank)
{
    // Clip frames 3, 5, ..., 13 (stride 2, 50 ms frames, so 0.1 s per step) with x = k²/2 at step k: per step
    // v = k and a = 1, so v = 10k and a = 100 per second. Marks before, between and after the clip steps carry
    // values that would change any window they joined.
    motrack::LegacyProject project;
    project.clip = {.firstFrame = 3, .stepSize = 2, .stepCount = 6, .frameDurationMs = 50.0, .startTimeMs = 250.0};
    motrack::PointMass mass{.name = "cart"};
    mass.marks.push_back({.frame = 1, .x = 1000.0, .y = 0.0});
    mass.marks.push_back({.frame = 4, .x = 1000.0, .y = 0.0});
    for (int step = 0; step < 6; ++step)
    {
        mass.marks.push_back({.frame = 3 + 2 * step, .x = 0.5 * step * step, .y = 0.0});
    }
    mass.marks.push_back({.frame = 15, .x = 1000.0, .y = 0.0});
    project.pointMasses.push_back(mass);
    const auto acceleration = motrack::measurementPlot(project, 0, motrack::PlotQuantity::AccelerationX);
    ASSERT_EQ(acceleration.samples.size(), 2u);
    EXPECT_EQ(acceleration.samples.at(0).frame, 7);
    EXPECT_EQ(acceleration.samples.at(1).frame, 9);
    for (const auto& sample : acceleration.samples)
    {
        EXPECT_NEAR(sample.vertical, 100.0, 1e-9);
        EXPECT_DOUBLE_EQ(sample.horizontal, (250.0 + (sample.frame - 3) * 50.0) / 1000.0);
    }
    const auto velocity = valuesByFrame(motrack::measurementPlot(project, 0, motrack::PlotQuantity::VelocityX));
    ASSERT_EQ(velocity.size(), 4u);
    for (const auto step : {1, 2, 3, 4})
    {
        EXPECT_NEAR(velocity.at(3 + 2 * step), 10.0 * step, 1e-9) << step;
    }
}

TEST(MeasurementPlot, KeepsTimeAwareEstimatesForUnequalDecodedFrameIntervals)
{
    // Decoded frame times 0, 1, 3, 4, 6 s and x = t², y = 2t. For unequal intervals MoTrack keeps the
    // derivatives of the quadratic through each step and its neighbours (#87), exact here: v_x = 2t and
    // a_x = 2. Tracker's uniform stencils would report v_x = 3, 5, 9 and a_x = (37/7) / 1.5² ≈ 2.35.
    // Acceleration still needs Tracker's five-step window, so only step 2 has one.
    const std::array times{0.0, 1.0, 3.0, 4.0, 6.0};
    motrack::LegacyProject project;
    motrack::PointMass mass{.name = "A"};
    for (int frame = 0; frame < 5; ++frame)
    {
        const double t = times.at(static_cast<std::size_t>(frame));
        mass.marks.push_back({.frame = frame, .x = t * t, .y = 2.0 * t});
    }
    project.pointMasses.push_back(mass);
    const auto velocityX = valuesByFrame(motrack::measurementPlot(project, 0, motrack::PlotQuantity::VelocityX, times));
    const auto velocityY = valuesByFrame(motrack::measurementPlot(project, 0, motrack::PlotQuantity::VelocityY, times));
    ASSERT_EQ(velocityX.size(), 3u);
    ASSERT_EQ(velocityY.size(), 3u);
    EXPECT_DOUBLE_EQ(velocityX.at(1), 2.0);
    EXPECT_DOUBLE_EQ(velocityX.at(2), 6.0);
    EXPECT_DOUBLE_EQ(velocityX.at(3), 8.0);
    for (const auto& [frame, value] : velocityY)
    {
        EXPECT_DOUBLE_EQ(value, 2.0) << frame;
    }
    const auto accelerationX = motrack::measurementPlot(project, 0, motrack::PlotQuantity::AccelerationX, times);
    ASSERT_EQ(accelerationX.samples.size(), 1u);
    EXPECT_EQ(accelerationX.samples.front().frame, 2);
    EXPECT_DOUBLE_EQ(accelerationX.samples.front().horizontal, 3.0);
    EXPECT_DOUBLE_EQ(accelerationX.samples.front().vertical, 2.0);
    const auto accelerationY = motrack::measurementPlot(project, 0, motrack::PlotQuantity::AccelerationY, times);
    ASSERT_EQ(accelerationY.samples.size(), 1u);
    EXPECT_EQ(accelerationY.samples.front().vertical, 0.0);

    // True variable frame rate: steady motion captured at the recorded, unevenly spaced times (positions follow
    // those times, x = 2t). The time-aware estimate gives no acceleration, where Tracker's stencil reports about
    // -0.26 m/s² at step 2. Constant-rate capture with coarse timestamps is the opposite case (next test).
    const std::array recorded{0.0, 0.033, 0.067, 0.1, 0.133, 0.167};
    motrack::LegacyProject steady;
    motrack::PointMass cart{.name = "cart"};
    for (int frame = 0; frame < 6; ++frame)
    {
        cart.marks.push_back({.frame = frame, .x = 2.0 * recorded.at(static_cast<std::size_t>(frame)), .y = 0.0});
    }
    steady.pointMasses.push_back(cart);
    const auto trackerStencil = (2 * cart.marks[0].x - cart.marks[1].x - 2 * cart.marks[2].x - cart.marks[3].x + 2 * cart.marks[4].x) / 7 /
                                std::pow((recorded[4] - recorded[0]) / 4, 2);
    EXPECT_LT(trackerStencil, -0.2);
    const auto steadyAcceleration = motrack::measurementPlot(steady, 0, motrack::PlotQuantity::AccelerationX, recorded);
    ASSERT_EQ(steadyAcceleration.samples.size(), 2u);
    for (const auto& sample : steadyAcceleration.samples)
    {
        EXPECT_NEAR(sample.vertical, 0.0, 1e-9) << sample.frame;
    }
}

TEST(MeasurementPlot, ConstantRateCaptureWithMillisecondTimestampsKeepsTheTimeAwareError)
{
    // KNOWN LIMITATION, pinned so that the #87 follow-up notices the change. Constant-rate capture whose
    // decoded times are whole milliseconds (the Matroska/WebM time base): positions follow the true times,
    // x = 2 m/s × k / rate, but the 33/34 ms intervals count as unequal, so MoTrack's time-aware estimates
    // turn the timestamp rounding into acceleration and a 1–2% velocity error. Tracker's uniform stencils,
    // given the same inputs, are accurate to about 1e-13. Values over one second (31 frames, steady 2 m/s).
    struct Rate
    {
        double numerator;
        double denominator;
        double maximumAcceleration;
        double maximumVelocityError;
    };
    for (const auto& rate :
         {Rate{.numerator = 1.0, .denominator = 30.0, .maximumAcceleration = 1.773663, .maximumVelocityError = 0.010101},
          Rate{.numerator = 1001.0, .denominator = 30000.0, .maximumAcceleration = 1.775437, .maximumVelocityError = 0.011111},
          Rate{.numerator = 1.0, .denominator = 60.0, .maximumAcceleration = 7.427213, .maximumVelocityError = 0.019608}})
    {
        constexpr int FRAMES = 31;
        std::vector<double> times;
        std::vector<std::optional<motrack::science::PlanarValue>> positions;
        motrack::LegacyProject project;
        motrack::PointMass mass{.name = "cart"};
        for (int frame = 0; frame < FRAMES; ++frame)
        {
            const double t = frame * rate.numerator / rate.denominator;
            times.push_back(std::round(frame * 1000.0 * rate.numerator / rate.denominator) / 1000.0);
            mass.marks.push_back({.frame = frame, .x = 2.0 * t, .y = 0.0});
            positions.emplace_back(motrack::science::PlanarValue{.x = 2.0 * t, .y = 0.0});
        }
        project.pointMasses.push_back(mass);
        double acceleration = 0.0;
        for (const auto& sample : motrack::measurementPlot(project, 0, motrack::PlotQuantity::AccelerationX, times).samples)
        {
            acceleration = std::max(acceleration, std::abs(sample.vertical));
        }
        double velocityError = 0.0;
        for (const auto& sample : motrack::measurementPlot(project, 0, motrack::PlotQuantity::VelocityX, times).samples)
        {
            velocityError = std::max(velocityError, std::abs(sample.vertical - 2.0) / 2.0);
        }
        EXPECT_NEAR(acceleration, rate.maximumAcceleration, 1e-5) << rate.denominator;
        EXPECT_NEAR(velocityError, rate.maximumVelocityError, 1e-5) << rate.denominator;

        const auto tracker = motrack::science::pointMassDerivatives(motrack::science::TRACKER_DEFAULT_DERIVATIVE_ALGORITHM,
                                                                    {.firstFrame = 0, .stepSize = 1, .stepCount = FRAMES},
                                                                    positions,
                                                                    times);
        for (const auto& value : tracker.acceleration)
        {
            if (value)
            {
                EXPECT_LT(std::abs(value->x), 1e-11) << rate.denominator;
            }
        }
    }
}

TEST(MeasurementPlot, ConstantTimingOnAStrideUsesTrackersStepTimeOperandOrder)
{
    // Tracker's media-free step time is k × duration × stride (StepperClipControl.getStepTime), which for a
    // stride of 3 and 1000/30 ms frames differs from (k × stride) × duration in the last bit at some steps.
    // With a zero start the plot equals Tracker's derivatives on Tracker's step times bit for bit.
    constexpr double DURATION = 1000.0 / 30.0;
    constexpr int STRIDE = 3;
    constexpr int COUNT = 16;
    std::vector<double> trackerTimes;
    std::vector<double> reorderedTimes;
    std::vector<std::optional<motrack::science::PlanarValue>> positions;
    motrack::LegacyProject project;
    project.clip = {.firstFrame = 0, .stepSize = STRIDE, .stepCount = COUNT, .frameDurationMs = DURATION, .startTimeMs = 0.0};
    motrack::PointMass mass{.name = "ball"};
    for (int step = 0; step < COUNT; ++step)
    {
        trackerTimes.push_back(step * DURATION * STRIDE / 1000.0);
        reorderedTimes.push_back((step * STRIDE) * DURATION / 1000.0);
        const double t = trackerTimes.back();
        const motrack::science::PlanarValue point{.x = 0.3 + 1.7 * t + 0.001 * ((step * 7) % 5 - 2), .y = 2.0 - 4.9 * t * t};
        positions.emplace_back(point);
        mass.marks.push_back({.frame = step * STRIDE, .x = point.x, .y = point.y});
    }
    project.pointMasses.push_back(mass);
    const motrack::science::ClipLattice lattice{.firstFrame = 0, .stepSize = STRIDE, .stepCount = COUNT};
    const auto tracker =
        motrack::science::pointMassDerivatives(motrack::science::TRACKER_DEFAULT_DERIVATIVE_ALGORITHM, lattice, positions, trackerTimes);
    const auto reordered =
        motrack::science::pointMassDerivatives(motrack::science::TRACKER_DEFAULT_DERIVATIVE_ALGORITHM, lattice, positions, reorderedTimes);
    // The operand order is observable in this case, so the comparison below can tell them apart.
    EXPECT_TRUE(tracker.velocity != reordered.velocity || tracker.acceleration != reordered.acceleration);
    using enum motrack::PlotQuantity;
    for (const auto quantity : {VelocityX, VelocityY, AccelerationX, AccelerationY})
    {
        const bool second = quantity == AccelerationX || quantity == AccelerationY;
        const bool vertical = quantity == VelocityY || quantity == AccelerationY;
        const auto plotted = valuesByFrame(motrack::measurementPlot(project, 0, quantity));
        const auto& expected = second ? tracker.acceleration : tracker.velocity;
        ASSERT_EQ(std::ssize(plotted), std::ranges::count_if(expected, [](const auto& value) { return value.has_value(); }));
        for (int step = 0; step < COUNT; ++step)
        {
            if (const auto& value = expected[static_cast<std::size_t>(step)])
            {
                EXPECT_EQ(plotted.at(step * STRIDE), vertical ? value->y : value->x) << step;
            }
        }
    }
}

TEST(MeasurementPlot, ARepeatedFrameTimeOrAnOverflowLeavesDerivativesMissing)
{
    // Frames 1 and 2 share a time. Steps next to that zero interval have no velocity, and the acceleration
    // whose three-point stencil spans it is missing rather than infinite. x = frame, so every value is 1 or 0.
    const std::array times{0.0, 1.0, 1.0, 2.0, 3.0, 4.0, 5.0};
    motrack::LegacyProject project;
    motrack::PointMass mass{.name = "A"};
    for (int frame = 0; frame < 7; ++frame)
    {
        mass.marks.push_back({.frame = frame, .x = static_cast<double>(frame), .y = 0.0});
    }
    project.pointMasses.push_back(mass);
    const auto velocity = valuesByFrame(motrack::measurementPlot(project, 0, motrack::PlotQuantity::VelocityX, times));
    EXPECT_EQ(velocity, (std::map<int, double>{{3, 1.0}, {4, 1.0}, {5, 1.0}}));
    const auto acceleration = valuesByFrame(motrack::measurementPlot(project, 0, motrack::PlotQuantity::AccelerationX, times));
    EXPECT_EQ(acceleration, (std::map<int, double>{{3, 0.0}, {4, 0.0}}));

    // A difference that overflows on unequal intervals is missing too, never infinite.
    const std::array uneven{0.0, 1.0, 3.0};
    motrack::LegacyProject overflowing;
    overflowing.pointMasses.push_back(
        {.name = "B",
         .marks = {{.frame = 0, .x = 0.0, .y = 0.0}, {.frame = 1, .x = 1e308, .y = 0.0}, {.frame = 2, .x = -1e308, .y = 0.0}}});
    EXPECT_TRUE(motrack::measurementPlot(overflowing, 0, motrack::PlotQuantity::VelocityX, uneven).samples.empty());
}

TEST(MeasurementPlot, UsesTrackersStencilsForDecodedTimesRoundedToNanoseconds)
{
    // 30 fps frame times rounded to whole nanoseconds, as the decoder reports them: intervals of 33333333
    // and 33333334 ns are equal within that resolution, so the alternating noise of the free fall follows
    // the five-point estimate, (4/7)(-1)^k ε / dt², not the three-point -4 (-1)^k ε / dt².
    constexpr double STEP = 1.0 / 30.0;
    constexpr double NOISE = 0.001;
    std::vector<double> times;
    motrack::LegacyProject project;
    motrack::PointMass mass{.name = "ball"};
    for (int frame = 0; frame < 12; ++frame)
    {
        times.push_back(static_cast<double>(std::llround(frame * 1e9 / 30.0)) / 1e9);
        const double t = frame * STEP;
        mass.marks.push_back({.frame = frame, .x = 0.0, .y = 2.0 - 4.9 * t * t + (frame % 2 == 0 ? NOISE : -NOISE)});
    }
    project.pointMasses.push_back(mass);
    const auto acceleration = valuesByFrame(motrack::measurementPlot(project, 0, motrack::PlotQuantity::AccelerationY, times));
    ASSERT_EQ(acceleration.size(), 8u);
    for (const auto& [frame, value] : acceleration)
    {
        const double sign = frame % 2 == 0 ? 1.0 : -1.0;
        // Rounded times scale the stencil by up to 2e-8 relative.
        EXPECT_NEAR(value, -9.8 + sign * (4.0 / 7.0) * NOISE / (STEP * STEP), 1e-6) << frame;
    }
}

TEST(MeasurementPlot, ExportsTrackersDerivativesAsTheSelectedSeries)
{
    // The plot's CSV export writes exactly the plotted samples: velocity at the three interior steps and the
    // one five-point acceleration (8/7, worked by hand above), nothing at the edges.
    constexpr std::array X{0.0, 3.0, 2.0, 7.0, 8.0};
    constexpr std::array Y{1.0, 1.0, 1.0, 1.0, 1.0};
    const auto project = consecutiveMarks(X, Y, 500.0);
    EXPECT_EQ(motrack::plotCsv(motrack::measurementPlot(project, 0, motrack::PlotQuantity::AccelerationX), "x acceleration"),
              "frame,time_s,x acceleration\r\n2,1,1.1428571428571428\r\n");
    EXPECT_EQ(motrack::plotCsv(motrack::measurementPlot(project, 0, motrack::PlotQuantity::VelocityX), "x velocity"),
              "frame,time_s,x velocity\r\n1,0.5,2\r\n2,1,4\r\n3,1.5,6\r\n");
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
