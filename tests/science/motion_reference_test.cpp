#include "measurements.hpp"
#include "science/motion/motion_derivatives.hpp"
#include "science/motion/motion_filters.hpp"
#include "science/motion/motion_types.hpp"
#include "science/motion/numerical_derivative.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
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
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// Compares the motion kernels with tests/science/motion-reference.json, which tools/reference/MotionOracle.java
// records from the pinned Tracker 6.3.5 classes (python tools/corpus.py motion-oracle). See
// docs/motion-kernels.md for the cases and the agreement classes:
//  - exact: finite differences, Bounce Detection, moving average and Savitzky-Golay are IEEE arithmetic
//    and sqrt in Java's operand order (this library and test build with -ffp-contract=off);
//  - Butterworth: Java's Math.tan/sin/cos and the C++ library may differ by an ulp in the design, which
//    the filter carries through its poles (BUTTERWORTH_RELATIVE);
//  - angular data: StrictMath.atan2 (fdlibm) against the C++ atan2, a few ulps of each angle;
//  - OSP's x log x sin x cos x: Math.log/sin/cos against <cmath>, bounded per case below.

namespace
{

namespace science = motrack::science;
using science::PlanarValue;

constexpr double NOT_A_NUMBER = std::numeric_limits<double>::quiet_NaN();
constexpr double EPSILON = std::numeric_limits<double>::epsilon();

/// The value of an optional that the test requires to be present; a missing value fails the test.
template <typename T> const T& present(const std::optional<T>& value)
{
    if (!value)
    {
        throw std::logic_error("expected a value");
    }
    return *value;
}

/// Butterworth outputs relative to the largest input. A last-ulp design difference changes each pole by
/// about 1e-16, and the response's sensitivity to a pole grows like 1 / (1 - |pole|), below 1e3 for the
/// fixture's cutoffs (fc / fs >= 0.002), so a correct port stays within 1e-13; observed on Windows: 8e-16.
/// A wrong coefficient, prewarp or pass order changes outputs by more than 1e-6.
constexpr double BUTTERWORTH_RELATIVE = 1e-12;

QJsonObject loadReference()
{
    QFile file(QStringLiteral(MOTRACK_MOTION_REFERENCE));
    if (!file.open(QIODevice::ReadOnly))
    {
        ADD_FAILURE() << "Cannot open " << MOTRACK_MOTION_REFERENCE;
        return {};
    }
    QJsonParseError error{};
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    EXPECT_EQ(error.error, QJsonParseError::NoError) << error.errorString().toStdString();
    return document.object();
}

const QJsonObject& reference()
{
    static const QJsonObject loaded = loadReference();
    return loaded;
}

/// A fixture number; the oracle writes non-finite values as strings.
double number(const QJsonValue& value)
{
    if (value.isString())
    {
        const auto text = value.toString();
        if (text == QStringLiteral("Infinity"))
        {
            return std::numeric_limits<double>::infinity();
        }
        if (text == QStringLiteral("-Infinity"))
        {
            return -std::numeric_limits<double>::infinity();
        }
        return NOT_A_NUMBER;
    }
    return value.toDouble();
}

std::optional<double> optionalNumber(const QJsonValue& value)
{
    return value.isNull() ? std::nullopt : std::optional(number(value));
}

std::vector<double> numbers(const QJsonValue& value)
{
    std::vector<double> result;
    for (const auto& item : value.toArray())
    {
        result.push_back(number(item));
    }
    return result;
}

std::vector<std::optional<double>> optionalNumbers(const QJsonValue& value)
{
    std::vector<std::optional<double>> result;
    for (const auto& item : value.toArray())
    {
        result.push_back(optionalNumber(item));
    }
    return result;
}

std::vector<std::optional<PlanarValue>> points(const QJsonValue& value)
{
    std::vector<std::optional<PlanarValue>> result;
    for (const auto& item : value.toArray())
    {
        if (item.isNull())
        {
            result.emplace_back();
            continue;
        }
        const auto pair = item.toArray();
        result.emplace_back(PlanarValue{.x = number(pair.at(0)), .y = number(pair.at(1))});
    }
    return result;
}

std::string text(const QJsonValue& value)
{
    return value.toString().toStdString();
}

/// Exact agreement, with missing values in the same places.
void expectSame(std::span<const std::optional<double>> actual, std::span<const std::optional<double>> expected, const std::string& where)
{
    ASSERT_EQ(actual.size(), expected.size()) << where;
    for (std::size_t i = 0; i < expected.size(); ++i)
    {
        ASSERT_EQ(actual[i].has_value(), expected[i].has_value()) << where << " [" << i << "]";
        if (expected[i])
        {
            EXPECT_EQ(present(actual[i]), present(expected[i])) << where << " [" << i << "]";
        }
    }
}

void expectSame(std::span<const std::optional<PlanarValue>> actual,
                std::span<const std::optional<PlanarValue>> expected,
                const std::string& where)
{
    ASSERT_EQ(actual.size(), expected.size()) << where;
    for (std::size_t i = 0; i < expected.size(); ++i)
    {
        ASSERT_EQ(actual[i].has_value(), expected[i].has_value()) << where << " [" << i << "]";
        if (expected[i])
        {
            EXPECT_EQ(present(actual[i]).x, present(expected[i]).x) << where << " [" << i << "].x";
            EXPECT_EQ(present(actual[i]).y, present(expected[i]).y) << where << " [" << i << "].y";
        }
    }
}

/// Agreement within |actual - expected| <= tolerance(i), with missing values in the same places.
template <typename Tolerance>
void expectNear(std::span<const std::optional<double>> actual,
                std::span<const std::optional<double>> expected,
                Tolerance tolerance,
                const std::string& where)
{
    ASSERT_EQ(actual.size(), expected.size()) << where;
    for (std::size_t i = 0; i < expected.size(); ++i)
    {
        ASSERT_EQ(actual[i].has_value(), expected[i].has_value()) << where << " [" << i << "]";
        if (expected[i])
        {
            EXPECT_NEAR(present(actual[i]), present(expected[i]), tolerance(i)) << where << " [" << i << "]";
        }
    }
}

science::DerivativeAlgorithm algorithm(const std::string& name)
{
    if (name == "smooth_finite_difference")
    {
        return science::DerivativeAlgorithm::SmoothFiniteDifference;
    }
    if (name.starts_with("bounce_detection"))
    {
        return science::DerivativeAlgorithm::BounceDetection;
    }
    return science::DerivativeAlgorithm::FiniteDifference;
}

TEST(MotionReference, RecordsTrackerSettingsAndPassingSelfTest)
{
    const auto& root = reference();
    ASSERT_EQ(text(root["reference_version"]), "6.3.5");
    const auto tracker = root["tracker"].toObject();
    const auto codes = tracker["algorithm_codes"].toObject();
    EXPECT_EQ(codes["FINITE_DIFF"].toInt(), science::trackerAlgorithmCode(science::DerivativeAlgorithm::FiniteDifference));
    EXPECT_EQ(codes["BOUNCE_DETECT"].toInt(), science::trackerAlgorithmCode(science::DerivativeAlgorithm::BounceDetection));
    EXPECT_EQ(codes["FINITE_DIFF_VSPILL2"].toInt(), science::trackerAlgorithmCode(science::DerivativeAlgorithm::SmoothFiniteDifference));
    EXPECT_EQ(tracker["default_algorithm"].toInt(), science::trackerAlgorithmCode(science::TRACKER_DEFAULT_DERIVATIVE_ALGORITHM));
    const auto spills = tracker["spills"].toObject();
    EXPECT_EQ(spills["vDerivSpill"].toInt(), 1);
    EXPECT_EQ(spills["aDerivSpill"].toInt(), 2);
    EXPECT_EQ(spills["bounceDerivsSpill"].toInt(), 3);
    const auto defaults = tracker["filter_defaults"].toObject();
    EXPECT_EQ(defaults["moving_average"].toObject()["window"].toInt(), science::MovingAverageFilter{}.window());
    const auto butterworth = defaults["butterworth"].toObject();
    const science::ButterworthFilter defaultButterworth;
    EXPECT_EQ(butterworth["order"].toInt(), defaultButterworth.order());
    EXPECT_EQ(butterworth["cutoff_hz"].toDouble(), defaultButterworth.cutoffHz());
    EXPECT_EQ(butterworth["sample_rate_hz"].toDouble(), defaultButterworth.sampleRateHz());
    const auto savitzkyGolay = defaults["savitzky_golay"].toObject();
    EXPECT_EQ(savitzkyGolay["window"].toInt(), science::SavitzkyGolayFilter{}.window());
    EXPECT_EQ(savitzkyGolay["poly_order"].toInt(), science::SavitzkyGolayFilter{}.polynomialOrder());
    const auto selfTest = root["self_test"].toArray();
    ASSERT_EQ(selfTest.size(), 6);
    for (const auto& check : selfTest)
    {
        EXPECT_TRUE(check.toObject()["passed"].toBool()) << text(check.toObject()["name"]);
    }
}

TEST(MotionReference, DerivativesMatchTrackerExactly)
{
    const auto cases = reference()["derivative_cases"].toArray();
    ASSERT_GE(cases.size(), 15);
    int compared = 0;
    int trackerFailures = 0;
    for (const auto& item : cases)
    {
        const auto entry = item.toObject();
        const auto id = text(entry["id"]);
        const auto clipObject = entry["clip"].toObject();
        const science::ClipLattice clip{.firstFrame = clipObject["first_frame"].toInt(),
                                        .stepSize = clipObject["step_size"].toInt(),
                                        .stepCount = clipObject["step_count"].toInt()};
        const auto times = numbers(entry["times"]);
        const auto positions = points(entry["positions"]);
        const auto results = entry["results"].toArray();
        std::optional<science::StepDerivatives> stridedBounce;
        for (const auto& resultItem : results)
        {
            const auto result = resultItem.toObject();
            const auto name = text(result["algorithm"]);
            auto where = id;
            where += '/';
            where += name;
            const bool compacted = name == "bounce_detection_compacted";
            const auto lattice =
                compacted ? science::ClipLattice{.firstFrame = clip.firstFrame, .stepSize = 1, .stepCount = clip.stepCount} : clip;
            const auto steps = science::stepDerivatives(algorithm(name), lattice, positions);
            const auto motion = science::pointMassDerivatives(algorithm(name), lattice, positions, times);
            if (!result["error"].isNull())
            {
                // Tracker throws here (a stride above 1, or overflowing coordinates); MoTrack returns
                // finite estimates, checked against the compacted reference or analytic cases elsewhere.
                ++trackerFailures;
                EXPECT_EQ(name, "bounce_detection") << where;
                for (std::size_t step = 0; step < positions.size(); ++step)
                {
                    EXPECT_EQ(steps.first[step].has_value() && !positions[step], false) << where;
                }
                stridedBounce = steps;
                continue;
            }
            const auto first = points(result["first"]);
            const auto second = points(result["second"]);
            if (compacted)
            {
                // MoTrack's Bounce Detection on a stride equals Tracker's on the same samples at stride 1,
                // bit for bit.
                ASSERT_TRUE(stridedBounce.has_value()) << where;
                const auto& strided = present(stridedBounce);
                expectSame(strided.first, first, where + ".strided.first");
                expectSame(strided.second, second, where + ".strided.second");
                // The stride-1 evaluation itself is Tracker's, bit for bit.
            }
            expectSame(steps.first, first, where + ".first");
            expectSame(steps.second, second, where + ".second");
            expectSame(motion.velocity, points(result["velocity"]), where + ".velocity");
            expectSame(motion.acceleration, points(result["acceleration"]), where + ".acceleration");
            ++compared;
        }
    }
    EXPECT_GE(compared, 50);
    EXPECT_EQ(trackerFailures, 3);
}

/// MotionOracle.derivativeCases' uniformTimes(count, start ms, frame duration ms, stride) arguments for the
/// cases with a constant frame duration; the test checks that they reproduce the fixture's step times.
struct ConstantTiming
{
    std::string_view id;
    double startMs;
    double durationMs;
};

constexpr std::array CONSTANT_TIMINGS{
    ConstantTiming{.id = "free-fall", .startMs = 250.0, .durationMs = 1000.0 / 30.0},
    ConstantTiming{.id = "noisy-gaps", .startMs = 0.0, .durationMs = 40.0},
    ConstantTiming{.id = "bounce", .startMs = -500.0, .durationMs = 1000.0 / 60.0},
    ConstantTiming{.id = "stride-2", .startMs = 1000.0, .durationMs = 10.0},
    ConstantTiming{.id = "all-missing", .startMs = 0.0, .durationMs = 100.0},
    ConstantTiming{.id = "isolated", .startMs = 0.0, .durationMs = 50.0},
    ConstantTiming{.id = "huge", .startMs = 0.0, .durationMs = 100.0},
};

std::optional<ConstantTiming> constantTiming(std::string_view id)
{
    if (id.starts_with("length-"))
    {
        return ConstantTiming{.id = id, .startMs = 0.0, .durationMs = 100.0};
    }
    const auto found = std::ranges::find(CONSTANT_TIMINGS, id, &ConstantTiming::id);
    return found == CONSTANT_TIMINGS.end() ? std::nullopt : std::optional(*found);
}

/// The exact derivatives of the oracle's stride-3-variable-time motion, x = 2t and y = 1 - t²/2.
double variableTimeMotion(motrack::PlotQuantity quantity, double time)
{
    switch (quantity)
    {
    case motrack::PlotQuantity::VelocityX:
        return 2.0;
    case motrack::PlotQuantity::VelocityY:
        return -time;
    case motrack::PlotQuantity::AccelerationX:
        return 0.0;
    case motrack::PlotQuantity::AccelerationY:
        return -1.0;
    default:
        return NOT_A_NUMBER; // Not compared: fails any comparison.
    }
}

TEST(MotionReference, MeasurementPlotMatchesTrackersDefaultDerivatives)
{
    // measurementPlot, which the workspace plot and its CSV export show, against Tracker's Finite Difference
    // velocity and acceleration (#207). With Tracker's own step times as decoded frame times its values are
    // Tracker's bit for bit, and with the saved constant timing too when the clip starts at 0 ms. For another
    // start MoTrack measures step times from the clip start instead of subtracting absolute times: rounding only.
    // The variable-time case has unequal intervals, where MoTrack keeps its time-aware estimates (#87): they are
    // exact for that motion (x = 2t, y = 1 - t²/2) while Tracker's uniform stencils report a spurious a_x.
    using motrack::PlotQuantity;
    int exact = 0;
    int rounded = 0;
    int uneven = 0;
    for (const auto& item : reference()["derivative_cases"].toArray())
    {
        const auto entry = item.toObject();
        const auto id = text(entry["id"]);
        const auto clipObject = entry["clip"].toObject();
        const science::ClipLattice clip{.firstFrame = clipObject["first_frame"].toInt(),
                                        .stepSize = clipObject["step_size"].toInt(),
                                        .stepCount = clipObject["step_count"].toInt()};
        const auto times = numbers(entry["times"]);
        const auto positions = points(entry["positions"]);
        QJsonObject finiteDifference;
        for (const auto& result : entry["results"].toArray())
        {
            if (text(result.toObject()["algorithm"]) == "finite_difference")
            {
                finiteDifference = result.toObject();
            }
        }
        ASSERT_FALSE(finiteDifference.isEmpty()) << id;
        const auto velocity = points(finiteDifference["velocity"]);
        const auto acceleration = points(finiteDifference["acceleration"]);

        // An uncalibrated project plots image coordinates as they are; a NaN coordinate is a missing mark.
        motrack::LegacyProject project;
        project.clip.firstFrame = clip.firstFrame;
        project.clip.stepSize = clip.stepSize;
        project.clip.stepCount = clip.stepCount;
        motrack::PointMass mass{.name = "mass"};
        std::vector<double> frameTimes(static_cast<std::size_t>(clip.frame(std::max(clip.stepCount, 1))), NOT_A_NUMBER);
        for (int step = 0; step < clip.stepCount; ++step)
        {
            const auto index = static_cast<std::size_t>(step);
            frameTimes[static_cast<std::size_t>(clip.frame(step))] = times[index];
            if (const auto& position = positions[index])
            {
                mass.marks.push_back({.frame = clip.frame(step), .x = position->x, .y = position->y});
            }
        }
        project.pointMasses.push_back(mass);

        using Check = void (*)(double actual, double tracker, double time, PlotQuantity quantity, int& count);
        const auto compare = [&](std::span<const double> decoded, const std::string& where, Check check, int& count)
        {
            for (const auto quantity :
                 {PlotQuantity::VelocityX, PlotQuantity::VelocityY, PlotQuantity::AccelerationX, PlotQuantity::AccelerationY})
            {
                const bool second = quantity == PlotQuantity::AccelerationX || quantity == PlotQuantity::AccelerationY;
                const bool vertical = quantity == PlotQuantity::VelocityY || quantity == PlotQuantity::AccelerationY;
                std::map<int, double> plotted;
                for (const auto& sample : motrack::measurementPlot(project, 0, quantity, decoded).samples)
                {
                    plotted.emplace(sample.frame, sample.vertical);
                }
                const auto& expected = second ? acceleration : velocity;
                const auto present = std::ranges::count_if(expected, [](const auto& value) { return value.has_value(); });
                ASSERT_EQ(std::ssize(plotted), present) << where;
                for (int step = 0; step < clip.stepCount; ++step)
                {
                    const auto& tracker = expected[static_cast<std::size_t>(step)];
                    const auto found = plotted.find(clip.frame(step));
                    ASSERT_EQ(found != plotted.end(), tracker.has_value()) << where << " step " << step;
                    if (tracker)
                    {
                        check(found->second, vertical ? tracker->y : tracker->x, times[static_cast<std::size_t>(step)], quantity, count);
                    }
                }
            }
        };
        if (id == "stride-3-variable-time")
        {
            compare(
                frameTimes,
                id + "/decoded",
                [](double actual, double tracker, double time, PlotQuantity quantity, int& count)
                {
                    EXPECT_NEAR(actual, variableTimeMotion(quantity, time), 1e-9);
                    if (quantity == PlotQuantity::AccelerationX)
                    {
                        EXPECT_GT(std::abs(tracker), 0.05); // Tracker's stencil assumes equal intervals.
                    }
                    ++count;
                },
                uneven);
            continue;
        }
        const auto same = [](double actual, double tracker, double, PlotQuantity, int& count)
        {
            EXPECT_EQ(actual, tracker);
            ++count;
        };
        compare(frameTimes, id + "/decoded", same, exact);

        const auto timing = constantTiming(id);
        ASSERT_TRUE(timing.has_value()) << id;
        for (int step = 0; step < clip.stepCount; ++step)
        {
            // VideoPlayer.getStepTime(k) / 1000 in Tracker's operand order (k * duration * stride + start): the
            // table holds the oracle's arguments.
            EXPECT_EQ((step * timing->durationMs * clip.stepSize + timing->startMs) / 1000.0, times[static_cast<std::size_t>(step)])
                << id << " step " << step;
        }
        project.clip.frameDurationMs = timing->durationMs;
        project.clip.startTimeMs = timing->startMs;
        if (timing->startMs == 0.0)
        {
            compare({}, id + "/constant", same, exact);
        }
        else
        {
            compare(
                {},
                id + "/constant",
                [](double actual, double tracker, double, PlotQuantity, int& count)
                {
                    EXPECT_NEAR(actual, tracker, 1e-12 * std::max(1.0, std::abs(tracker)));
                    ++count;
                },
                rounded);
        }
    }
    // Every finite velocity and acceleration component of the 17 cases (672 exact, 312 rounded, 60 uneven).
    EXPECT_EQ(exact, 672);
    EXPECT_EQ(rounded, 312);
    EXPECT_EQ(uneven, 60);
}

TEST(MotionReference, OneDimensionalDifferencesMatchTrackerExactly)
{
    for (const auto& item : reference()["difference_cases"].toArray())
    {
        const auto entry = item.toObject();
        const auto spill = entry["spill"].toInt() == 1 ? science::DerivativeSpill::One : science::DerivativeSpill::Two;
        const auto samples = optionalNumbers(entry["samples"]);
        expectSame(science::firstStepDifference(samples, spill), optionalNumbers(entry["first"]), text(entry["id"]) + ".first");
        expectSame(science::secondStepDifference(samples, spill), optionalNumbers(entry["second"]), text(entry["id"]) + ".second");
    }
}

TEST(MotionReference, AngularDataMatchesTrackerWithinAtan2Rounding)
{
    for (const auto& item : reference()["angular_cases"].toArray())
    {
        const auto entry = item.toObject();
        const auto id = text(entry["id"]);
        const auto times = numbers(entry["times"]);
        const auto positions = points(entry["positions"]);
        const auto motion = science::pointMassAngularMotion(positions, times);
        const auto angle = optionalNumbers(entry["angle"]);
        // fdlibm's atan2 and the C++ library's each err by under 2 ulps, so each angle differs by at most
        // a few ulps of pi; the cumulative angle adds one rounding per step. 64 ulps of the largest angle
        // bounds theta; omega and alpha divide that by their step intervals (2 dt and dt^2 / 2 per term).
        // Observed on Windows: under 2% of each bound.
        double scale = std::numbers::pi;
        for (const auto& value : angle)
        {
            scale = std::max(scale, value ? std::fabs(*value) : 0.0);
        }
        const double angleTolerance = 64 * EPSILON * scale;
        const double dt = times.size() > 1 ? times[1] - times[0] : 1.0;
        expectNear(motion.angle, angle, [&](std::size_t) { return angleTolerance; }, id + ".angle");
        expectNear(
            motion.angularVelocity,
            optionalNumbers(entry["angular_velocity"]),
            [&](std::size_t) { return angleTolerance / dt; },
            id + ".angular_velocity");
        expectNear(
            motion.angularAcceleration,
            optionalNumbers(entry["angular_acceleration"]),
            [&](std::size_t) { return 8 * angleTolerance / (dt * dt); },
            id + ".angular_acceleration");
    }
}

science::MotionFilter filterFor(const QJsonObject& entry)
{
    const auto type = text(entry["type"]);
    const auto parameters = numbers(entry["parameters"]);
    if (type == "moving_average")
    {
        return science::MovingAverageFilter(static_cast<int>(parameters.at(0)));
    }
    if (type == "butterworth")
    {
        return science::ButterworthFilter(static_cast<int>(parameters.at(0)), parameters.at(1), parameters.at(2));
    }
    return science::SavitzkyGolayFilter(static_cast<int>(parameters.at(0)), static_cast<int>(parameters.at(1)));
}

void expectNormalized(const science::MotionFilter& filter, const QJsonObject& normalized, const std::string& where)
{
    if (const auto* average = std::get_if<science::MovingAverageFilter>(&filter))
    {
        EXPECT_EQ(average->window(), normalized["window"].toInt()) << where;
    }
    else if (const auto* butterworth = std::get_if<science::ButterworthFilter>(&filter))
    {
        EXPECT_EQ(butterworth->order(), normalized["order"].toInt()) << where;
        EXPECT_EQ(butterworth->cutoffHz(), normalized["cutoff_hz"].toDouble()) << where;
        EXPECT_EQ(butterworth->sampleRateHz(), normalized["sample_rate_hz"].toDouble()) << where;
    }
    else
    {
        const auto& golay = std::get<science::SavitzkyGolayFilter>(filter);
        EXPECT_EQ(golay.window(), normalized["window"].toInt()) << where;
        EXPECT_EQ(golay.polynomialOrder(), normalized["poly_order"].toInt()) << where;
    }
}

TEST(MotionReference, FiltersMatchTracker)
{
    std::map<std::string, std::vector<std::optional<double>>> signalSamples; // not "signals", a Qt keyword
    for (const auto& item : reference()["filter_signals"].toArray())
    {
        signalSamples[text(item.toObject()["id"])] = optionalNumbers(item.toObject()["samples"]);
    }
    int butterworthCases = 0;
    for (const auto& item : reference()["filter_cases"].toArray())
    {
        const auto entry = item.toObject();
        const auto id = text(entry["id"]);
        const auto filter = filterFor(entry);
        expectNormalized(filter, entry["normalized"].toObject(), id);
        const auto& samples = signalSamples.at(text(entry["signal"]));
        const auto actual = science::applyMotionFilter(filter, samples);
        const auto expected = optionalNumbers(entry["output"]);
        if (std::holds_alternative<science::ButterworthFilter>(filter))
        {
            ++butterworthCases;
            double scale = 1.0;
            for (const auto& value : samples)
            {
                scale = std::max(scale, value && std::isfinite(*value) ? std::fabs(*value) : 0.0);
            }
            expectNear(actual, expected, [&](std::size_t) { return BUTTERWORTH_RELATIVE * scale; }, id);
        }
        else
        {
            expectSame(actual, expected, id);
        }
    }
    EXPECT_GT(butterworthCases, 20);
}

double testFunction(double x)
{
    return x * std::log(x) * std::sin(x) * std::cos(x);
}

double cubic(double x)
{
    return x * x * x - 2.0 * x;
}

/// A finite-difference combination of function values within a few ulps of each: weight * 8 ulps of the
/// largest value, divided by the step powers of the formula.
double differenceTolerance(double x, double h, double weight, double divisor)
{
    double largest = 0;
    for (const double offset : {-2.0, -1.0, 0.0, 1.0, 2.0})
    {
        if (x + offset * h > 0)
        {
            largest = std::max(largest, std::fabs(testFunction(x + offset * h)));
        }
    }
    return weight * 8 * EPSILON * largest / divisor;
}

TEST(MotionReference, OspNumericalDerivativesMatchJava)
{
    int cases = 0;
    for (const auto& item : reference()["numerical_cases"].toArray())
    {
        const auto entry = item.toObject();
        const auto method = text(entry["method"]);
        const bool transcendental = text(entry["function"]) == "x-log-sin-cos";
        const science::RealFunction f = transcendental ? science::RealFunction(testFunction) : science::RealFunction(cubic);
        const double h = number(entry["h"]);
        const double expected = number(entry["value"]);
        const std::string where = method + "/" + text(entry["function"]) + "/" + std::to_string(number(entry["x"]));
        ++cases;
        if (method == "benchmark-sum")
        {
            // src/test/Derivative.java: the sum of getFirst(f, i, 0.1) for i = 1 .. 999999. Each term differs by
            // at most 8 ulps of |f| / 2h (6e-8 near i = 1e6); even if all 1e6 errors had one sign, the sum (-8e6)
            // would move by under 1e-8 of itself. Observed on Windows: 2e-13.
            double sum = 0;
            for (int i = 1; i < 1000000; i++)
            {
                sum += science::centeredDerivative(f, i, 0.1);
            }
            EXPECT_NEAR(sum, expected, 1e-8 * std::fabs(expected)) << where;
            continue;
        }
        if (method == "partial")
        {
            const auto point = numbers(entry["point"]);
            const auto surface = [](std::span<const double> v)
            {
                return v[0] * v[0] * v[1] - 3.0 * v[1] + v[2] / 4.0;
            };
            EXPECT_EQ(science::partialDerivative(surface, point, static_cast<std::size_t>(entry["index"].toInt()), h), expected) << where;
            continue;
        }
        const double x = number(entry["x"]);
        if (method == "romberg")
        {
            const auto result = science::rombergDerivative(f, x, h, number(entry["tolerance"]));
            const auto status = text(entry["status"]);
            EXPECT_EQ(result.status,
                      status == "converged"       ? science::RombergStatus::Converged
                      : status == "not_converged" ? science::RombergStatus::NotConverged
                                                  : science::RombergStatus::StepBelowPrecision)
                << where;
            if (transcendental)
            {
                // Richardson weights sum to at most 4^5 / (4^5 - 1) per column over steps down to h / 32.
                EXPECT_NEAR(result.value, expected, differenceTolerance(x, h / 32, 4, h / 32)) << where;
            }
            else
            {
                EXPECT_EQ(result.value, expected) << where;
            }
            continue;
        }
        double actual = 0;
        double weight = 2;      // |coefficients| of the difference formula
        double divisor = 2 * h; // step power in the denominator
        if (method == "first" || method == "centered")
        {
            actual = science::centeredDerivative(f, x, h);
        }
        else if (method == "get-first")
        {
            const auto derivative = [&](double at)
            {
                return science::centeredDerivative(f, at, h);
            };
            actual = derivative(x);
        }
        else if (method == "forward")
        {
            actual = science::forwardDerivative(f, x, h);
            weight = 8;
        }
        else if (method == "backward")
        {
            actual = science::backwardDerivative(f, x, h);
            weight = 8;
        }
        else
        {
            ASSERT_TRUE(method == "second" || method == "get-second") << where;
            actual = science::secondDerivative(f, x, h);
            weight = 4;
            divisor = h * h;
        }
        if (transcendental)
        {
            EXPECT_NEAR(actual, expected, differenceTolerance(x, h, weight, divisor)) << where;
        }
        else
        {
            EXPECT_EQ(actual, expected) << where;
        }
    }
    EXPECT_GE(cases, 60);
}

} // namespace
