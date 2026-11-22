#include "science/motion/motion_derivatives.hpp"
#include "science/motion/motion_filters.hpp"
#include "science/motion/motion_types.hpp"
#include "science/motion/numerical_derivative.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <numbers>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

// Independent expectations for the motion kernels: analytic derivatives of polynomials and sinusoids,
// the documented gap and edge rules, units, strides and degenerate inputs. These hold for any correct
// implementation of Tracker's formulas, so a port that is consistently wrong with its own oracle fails.

namespace
{

namespace science = motrack::science;
using science::DerivativeAlgorithm;
using science::DerivativeSpill;
using science::PlanarValue;

/// The value of an optional that the test requires to be present; a missing value fails the test.
template <typename T> const T& present(const std::optional<T>& value)
{
    if (!value)
    {
        throw std::logic_error("expected a value");
    }
    return *value;
}

constexpr double NOT_A_NUMBER = std::numeric_limits<double>::quiet_NaN();
constexpr double INFINITE = std::numeric_limits<double>::infinity();

/// Times of clip steps in seconds for a constant frame duration, the way VideoPlayer.getStepTime / 1000
/// computes them from a clip start time, a stride and a frame duration in milliseconds, in its operand
/// order: StepperClipControl's k * frameDuration * stepSize, then the start time.
std::vector<double> stepTimes(int count, double startMs, double frameMs, int stride = 1)
{
    std::vector<double> times;
    times.reserve(static_cast<std::size_t>(std::max(count, 0)));
    for (int k = 0; k < count; ++k)
    {
        times.push_back((k * frameMs * stride + startMs) / 1000.0);
    }
    return times;
}

template <typename Function> std::vector<std::optional<PlanarValue>> trajectory(std::span<const double> times, Function position)
{
    std::vector<std::optional<PlanarValue>> result;
    for (const double t : times)
    {
        result.emplace_back(position(t));
    }
    return result;
}

std::vector<std::optional<double>> series(std::span<const double> values)
{
    return {values.begin(), values.end()};
}

science::ClipLattice lattice(int count, int first = 0, int stride = 1)
{
    return {.firstFrame = first, .stepSize = stride, .stepCount = count};
}

// ------------------------------------------------------------------------------------------------
// Clip lattice and marks

TEST(MotionKernels, ClipLatticeValidatesTrackerRanges)
{
    EXPECT_TRUE(lattice(0).valid());
    EXPECT_TRUE(lattice(10, 5, 3).valid());
    EXPECT_EQ(lattice(10, 5, 3).frame(4), 17);
    EXPECT_FALSE(lattice(5, -1).valid());
    EXPECT_FALSE(lattice(5, 0, 0).valid());
    EXPECT_FALSE(lattice(-1).valid());
    EXPECT_TRUE(lattice(2, std::numeric_limits<int>::max() - 1, 1).valid());
    EXPECT_FALSE(lattice(3, std::numeric_limits<int>::max() - 1, 1).valid());
}

TEST(MotionKernels, ClipStepPositionsKeepOnlyClipFramesAndFiniteMarks)
{
    const std::vector<science::FramePosition> marks = {
        {.frame = 3, .position = {.x = 9, .y = 9}},            // before the clip
        {.frame = 4, .position = {.x = 1, .y = 2}},            // step 0
        {.frame = 5, .position = {.x = 8, .y = 8}},            // between steps
        {.frame = 6, .position = {.x = 3, .y = 4}},            // step 1, replaced below
        {.frame = 6, .position = {.x = 5, .y = 6}},            // the last mark of a frame wins
        {.frame = 8, .position = {.x = NOT_A_NUMBER, .y = 1}}, // NaN world position: missing
        {.frame = 10, .position = {.x = 1, .y = INFINITE}},    // non-finite: missing
        {.frame = 12, .position = {.x = 7, .y = 7}},           // beyond the last step
    };
    const auto positions = science::clipStepPositions(lattice(4, 4, 2), marks);
    ASSERT_EQ(positions.size(), 4U);
    EXPECT_EQ(positions[0], (PlanarValue{.x = 1, .y = 2}));
    EXPECT_EQ(positions[1], (PlanarValue{.x = 5, .y = 6}));
    EXPECT_FALSE(positions[2].has_value());
    EXPECT_FALSE(positions[3].has_value());
    const std::vector<science::FramePosition> replaced = {{.frame = 4, .position = {.x = 1, .y = 2}},
                                                          {.frame = 4, .position = {.x = NOT_A_NUMBER, .y = 0}}};
    EXPECT_FALSE(science::clipStepPositions(lattice(1, 4), replaced)[0].has_value());
    EXPECT_THROW((void)science::clipStepPositions(lattice(1, 0, 0), marks), std::invalid_argument);
}

TEST(MotionKernels, AlgorithmCodesMatchTracker)
{
    EXPECT_EQ(science::TRACKER_DEFAULT_DERIVATIVE_ALGORITHM, DerivativeAlgorithm::FiniteDifference);
    EXPECT_EQ(science::trackerAlgorithmCode(DerivativeAlgorithm::FiniteDifference), 0);
    EXPECT_EQ(science::trackerAlgorithmCode(DerivativeAlgorithm::BounceDetection), 1);
    EXPECT_EQ(science::trackerAlgorithmCode(DerivativeAlgorithm::SmoothFiniteDifference), 2);
    for (const auto algorithm : science::ALL_DERIVATIVE_ALGORITHMS)
    {
        EXPECT_EQ(science::derivativeAlgorithmFromTrackerCode(science::trackerAlgorithmCode(algorithm)), algorithm);
    }
    EXPECT_FALSE(science::derivativeAlgorithmFromTrackerCode(3).has_value());
    EXPECT_FALSE(science::derivativeAlgorithmFromTrackerCode(-1).has_value());
}

// ------------------------------------------------------------------------------------------------
// Finite differences

TEST(MotionKernels, StepDifferencesAreExactForQuadraticsAndKnownForCubics)
{
    // x = k^2 per step: first derivative 2k, second derivative 2 at every spill.
    std::vector<double> square;
    std::vector<double> cube;
    for (int k = 0; k < 9; ++k)
    {
        square.push_back(k * k);
        cube.push_back(k * k * k);
    }
    for (const auto spill : {DerivativeSpill::One, DerivativeSpill::Two})
    {
        const auto first = science::firstStepDifference(series(square), spill);
        const auto second = science::secondStepDifference(series(square), spill);
        const auto edge = static_cast<std::size_t>(spill);
        for (std::size_t k = 0; k < square.size(); ++k)
        {
            const bool interior = k >= edge && k + edge < square.size();
            ASSERT_EQ(first[k].has_value(), interior) << k;
            if (interior)
            {
                EXPECT_EQ(present(first[k]), 2.0 * static_cast<double>(k));
            }
        }
        // Acceleration always needs its own window.
        for (std::size_t k = 0; k < square.size(); ++k)
        {
            const bool interior = k >= edge && k + edge < square.size();
            ASSERT_EQ(second[k].has_value(), interior) << k;
            if (interior)
            {
                EXPECT_NEAR(present(second[k]), 2.0, 1e-12);
            }
        }
    }
    // For x = k^3 the centred difference is 3k^2 + 1 and the five-point slope 3k^2 + 34/10 (h = 1); both
    // second differences are exactly 6k, the cubic term being odd about the centre.
    const auto first1 = science::firstStepDifference(series(cube), DerivativeSpill::One);
    const auto first2 = science::firstStepDifference(series(cube), DerivativeSpill::Two);
    const auto second1 = science::secondStepDifference(series(cube), DerivativeSpill::One);
    const auto second2 = science::secondStepDifference(series(cube), DerivativeSpill::Two);
    for (std::size_t k = 2; k + 2 < cube.size(); ++k)
    {
        const auto kk = static_cast<double>(k);
        EXPECT_DOUBLE_EQ(present(first1[k]), 3 * kk * kk + 1);
        EXPECT_DOUBLE_EQ(present(first2[k]), 3 * kk * kk + 3.4);
        EXPECT_DOUBLE_EQ(present(second1[k]), 6 * kk);
        EXPECT_NEAR(present(second2[k]), 6 * kk, 1e-12);
    }
}

TEST(MotionKernels, StepDifferencesNeedTheirWholeWindowAndNeverInventValues)
{
    const std::vector<std::optional<double>> samples = {0, 1, 2, std::nullopt, 4, 5, NOT_A_NUMBER, 7, 8, INFINITE, 10, 11, 12};
    const auto first = science::firstStepDifference(samples, DerivativeSpill::One);
    const std::vector<bool> expectedFirst = {false, true, false, false, false, false, false, false, false, false, false, true, false};
    const auto second = science::secondStepDifference(samples, DerivativeSpill::Two);
    for (std::size_t k = 0; k < samples.size(); ++k)
    {
        EXPECT_EQ(first[k].has_value(), expectedFirst[k]) << k;
        EXPECT_FALSE(second[k].has_value()) << k; // no five consecutive samples
    }
    EXPECT_EQ(present(first[1]), 1.0);
    EXPECT_TRUE(science::firstStepDifference({}, DerivativeSpill::Two).empty());
    // An overflowing difference is missing, not infinite.
    const std::vector<std::optional<double>> huge = {-1.7e308, 0, 1.7e308};
    EXPECT_FALSE(science::firstStepDifference(huge, DerivativeSpill::One)[1].has_value());
}

TEST(MotionKernels, FiniteDifferencesGiveFreeFallExactlyWithANonzeroTimeOrigin)
{
    // 30 fps from t = -0.5 s with a stride of 2: dt = 2/30 s. x = 1.25 t, y = 2 + 3 t - 4.9 t^2 (metres).
    constexpr int COUNT = 20;
    const auto times = stepTimes(COUNT, -500.0, 1000.0 / 30.0, 2);
    const auto positions = trajectory(times, [](double t) { return PlanarValue{.x = 1.25 * t, .y = 2.0 + 3.0 * t - 4.9 * t * t}; });
    for (const auto algorithm : {DerivativeAlgorithm::FiniteDifference, DerivativeAlgorithm::SmoothFiniteDifference})
    {
        const auto motion = science::pointMassDerivatives(algorithm, lattice(COUNT, 11, 2), positions, times);
        const int velocityEdge = algorithm == DerivativeAlgorithm::FiniteDifference ? 1 : 2;
        for (int k = 0; k < COUNT; ++k)
        {
            const auto step = static_cast<std::size_t>(k);
            const bool velocityPresent = k >= velocityEdge && k + velocityEdge < COUNT;
            ASSERT_EQ(motion.velocity[step].has_value(), velocityPresent) << k;
            if (velocityPresent)
            {
                // m/s: central differences are exact for a quadratic.
                EXPECT_NEAR(present(motion.velocity[step]).x, 1.25, 1e-12) << k;
                EXPECT_NEAR(present(motion.velocity[step]).y, 3.0 - 9.8 * times[step], 1e-12) << k;
            }
            const bool accelerationPresent = k >= 2 && k + 2 < COUNT;
            ASSERT_EQ(motion.acceleration[step].has_value(), accelerationPresent) << k;
            if (accelerationPresent)
            {
                EXPECT_NEAR(present(motion.acceleration[step]).x, 0.0, 1e-9) << k;
                EXPECT_NEAR(present(motion.acceleration[step]).y, -9.8, 1e-9) << k; // m/s^2
            }
        }
    }
}

TEST(MotionKernels, FiniteDifferencesMatchTheAnalyticResponseToASinusoid)
{
    // x = sin(w t): spill 1 gives sin(w h)/h cos(w t); the five-point parabola gives
    // sin(w t) (4 cos 2wh - 2 cos wh - 2) / (7 h^2).
    constexpr double OMEGA = 7.0;
    constexpr double H = 0.04;
    constexpr int COUNT = 30;
    const auto times = stepTimes(COUNT, 0.0, 40.0);
    const auto positions = trajectory(times, [](double t) { return PlanarValue{.x = std::sin(OMEGA * t), .y = std::cos(OMEGA * t)}; });
    const auto motion = science::pointMassDerivatives(DerivativeAlgorithm::FiniteDifference, lattice(COUNT), positions, times);
    const double velocityGain = std::sin(OMEGA * H) / H;
    const double accelerationGain = (4 * std::cos(2 * OMEGA * H) - 2 * std::cos(OMEGA * H) - 2) / (7 * H * H);
    for (std::size_t k = 2; k + 2 < COUNT; ++k)
    {
        const double t = times[k];
        EXPECT_NEAR(present(motion.velocity[k]).x, velocityGain * std::cos(OMEGA * t), 1e-12);
        EXPECT_NEAR(present(motion.velocity[k]).y, -velocityGain * std::sin(OMEGA * t), 1e-12);
        EXPECT_NEAR(present(motion.acceleration[k]).x, accelerationGain * std::sin(OMEGA * t), 1e-9);
        EXPECT_NEAR(present(motion.acceleration[k]).y, accelerationGain * std::cos(OMEGA * t), 1e-9);
    }
    // The smoothing stencil damps more than the centred difference: a known 5-point gain.
    const auto smooth = science::pointMassDerivatives(DerivativeAlgorithm::SmoothFiniteDifference, lattice(COUNT), positions, times);
    const double smoothGain = (2 * std::sin(2 * OMEGA * H) + std::sin(OMEGA * H)) / (5 * H);
    for (std::size_t k = 2; k + 2 < COUNT; ++k)
    {
        EXPECT_NEAR(present(smooth.velocity[k]).x, smoothGain * std::cos(OMEGA * times[k]), 1e-12);
    }
}

TEST(MotionKernels, TrackerStencilsAssumeEqualStepIntervals)
{
    // Constant velocity with jittered step times (variable frame-rate media). Tracker's velocity is the
    // secant over k +/- 1, exact for linear motion; its acceleration stencil ignores the jitter and reports
    // a spurious value, which is Tracker's behaviour (docs/motion-kernels.md, MoTrack #87).
    const std::vector<double> times = {0.0, 0.035, 0.064, 0.101, 0.132, 0.168, 0.2, 0.233};
    const auto positions = trajectory(times, [](double t) { return PlanarValue{.x = 2.0 * t, .y = -t}; });
    const auto motion = science::pointMassDerivatives(DerivativeAlgorithm::FiniteDifference, lattice(8), positions, times);
    for (std::size_t k = 1; k + 1 < times.size(); ++k)
    {
        EXPECT_NEAR(present(motion.velocity[k]).x, 2.0, 1e-12);
        EXPECT_NEAR(present(motion.velocity[k]).y, -1.0, 1e-12);
    }
    ASSERT_TRUE(motion.acceleration[3].has_value());
    const auto& a = present(motion.acceleration[3]);
    const double stencil = 2 * times[1] - times[2] - 2 * times[3] - times[4] + 2 * times[5];
    const double interval = (times[5] - times[1]) / 4;
    EXPECT_NEAR(a.x, 2.0 * stencil / 7 / (interval * interval), 1e-9);
    EXPECT_GT(std::fabs(a.x), 1.0); // a constant velocity "accelerates" at over 1 m/s^2
}

TEST(MotionKernels, MissingTimesAndInvalidInputsAreRejectedOrMissing)
{
    const auto positions = trajectory(stepTimes(6, 0.0, 100.0), [](double t) { return PlanarValue{.x = t, .y = t}; });
    auto times = stepTimes(6, 0.0, 100.0);
    times[2] = NOT_A_NUMBER; // unknown step time
    const auto motion = science::pointMassDerivatives(DerivativeAlgorithm::FiniteDifference, lattice(6), positions, times);
    EXPECT_FALSE(motion.velocity[1].has_value());
    EXPECT_FALSE(motion.velocity[3].has_value());
    EXPECT_TRUE(motion.velocity[2].has_value());
    const std::vector<double> repeated(6, 1.0); // zero intervals would divide by zero
    const auto stalled = science::pointMassDerivatives(DerivativeAlgorithm::FiniteDifference, lattice(6), positions, repeated);
    EXPECT_TRUE(std::ranges::none_of(stalled.velocity, [](const auto& value) { return value.has_value(); }));
    const auto valid = stepTimes(6, 0.0, 100.0);
    EXPECT_THROW((void)science::pointMassDerivatives(DerivativeAlgorithm::FiniteDifference, lattice(5), positions, valid),
                 std::invalid_argument);
    EXPECT_THROW(
        (void)science::pointMassDerivatives(DerivativeAlgorithm::FiniteDifference, lattice(6), positions, std::span(valid).first(5)),
        std::invalid_argument);
    EXPECT_THROW((void)science::stepDerivatives(DerivativeAlgorithm::BounceDetection, lattice(6, 0, 0), positions), std::invalid_argument);
    EXPECT_THROW((void)science::pointMassAngularMotion(positions, std::span(valid).first(4)), std::invalid_argument);
}

// ------------------------------------------------------------------------------------------------
// Bounce Detection

TEST(MotionKernels, BounceDetectionIsExactForConstantAccelerationIncludingClipEdges)
{
    // A quadratic is fitted exactly by every window, so shifted edge windows also give exact estimates.
    constexpr int COUNT = 15;
    std::vector<std::optional<PlanarValue>> positions;
    for (int k = 0; k < COUNT; ++k)
    {
        const double t = k;
        positions.emplace_back(PlanarValue{.x = 1.0 + 0.5 * t, .y = 3.0 - 0.25 * t * t});
    }
    const auto steps = science::stepDerivatives(DerivativeAlgorithm::BounceDetection, lattice(COUNT, 100), positions);
    for (std::size_t k = 0; k < positions.size(); ++k)
    {
        ASSERT_TRUE(steps.first[k] && steps.second[k]) << k;
        EXPECT_NEAR(present(steps.first[k]).x, 0.5, 1e-9) << k;
        EXPECT_NEAR(present(steps.first[k]).y, -0.5 * static_cast<double>(k), 1e-9) << k;
        EXPECT_NEAR(present(steps.second[k]).x, 0.0, 1e-9) << k;
        EXPECT_NEAR(present(steps.second[k]).y, -0.5, 1e-9) << k;
    }
    // Time scaling still needs neighbouring clip steps: Tracker's table is blank at the clip edges.
    const auto times = stepTimes(COUNT, 0.0, 10.0);
    const auto motion = science::pointMassDerivatives(DerivativeAlgorithm::BounceDetection, lattice(COUNT, 100), positions, times);
    EXPECT_FALSE(motion.velocity.front().has_value());
    EXPECT_FALSE(motion.velocity.back().has_value());
    EXPECT_FALSE(motion.acceleration[1].has_value());
    EXPECT_NEAR(present(motion.velocity[1]).x, 0.5 / 0.01, 1e-6);
    EXPECT_NEAR(present(motion.acceleration[2]).y, -0.5 / (0.01 * 0.01), 1e-3);
}

TEST(MotionKernels, BounceDetectionFindsAVelocityReversal)
{
    // A ball rolls at +2 per step, rebounds off a wall between steps 9 and 10 and returns at -3 per step.
    constexpr int COUNT = 24;
    constexpr double WALL = 9.4; // step time of the impact
    std::vector<std::optional<PlanarValue>> positions;
    for (int k = 0; k < COUNT; ++k)
    {
        const double t = k;
        const double x = t < WALL ? 2.0 * t : 2.0 * WALL - 3.0 * (t - WALL);
        positions.emplace_back(PlanarValue{.x = x, .y = 1.0});
    }
    // Tracker 6.3.5 throws for a stride above 1 (#204); MoTrack finds the same reversal on any stride.
    for (const int stride : {1, 2, 3})
    {
        const auto bounce = science::stepDerivatives(DerivativeAlgorithm::BounceDetection, lattice(COUNT, 5, stride), positions);
        for (int k = 0; k < COUNT; ++k)
        {
            const auto step = static_cast<std::size_t>(k);
            ASSERT_TRUE(bounce.first[step].has_value()) << stride << " " << k;
            EXPECT_NEAR(present(bounce.first[step]).x, k < WALL ? 2.0 : -3.0, 1e-9) << stride << " " << k;
            EXPECT_NEAR(present(bounce.first[step]).y, 0.0, 1e-12) << stride << " " << k;
            // The velocity change appears as one acceleration pulse at the step nearest the impact.
            EXPECT_NEAR(present(bounce.second[step]).x, k == 9 ? -5.0 : 0.0, 1e-9) << stride << " " << k;
        }
    }
    const auto finite = science::stepDerivatives(DerivativeAlgorithm::FiniteDifference, lattice(COUNT), positions);
    // Finite differences smear the same reversal over the steps around it.
    EXPECT_NEAR(present(finite.first[9]).x, (present(positions[10]).x - present(positions[8]).x) / 2, 1e-12);
    EXPECT_GT(present(finite.first[9]).x, -3.0);
    EXPECT_LT(present(finite.first[9]).x, 2.0);
}

TEST(MotionKernels, BounceDetectionNeedsSevenStepsAndKeepsOnlyMarkedSteps)
{
    std::vector<std::optional<PlanarValue>> positions;
    positions.reserve(6);
    for (int k = 0; k < 6; ++k)
    {
        positions.emplace_back(PlanarValue{.x = static_cast<double>(k), .y = 0.0});
    }
    auto steps = science::stepDerivatives(DerivativeAlgorithm::BounceDetection, lattice(6), positions);
    EXPECT_TRUE(std::ranges::none_of(steps.first, [](const auto& value) { return value.has_value(); }));
    EXPECT_TRUE(science::stepDerivatives(DerivativeAlgorithm::BounceDetection, lattice(0), {}).first.empty());
    // Seven marked steps, then a gap: the window shifts back, but a step without a mark gets no value.
    positions.emplace_back(PlanarValue{.x = 6.0, .y = 0.0});
    positions.emplace_back(std::nullopt);
    positions.emplace_back(PlanarValue{.x = 8.0, .y = NOT_A_NUMBER});
    steps = science::stepDerivatives(DerivativeAlgorithm::BounceDetection, lattice(9), positions);
    for (std::size_t k = 0; k < 7; ++k)
    {
        ASSERT_TRUE(steps.first[k].has_value()) << k;
        EXPECT_NEAR(present(steps.first[k]).x, 1.0, 1e-12);
    }
    EXPECT_FALSE(steps.first[7].has_value());
    EXPECT_FALSE(steps.first[8].has_value());
}

TEST(MotionKernels, BounceDetectionNeedsSevenConsecutiveStepsForEveryWindow)
{
    // Seven steps with a velocity change at 5.5, a gap of three, then a run of four that no seven-step
    // window can cover: the short run gets no derivatives even though it has positions.
    std::vector<std::optional<PlanarValue>> positions;
    for (int k = 0; k < 16; ++k)
    {
        const double t = k;
        positions.emplace_back(PlanarValue{.x = t < 5.5 ? t : 5.5 + 3.0 * (t - 5.5), .y = 2.0});
    }
    for (const int gap : {7, 8, 9, 14, 15})
    {
        positions[static_cast<std::size_t>(gap)].reset();
    }
    const auto steps = science::stepDerivatives(DerivativeAlgorithm::BounceDetection, lattice(16, 3), positions);
    for (std::size_t k = 0; k < positions.size(); ++k)
    {
        EXPECT_EQ(steps.first[k].has_value(), k < 7) << k;
        EXPECT_EQ(steps.second[k].has_value(), k < 7) << k;
    }
    // Every step shares the window 0..6, where the unknown-step model (which assumes a change near the
    // centre) cannot place a change at 5.5 exactly; the estimates lie between the two slopes.
    EXPECT_NEAR(present(steps.first[1]).x, 1.0, 0.25);
    EXPECT_GT(present(steps.first[6]).x, 1.5);
    EXPECT_LT(present(steps.first[6]).x, 3.0);
}

TEST(MotionKernels, BounceDetectionOnAStrideEqualsTheSameStepsAtStrideOne)
{
    // Tracker 6.3.5 throws NullPointerException for a stride above 1 (MoTrack issue in docs/motion-kernels.md).
    constexpr int COUNT = 20;
    std::vector<std::optional<PlanarValue>> positions;
    for (int k = 0; k < COUNT; ++k)
    {
        const double t = k;
        positions.emplace_back(PlanarValue{.x = t < 7.3 ? 0.5 * t * t : 26.645 - 4.0 * (t - 7.3), .y = t * t * t / 30.0});
    }
    positions[12].reset();
    const auto reference = science::stepDerivatives(DerivativeAlgorithm::BounceDetection, lattice(COUNT, 40, 1), positions);
    for (const int stride : {2, 3, 7})
    {
        const auto strided = science::stepDerivatives(DerivativeAlgorithm::BounceDetection, lattice(COUNT, 40, stride), positions);
        for (std::size_t k = 0; k < positions.size(); ++k)
        {
            ASSERT_EQ(strided.first[k].has_value(), reference.first[k].has_value()) << stride << " " << k;
            if (reference.first[k])
            {
                // Bit for bit: the step search runs on the same virtual stride-1 frames.
                EXPECT_EQ(present(strided.first[k]), present(reference.first[k])) << stride << " " << k;
                EXPECT_EQ(present(strided.second[k]), present(reference.second[k])) << stride << " " << k;
            }
        }
    }
}

TEST(MotionKernels, BounceDetectionStaysFiniteForHugeCoordinates)
{
    // The step-time weights overflow; Tracker recurses until StackOverflowError, MoTrack skips the estimate.
    std::vector<std::optional<PlanarValue>> positions;
    positions.reserve(12);
    for (int k = 0; k < 12; ++k)
    {
        positions.emplace_back(PlanarValue{.x = 1e200 * (k < 6 ? k : 12 - k), .y = 1e200 * k});
    }
    const auto steps = science::stepDerivatives(DerivativeAlgorithm::BounceDetection, lattice(12), positions);
    for (std::size_t k = 0; k < positions.size(); ++k)
    {
        ASSERT_TRUE(steps.first[k].has_value()) << k;
        EXPECT_TRUE(std::isfinite(present(steps.first[k]).x) && std::isfinite(present(steps.first[k]).y)) << k;
        EXPECT_NEAR(present(steps.first[k]).y, 1e200, 1e194) << k;
    }
}

// ------------------------------------------------------------------------------------------------
// Angular data

TEST(MotionKernels, AngularMotionUnwrapsUniformCircularMotion)
{
    // Three turns about the origin at -9 rad/s (clockwise), starting at phase 3 rad: theta passes -pi
    // many times but stays continuous; omega is constant and alpha zero.
    constexpr double OMEGA = -9.0;
    constexpr int COUNT = 70;
    const auto times = stepTimes(COUNT, 200.0, 1000.0 / 30.0);
    auto positions =
        trajectory(times, [](double t) { return PlanarValue{.x = 2 * std::cos(3.0 + OMEGA * t), .y = 2 * std::sin(3.0 + OMEGA * t)}; });
    positions[30].reset(); // a gap shorter than half a turn keeps the unwrapping continuous
    const auto motion = science::pointMassAngularMotion(positions, times);
    for (std::size_t k = 0; k < positions.size(); ++k)
    {
        if (k == 30)
        {
            EXPECT_FALSE(motion.angle[k].has_value());
            continue;
        }
        ASSERT_TRUE(motion.angle[k].has_value()) << k;
        // atan2 starts in (-pi, pi]: theta(0) = 3 + omega t0 wrapped, then follows the true angle.
        const double expected = 3.0 + OMEGA * times[k] - (3.0 + OMEGA * times[0] - present(motion.angle[0]));
        EXPECT_NEAR(present(motion.angle[k]), expected, 1e-12) << k;
        if (motion.angularVelocity[k])
        {
            EXPECT_NEAR(present(motion.angularVelocity[k]), OMEGA, 1e-9) << k; // rad/s
        }
        if (motion.angularAcceleration[k])
        {
            EXPECT_NEAR(present(motion.angularAcceleration[k]), 0.0, 1e-6) << k;
        }
    }
    EXPECT_NEAR(present(motion.angle[0]), std::remainder(3.0 + OMEGA * times[0], 2 * std::numbers::pi), 1e-12);
    EXPECT_LT(present(motion.angle.back()) - present(motion.angle.front()), -4 * std::numbers::pi);
    EXPECT_FALSE(motion.angularVelocity[29].has_value()); // the gap breaks the windows around it
    EXPECT_FALSE(motion.angularAcceleration[32].has_value());
    EXPECT_TRUE(motion.angularVelocity[28].has_value());
    EXPECT_NEAR(present(motion.angularVelocity[28]), OMEGA, 1e-9);
}

TEST(MotionKernels, AngularMotionHandlesTheOriginAndTheNegativeAxis)
{
    const std::vector<std::optional<PlanarValue>> positions = {
        PlanarValue{.x = 0, .y = 0}, PlanarValue{.x = -1, .y = 0}, PlanarValue{.x = -1, .y = -1e-12}, PlanarValue{.x = 0, .y = -2}};
    const auto motion = science::pointMassAngularMotion(positions, stepTimes(4, 0.0, 100.0));
    EXPECT_EQ(present(motion.angle[0]), 0.0);
    EXPECT_EQ(present(motion.angle[1]), std::numbers::pi);
    // Crossing from +pi to just below -pi continues past pi rather than jumping by 2 pi.
    EXPECT_NEAR(present(motion.angle[2]), std::numbers::pi + 1e-12, 4e-15);
    EXPECT_NEAR(present(motion.angle[3]), 1.5 * std::numbers::pi, 1e-12);
    EXPECT_NEAR(present(motion.angularVelocity[1]), (present(motion.angle[2]) - present(motion.angle[0])) / 2 / 0.1, 1e-9);
}

// ------------------------------------------------------------------------------------------------
// Motion filters

TEST(MotionKernels, FilterParametersNormalizeLikeTrackerSetters)
{
    EXPECT_EQ(science::MovingAverageFilter{}.window(), 5);
    EXPECT_EQ(science::MovingAverageFilter(0).window(), 1);
    EXPECT_EQ(science::MovingAverageFilter(-4).window(), 1);
    EXPECT_EQ(science::MovingAverageFilter(4).window(), 5);
    EXPECT_EQ(science::MovingAverageFilter(std::numeric_limits<int>::max() - 1).window(), std::numeric_limits<int>::max());
    const science::ButterworthFilter defaults;
    EXPECT_EQ(defaults, science::ButterworthFilter(4, 6.0, 30.0));
    EXPECT_EQ(science::ButterworthFilter(0, 6, 30).order(), 1);
    EXPECT_EQ(science::ButterworthFilter(12, 6, 30).order(), 8);
    EXPECT_EQ(science::ButterworthFilter(2, 0, 30).cutoffHz(), 1.0);
    EXPECT_EQ(science::ButterworthFilter(2, -3, -1).sampleRateHz(), 1.0);
    EXPECT_TRUE(std::isnan(science::ButterworthFilter(2, NOT_A_NUMBER, 30).cutoffHz())); // Tracker keeps NaN
    EXPECT_EQ(science::SavitzkyGolayFilter{}, science::SavitzkyGolayFilter(7, 2));
    EXPECT_EQ(science::SavitzkyGolayFilter(1, 2).window(), 3);
    EXPECT_EQ(science::SavitzkyGolayFilter(8, 2).window(), 9);
    EXPECT_EQ(science::SavitzkyGolayFilter(7, 0).polynomialOrder(), 1);
    EXPECT_EQ(science::SavitzkyGolayFilter(5, 9).polynomialOrder(), 4);
    EXPECT_DOUBLE_EQ(science::butterworthSampleRateHz(1000.0 / 30.0), 30.0); // a 30 fps clip
    EXPECT_EQ(science::butterworthSampleRateHz(4.0), 250.0);
    EXPECT_EQ(science::butterworthSampleRateHz(0.0), 30.0);
    EXPECT_EQ(science::butterworthSampleRateHz(NOT_A_NUMBER), 30.0);
}

TEST(MotionKernels, MovingAverageTruncatesItsWindowAtRunEnds)
{
    const std::vector<std::optional<double>> samples = {0, 1, 2, 3, 4, std::nullopt, 10, INFINITE, 20, 21};
    const auto out = science::applyMotionFilter(science::MovingAverageFilter(5), samples);
    EXPECT_EQ(present(out[0]), 1.0); // (0 + 1 + 2) / 3
    EXPECT_EQ(present(out[1]), 1.5); // (0 + 1 + 2 + 3) / 4
    EXPECT_EQ(present(out[2]), 2.0); // linear interior is preserved
    EXPECT_EQ(present(out[4]), 3.0); // (2 + 3 + 4) / 3
    EXPECT_FALSE(out[5].has_value());
    EXPECT_EQ(present(out[6]), 10.0); // a single-sample run
    EXPECT_FALSE(out[7].has_value()); // a non-finite sample is a gap
    EXPECT_EQ(present(out[8]), 20.5);
    EXPECT_EQ(present(out[9]), 20.5);
    EXPECT_EQ(science::applyMotionFilter(science::MovingAverageFilter(1), samples)[3], 3.0);
    EXPECT_TRUE(science::applyMotionFilter(science::MovingAverageFilter(3), {}).empty());
}

TEST(MotionKernels, ButterworthLeavesDcShortRunsAndSuperNyquistCutoffsAlone)
{
    std::vector<std::optional<double>> samples(40, 2.5);
    samples[10].reset();
    samples[13].reset(); // a two-sample run [11, 12]
    const auto out = science::applyMotionFilter(science::ButterworthFilter(3, 2.0, 30.0), samples);
    for (std::size_t k = 0; k < samples.size(); ++k)
    {
        ASSERT_EQ(out[k].has_value(), samples[k].has_value()) << k;
        if (out[k])
        {
            EXPECT_NEAR(present(out[k]), 2.5, 1e-12) << k;
        }
    }
    std::vector<std::optional<double>> alternating;
    alternating.reserve(120);
    for (int k = 0; k < 120; ++k)
    {
        alternating.emplace_back(k % 2 == 0 ? 1.0 : -1.0);
    }
    // At or above Nyquist the filter is a pass-through, exactly.
    EXPECT_EQ(science::applyMotionFilter(science::ButterworthFilter(4, 15.0, 30.0), alternating), alternating);
    // Below it, the Nyquist-frequency signal is removed (the bilinear transform puts a zero there) once
    // the edge transients of the reflection padding have decayed.
    const auto damped = science::applyMotionFilter(science::ButterworthFilter(4, 6.0, 30.0), alternating);
    for (std::size_t k = 40; k + 40 < damped.size(); ++k)
    {
        EXPECT_LT(std::fabs(present(damped[k])), 1e-6) << k;
    }
    // A NaN cutoff, which Tracker keeps from a malformed file, yields NaN there and nothing here.
    const auto broken = science::applyMotionFilter(science::ButterworthFilter(4, NOT_A_NUMBER, 30.0), alternating);
    EXPECT_TRUE(std::ranges::none_of(broken, [](const auto& value) { return value.has_value(); }));
}

TEST(MotionKernels, ButterworthMagnitudeAtTheCutoffIsMinusSixDecibels)
{
    // Forward-backward filtering squares |H|; |H(fc)| = 1/sqrt(2) for a Butterworth design, so a sinusoid
    // at the cutoff comes out at half its amplitude, for any order.
    constexpr double FS = 100.0;
    constexpr double FC = 5.0;
    std::vector<std::optional<double>> samples;
    samples.reserve(2000);
    for (int k = 0; k < 2000; ++k)
    {
        samples.emplace_back(std::sin(2 * std::numbers::pi * FC * k / FS));
    }
    for (const int order : {1, 2, 5, 8})
    {
        const auto out = science::applyMotionFilter(science::ButterworthFilter(order, FC, FS), samples);
        double peak = 0;
        for (std::size_t k = 500; k < 1500; ++k)
        {
            peak = std::max(peak, std::fabs(present(out[k])));
        }
        EXPECT_NEAR(peak, 0.5, 2e-3) << order;
    }
}

TEST(MotionKernels, SavitzkyGolayPreservesPolynomialsUpToItsOrderIncludingEdges)
{
    for (const auto [window, order] : {std::pair{5, 1}, std::pair{7, 2}, std::pair{9, 3}, std::pair{21, 4}, std::pair{99, 6}})
    {
        std::vector<std::optional<double>> samples;
        for (int k = 0; k < 150; ++k)
        {
            const double t = (k - 75) / 40.0;
            double value = 0;
            double power = 1;
            for (int degree = 0; degree <= order; ++degree)
            {
                value += (degree % 2 == 0 ? 1.0 : -0.5) * power;
                power *= t;
            }
            samples.emplace_back(value);
        }
        const auto out = science::applyMotionFilter(science::SavitzkyGolayFilter(window, order), samples);
        for (std::size_t k = 0; k < samples.size(); ++k)
        {
            EXPECT_NEAR(present(out[k]), present(samples[k]), 1e-9) << window << "," << order << " [" << k << "]";
        }
    }
}

TEST(MotionKernels, SavitzkyGolayShrinksOrSkipsShortRuns)
{
    // Runs of 2 and 4 samples with a quadratic of order 3: the 2-run is unchanged (fewer than order + 1),
    // the 4-run uses a 3-sample window of order 2, which interpolates its samples.
    const std::vector<std::optional<double>> samples = {1, 4, std::nullopt, 0, 1, 4, 9, std::nullopt, 5};
    const auto out = science::applyMotionFilter(science::SavitzkyGolayFilter(7, 3), samples);
    EXPECT_EQ(out[0], samples[0]);
    EXPECT_EQ(out[1], samples[1]);
    EXPECT_FALSE(out[2].has_value());
    for (const std::size_t k : {3U, 4U, 5U, 6U})
    {
        EXPECT_NEAR(present(out[k]), present(samples[k]), 1e-12) << k;
    }
    EXPECT_EQ(out[8], samples[8]);
    // Noise is reduced inside a long enough run.
    std::vector<std::optional<double>> noisy;
    noisy.reserve(60);
    for (int k = 0; k < 60; ++k)
    {
        noisy.emplace_back(k % 2 == 0 ? 0.1 : -0.1);
    }
    const auto smoothed = science::applyMotionFilter(science::SavitzkyGolayFilter(9, 2), noisy);
    for (std::size_t k = 10; k < 50; ++k)
    {
        EXPECT_LT(std::fabs(present(smoothed[k])), 0.05) << k;
    }
}

TEST(MotionKernels, FilteredPositionsShareGapsAndReportRmsDeviation)
{
    std::vector<std::optional<PlanarValue>> positions;
    positions.reserve(12);
    for (int k = 0; k < 12; ++k)
    {
        positions.emplace_back(PlanarValue{.x = static_cast<double>(k), .y = k % 2 == 0 ? 1.0 : -1.0});
    }
    positions[5].reset();
    positions[8] = PlanarValue{.x = NOT_A_NUMBER, .y = 0.0}; // FilteredPointMass: an invalid step
    const auto filtered = science::filterPositions(science::MovingAverageFilter(3), positions);
    EXPECT_FALSE(filtered[5].has_value());
    EXPECT_FALSE(filtered[8].has_value());
    EXPECT_EQ(present(filtered[1]).x, 1.0);
    EXPECT_NEAR(present(filtered[1]).y, 1.0 / 3.0, 1e-15); // (1 - 1 + 1) / 3
    const auto deviation = science::filterRmsDeviation(positions, filtered);
    ASSERT_TRUE(deviation.has_value());
    const auto& rms = present(deviation);
    EXPECT_GT(rms.y, 0.5);
    EXPECT_NEAR(rms.combined, std::hypot(rms.x, rms.y), 1e-15);
    // Identical positions deviate by zero; no common step is no measurement at all, not zero.
    EXPECT_EQ(present(science::filterRmsDeviation(positions, positions)).combined, 0.0);
    const std::vector<std::optional<PlanarValue>> none(positions.size());
    EXPECT_FALSE(science::filterRmsDeviation(positions, none).has_value());
    EXPECT_THROW((void)science::filterRmsDeviation(positions, std::span(none).first(3)), std::invalid_argument);
}

// ------------------------------------------------------------------------------------------------
// OSP numerics.Derivative

TEST(MotionKernels, NumericalDerivativesFollowOspFormulas)
{
    const science::RealFunction cube = [](double x)
    {
        return x * x * x;
    };
    constexpr double H = 0.125; // a power of two keeps these exact
    EXPECT_EQ(science::centeredDerivative(cube, 2.0, H), 12.0 + H * H);
    EXPECT_EQ(science::secondDerivative(cube, 2.0, H), 12.0);
    // Second-order one-sided differences: exact for quadratics, error 2 h^2 x''' / 6 * 2 for cubics.
    const science::RealFunction square = [](double x)
    {
        return 3 * x * x - x;
    };
    EXPECT_EQ(science::forwardDerivative(square, 1.5, H), 8.0);
    EXPECT_EQ(science::backwardDerivative(square, 1.5, H), 8.0);
    EXPECT_EQ(science::forwardDerivative(cube, 2.0, H), 12.0 - 2 * H * H);
    EXPECT_EQ(science::backwardDerivative(cube, 2.0, H), 12.0 - 2 * H * H);
    const std::array point = {2.0, -1.0};
    const science::MultivariateFunction product = [](std::span<const double> v)
    {
        return v[0] * v[0] * v[1];
    };
    EXPECT_EQ(science::partialDerivative(product, point, 0, H), -4.0);
    EXPECT_EQ(science::partialDerivative(product, point, 1, H), 4.0);
    EXPECT_THROW((void)science::partialDerivative(product, point, 2, H), std::out_of_range);
}

TEST(MotionKernels, RombergExtrapolationConvergesOrReportsWhy)
{
    const science::RealFunction sine = [](double x)
    {
        return std::sin(x);
    };
    const auto converged = science::rombergDerivative(sine, 0.7, 0.4, 1e-12);
    EXPECT_EQ(converged.status, science::RombergStatus::Converged);
    EXPECT_NEAR(converged.value, std::cos(0.7), 1e-11);
    const auto exhausted = science::rombergDerivative(sine, 0.7, 0.4, 0.0);
    EXPECT_EQ(exhausted.status, science::RombergStatus::NotConverged);
    EXPECT_NEAR(exhausted.value, std::cos(0.7), 1e-3); // the last plain centred difference
    EXPECT_EQ(science::rombergDerivative(sine, 0.7, -0.4, 1e-12).status, science::RombergStatus::StepBelowPrecision);
    EXPECT_EQ(science::rombergDerivative(sine, 0.7, 3e-162, 1e-12).status, science::RombergStatus::StepBelowPrecision);
}

} // namespace
