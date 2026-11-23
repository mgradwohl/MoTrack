#include "measurements.hpp"

#include "science/motion/motion_derivatives.hpp"
#include "science/motion/motion_types.hpp"
#include "science/motion/timestamp_precision.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <span>
#include <stdexcept>
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

/// The value of an optional that the test requires to be present; a missing value fails the test.
template <typename T> const T& present(const std::optional<T>& value)
{
    if (!value)
    {
        throw std::logic_error("expected a value");
    }
    return *value;
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

TEST(MeasurementPlot, UsesTimeAwareEstimatesForUnequalDecodedFrameIntervals)
{
    // Decoded frame times 0, 1, 3, 4, 6 s and x = t², y = 2t. For unequal intervals velocity is the slope of
    // the quadratic through each step and its neighbours, and acceleration the five-point least-squares
    // parabola (#87, #331), both exact here: v_x = 2t and a_x = 2. Tracker's uniform stencils would report
    // v_x = 3, 5, 9 and a_x = (37/7) / 1.5² ≈ 2.35. Acceleration needs Tracker's five-step window: step 2 only.
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
    EXPECT_NEAR(accelerationX.samples.front().vertical, 2.0, 1e-12);
    const auto accelerationY = motrack::measurementPlot(project, 0, motrack::PlotQuantity::AccelerationY, times);
    ASSERT_EQ(accelerationY.samples.size(), 1u);
    EXPECT_NEAR(accelerationY.samples.front().vertical, 0.0, 1e-12);

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

TEST(MeasurementPlot, ConstantRateCaptureWithMillisecondTimestampsUsesTrackersStencils)
{
    // Constant-rate capture whose decoded times are whole milliseconds (the Matroska/WebM time base): positions
    // follow the true times, x = 2 m/s × k / rate, while the 33/34 ms (or 16/17 ms) intervals only reflect the
    // rounding. With the stream's 1/1000 s time base every window is evenly spaced within its resolution, so
    // the values are Tracker's: no acceleration (Tracker's stencil is accurate to about 1e-13) and Tracker's
    // velocity. Before #331 the time-aware three-point estimates reported up to 1.77, 1.78 and 7.43 m/s² here.
    // Without the time base the five-point fit still sees the rounding as jitter (0.26 to 1.0 m/s²).
    struct Rate
    {
        double numerator;
        double denominator;
    };
    for (const auto& rate : {Rate{.numerator = 1.0, .denominator = 30.0},
                             Rate{.numerator = 1001.0, .denominator = 30000.0},
                             Rate{.numerator = 1.0, .denominator = 60.0}})
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
        constexpr motrack::FrameTimeBase MILLISECONDS{.numerator = 1, .denominator = 1000};
        const auto acceleration = motrack::measurementPlot(project, 0, motrack::PlotQuantity::AccelerationX, times, 0.0, MILLISECONDS);
        ASSERT_EQ(acceleration.samples.size(), static_cast<std::size_t>(FRAMES - 4)) << rate.denominator;
        for (const auto& sample : acceleration.samples)
        {
            EXPECT_LT(std::abs(sample.vertical), 1e-9) << rate.denominator << " frame " << sample.frame;
        }
        const auto tracker = motrack::science::pointMassDerivatives(motrack::science::TRACKER_DEFAULT_DERIVATIVE_ALGORITHM,
                                                                    {.firstFrame = 0, .stepSize = 1, .stepCount = FRAMES},
                                                                    positions,
                                                                    times);
        const auto velocity =
            valuesByFrame(motrack::measurementPlot(project, 0, motrack::PlotQuantity::VelocityX, times, 0.0, MILLISECONDS));
        ASSERT_EQ(velocity.size(), static_cast<std::size_t>(FRAMES - 2));
        for (const auto& [frame, value] : velocity)
        {
            EXPECT_EQ(value, present(tracker.velocity.at(static_cast<std::size_t>(frame))).x) << rate.denominator << " frame " << frame;
        }

        double unknownBase = 0.0;
        for (const auto& sample : motrack::measurementPlot(project, 0, motrack::PlotQuantity::AccelerationX, times).samples)
        {
            unknownBase = std::max(unknownBase, std::abs(sample.vertical));
        }
        EXPECT_GT(unknownBase, 0.2) << rate.denominator;
    }
}

TEST(MeasurementPlot, AMixedClipOnACoarseTimeBaseHasNoEstimatorDiscontinuity)
{
    // 30 fps on a 1/600 s time base (20 ticks per frame), free fall with ±1 mm alternating noise and positions at
    // the recorded times; frame 10 is one, two or three ticks late. The noise alone moves Tracker's five-point
    // estimate by ±(4/7) ε / dt² = ±0.51 m/s², where a three-point estimate would move it by ±3.6 m/s².
    constexpr double STEP = 1.0 / 30.0;
    constexpr double NOISE = 0.001;
    constexpr double GRAVITY = -9.8;
    constexpr int FRAMES = 21;
    constexpr motrack::FrameTimeBase PHONE{.numerator = 1, .denominator = 600};
    const double band = (4.0 / 7.0) * NOISE / (STEP * STEP);
    struct Recording
    {
        motrack::LegacyProject project;
        std::vector<double> times;
        std::vector<std::optional<motrack::science::PlanarValue>> positions;
    };
    const auto clip = [&](int lateTicks)
    {
        Recording result;
        motrack::PointMass ball{.name = "ball"};
        for (int frame = 0; frame < FRAMES; ++frame)
        {
            const auto ticks = 20 * frame + (frame == 10 ? lateTicks : 0);
            // Decoded times are whole nanoseconds of the tick times.
            const double t = static_cast<double>(std::llround(ticks * 1e9 / 600.0)) / 1e9;
            const double y = 2.0 - 4.9 * t * t + (frame % 2 == 0 ? NOISE : -NOISE);
            result.times.push_back(t);
            result.positions.emplace_back(motrack::science::PlanarValue{.x = 0.0, .y = y});
            ball.marks.push_back({.frame = frame, .x = 0.0, .y = y});
        }
        result.project.pointMasses.push_back(ball);
        return result;
    };

    // One tick late: every window lies within one tick of its evenly spaced grid, which is the resolution of
    // these timestamps, so all values are Tracker's. Treating that tick as rounding moves the steps whose
    // window holds frame 10 by at most (2/7) v tb / dt² = 1.4 m/s² at v = 3.3 m/s, never by the 3.6 m/s² noise
    // burst of a three-point estimate.
    {
        const auto oneTick = clip(1);
        const auto tracker = motrack::science::pointMassDerivatives(motrack::science::TRACKER_DEFAULT_DERIVATIVE_ALGORITHM,
                                                                    {.firstFrame = 0, .stepSize = 1, .stepCount = FRAMES},
                                                                    oneTick.positions,
                                                                    oneTick.times);
        const auto plotted =
            valuesByFrame(motrack::measurementPlot(oneTick.project, 0, motrack::PlotQuantity::AccelerationY, oneTick.times, 0.0, PHONE));
        ASSERT_EQ(plotted.size(), static_cast<std::size_t>(FRAMES - 4));
        for (const auto& [frame, value] : plotted)
        {
            EXPECT_EQ(value, present(tracker.acceleration.at(static_cast<std::size_t>(frame))).y) << frame;
            EXPECT_LT(std::abs(value - GRAVITY), band + 1.45) << frame;
        }
    }

    // Two and three ticks late: the windows that hold frame 10 (steps 8 to 12) are not evenly spaced, so they
    // use the time-aware five-point fit, which is exact for the free fall; the others are Tracker's. Every value
    // stays within the noise band, so nothing jumps where the estimator changes (steps 7/8 and 12/13).
    for (const auto lateTicks : {2, 3})
    {
        const auto late = clip(lateTicks);
        const auto plotted =
            valuesByFrame(motrack::measurementPlot(late.project, 0, motrack::PlotQuantity::AccelerationY, late.times, 0.0, PHONE));
        ASSERT_EQ(plotted.size(), static_cast<std::size_t>(FRAMES - 4));
        std::optional<double> previous;
        for (const auto& [frame, value] : plotted)
        {
            EXPECT_LT(std::abs(value - GRAVITY), band + 0.05) << lateTicks << " ticks, frame " << frame;
            if (previous)
            {
                EXPECT_LT(std::abs(value - *previous), 2.0 * band + 0.1) << lateTicks << " ticks, frame " << frame;
            }
            previous = value;
        }
    }
}

/// The time base an index records for timestamps in ticks of `tick`: the tick with the precision inferred from them.
motrack::FrameTimeBase indexedTimeBase(motrack::science::SecondsRatio tick, std::span<const std::int64_t> ticks)
{
    const auto precision = motrack::science::timestampPrecision(tick, ticks);
    return {.numerator = tick.numerator,
            .denominator = tick.denominator,
            .precisionNumerator = precision.numerator,
            .precisionDenominator = precision.denominator};
}

/// Decoded frame times in seconds: the tick times rescaled to whole nanoseconds, as the movie index records them.
std::vector<double> decodedTimes(std::span<const std::int64_t> ticks, double ticksPerSecond)
{
    std::vector<double> result;
    for (const auto tick : ticks)
    {
        result.push_back(static_cast<double>(std::llround(static_cast<double>(tick) * 1e9 / ticksPerSecond)) / 1e9);
    }
    return result;
}

double largestMagnitude(const motrack::PlotSeries& series)
{
    double result = 0.0;
    for (const auto& sample : series.samples)
    {
        result = std::max(result, std::abs(sample.vertical));
    }
    return result;
}

TEST(MeasurementPlot, RemuxedMillisecondTimestampsOnAFinerTimeBaseUseTrackersStencils)
{
    // MKV or WebM remuxed to MP4 without re-encoding (for example OBS "remux to MP4"): FFmpeg's MP4 muxer raises the
    // 1/1000 s time base to 1/16000 s, so the timestamps of a constant-rate capture stay whole milliseconds, now in
    // steps of 16 ticks. Positions follow the true times, x = 2 m/s × k / rate. The index infers the timestamps'
    // 1 ms precision (#335), every window is evenly spaced within it, and the values are Tracker's bit for bit: no
    // acceleration and Tracker's velocity. With the declared 1/16000 s alone (#334) the 33/34 ms (16/17 ms) intervals
    // count as uneven and the five-point fit reports their rounding as acceleration. Those maxima were computed
    // independently in exact rational arithmetic.
    constexpr motrack::science::SecondsRatio MP4{.numerator = 1, .denominator = 16000};
    struct Rate
    {
        std::int64_t numerator;
        std::int64_t denominator;
        double declaredOnly;
    };
    for (const auto& rate : {Rate{.numerator = 1, .denominator = 30, .declaredOnly = 0.260579893},
                             Rate{.numerator = 1001, .denominator = 30000, .declaredOnly = 0.260840473},
                             Rate{.numerator = 1, .denominator = 60, .declaredOnly = 1.001561498}})
    {
        constexpr int FRAMES = 31;
        std::vector<std::int64_t> ticks;
        std::vector<std::optional<motrack::science::PlanarValue>> positions;
        motrack::LegacyProject project;
        motrack::PointMass mass{.name = "cart"};
        for (std::int64_t frame = 0; frame < FRAMES; ++frame)
        {
            const double t = static_cast<double>(frame * rate.numerator) / static_cast<double>(rate.denominator);
            ticks.push_back(16 * std::llround(static_cast<double>(frame * 1000 * rate.numerator) / static_cast<double>(rate.denominator)));
            mass.marks.push_back({.frame = static_cast<int>(frame), .x = 2.0 * t, .y = 0.0});
            positions.emplace_back(motrack::science::PlanarValue{.x = 2.0 * t, .y = 0.0});
        }
        project.pointMasses.push_back(mass);
        const auto times = decodedTimes(ticks, 16000.0);
        const auto timeBase = indexedTimeBase(MP4, ticks);
        ASSERT_TRUE(timeBase.precisionKnown());
        EXPECT_EQ(timeBase.resolutionSeconds(), 0.001) << rate.denominator;

        const auto tracker = motrack::science::pointMassDerivatives(motrack::science::TRACKER_DEFAULT_DERIVATIVE_ALGORITHM,
                                                                    {.firstFrame = 0, .stepSize = 1, .stepCount = FRAMES},
                                                                    positions,
                                                                    times);
        const auto acceleration =
            valuesByFrame(motrack::measurementPlot(project, 0, motrack::PlotQuantity::AccelerationX, times, 0.0, timeBase));
        ASSERT_EQ(acceleration.size(), static_cast<std::size_t>(FRAMES - 4)) << rate.denominator;
        for (const auto& [frame, value] : acceleration)
        {
            EXPECT_LT(std::abs(value), 1e-9) << rate.denominator << " frame " << frame;
            EXPECT_EQ(value, present(tracker.acceleration.at(static_cast<std::size_t>(frame))).x) << rate.denominator << " frame " << frame;
        }
        const auto velocity = valuesByFrame(motrack::measurementPlot(project, 0, motrack::PlotQuantity::VelocityX, times, 0.0, timeBase));
        ASSERT_EQ(velocity.size(), static_cast<std::size_t>(FRAMES - 2));
        for (const auto& [frame, value] : velocity)
        {
            EXPECT_EQ(value, present(tracker.velocity.at(static_cast<std::size_t>(frame))).x) << rate.denominator << " frame " << frame;
        }

        const motrack::FrameTimeBase declared{.numerator = 1, .denominator = 16000};
        EXPECT_NEAR(largestMagnitude(motrack::measurementPlot(project, 0, motrack::PlotQuantity::AccelerationX, times, 0.0, declared)),
                    rate.declaredOnly,
                    1e-6)
            << rate.denominator;
    }
}

TEST(MeasurementPlot, GenuineVariableTimingOnAFineTimeBaseIsStillUneven)
{
    // True variable frame rate on a 1/90000 s time base, with positions at the recorded times (x = 2 m/s × t), in two
    // forms: 30 fps with up to ±0.5 ms of jitter, and 25 fps whose frames lengthen from 3600 to 3645 ticks (40 to
    // 40.5 ms) after frame 15. Their timestamps use the fine ticks, so the inferred precision stays 1/90000 s, the
    // uneven windows get the time-aware fit, and steady motion shows no acceleration. A coarser precision would hide
    // that timing: 1 ms for the jitter, or the offsets' greatest common divisor (45 ticks) for the rate change, gives
    // Tracker's stencils and 0.261 and 0.265 m/s² of acceleration (computed independently in exact arithmetic).
    constexpr motrack::science::SecondsRatio TRANSPORT_STREAM{.numerator = 1, .denominator = 90000};
    constexpr int FRAMES = 31;
    struct Case
    {
        std::vector<std::int64_t> ticks;
        motrack::FrameTimeBase coarsened;
        double coarsenedAtLeast;
    };
    std::vector<Case> cases(2);
    for (std::int64_t frame = 0; frame < FRAMES; ++frame)
    {
        cases[0].ticks.push_back(3000 * frame + (frame * 37) % 91 - 45);
        cases[1].ticks.push_back(frame <= 15 ? 3600 * frame : std::int64_t{3600} * 15 + 3645 * (frame - 15));
    }
    cases[0].coarsened = {.numerator = 1, .denominator = 90000, .precisionNumerator = 1, .precisionDenominator = 1000};
    cases[0].coarsenedAtLeast = 0.2;
    cases[1].coarsened = {.numerator = 1, .denominator = 90000, .precisionNumerator = 45, .precisionDenominator = 90000};
    cases[1].coarsenedAtLeast = 0.2;
    for (const auto& [ticks, coarsened, coarsenedAtLeast] : cases)
    {
        const auto timeBase = indexedTimeBase(TRANSPORT_STREAM, ticks);
        EXPECT_EQ(timeBase.resolutionSeconds(), 1.0 / 90000.0);
        const auto times = decodedTimes(ticks, 90000.0);
        motrack::LegacyProject project;
        motrack::PointMass mass{.name = "cart"};
        for (int frame = 0; frame < FRAMES; ++frame)
        {
            mass.marks.push_back({.frame = frame, .x = 2.0 * times.at(static_cast<std::size_t>(frame)), .y = 0.0});
        }
        project.pointMasses.push_back(mass);
        const auto acceleration = motrack::measurementPlot(project, 0, motrack::PlotQuantity::AccelerationX, times, 0.0, timeBase);
        ASSERT_EQ(acceleration.samples.size(), static_cast<std::size_t>(FRAMES - 4));
        EXPECT_LT(largestMagnitude(acceleration), 1e-9);
        EXPECT_GT(largestMagnitude(motrack::measurementPlot(project, 0, motrack::PlotQuantity::AccelerationX, times, 0.0, coarsened)),
                  coarsenedAtLeast);
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
    // Frames 1 and 2 share a time. Steps next to that zero interval have no velocity, and every acceleration
    // whose five-step window spans it (steps 2 and 3) is missing rather than infinite. x = frame, so every
    // value is 1 or 0.
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
    EXPECT_EQ(acceleration, (std::map<int, double>{{4, 0.0}}));

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
