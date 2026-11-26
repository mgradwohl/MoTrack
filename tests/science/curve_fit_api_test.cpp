#include "science/expression.hpp"
#include "science/fit/curve_fit.hpp"
#include "science/fit/minimizers.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Public-API behavior of the curve-fit kernel that the Java oracle (curve_fit_test.cpp) does not
// pin down: argument validation, statuses, cancellation and independent closed-form results.

namespace
{

using motrack::science::ALL_BUILT_IN_FITS;
using motrack::science::BuiltInFit;
using motrack::science::builtInFit;
using motrack::science::fitCurve;
using motrack::science::FitDialect;
using motrack::science::FitFunction;
using motrack::science::FitMethod;
using motrack::science::FitOptions;
using motrack::science::FitResult;
using motrack::science::FitStart;
using motrack::science::FitStatus;
using motrack::science::UncertaintyStatus;

constexpr double NOT_A_NUMBER = std::numeric_limits<double>::quiet_NaN();
constexpr double INFINITE = std::numeric_limits<double>::infinity();
constexpr std::array DIALECTS = {FitDialect::Tracker, FitDialect::Corrected};

std::vector<double> grid(double start, double step, std::size_t count)
{
    std::vector<double> x(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        x[i] = start + (static_cast<double>(i) * step);
    }
    return x;
}

std::vector<double> evaluate(const FitFunction& f, std::span<const double> parameters, std::span<const double> x)
{
    std::vector<double> y;
    for (const double xi : x)
    {
        y.push_back(f.model(xi, parameters));
    }
    return y;
}

/// A deterministic zero-mean wiggle in place of random noise.
std::vector<double> wiggle(std::vector<double> y, double size)
{
    for (std::size_t i = 0; i < y.size(); ++i)
    {
        y[i] += size * std::sin((1.7 * static_cast<double>(i)) + 0.3);
    }
    return y;
}

motrack::science::FitParameter parameter(const std::string& name, double value)
{
    return {.name = name, .value = value, .description = {}};
}

FitFunction withValues(FitFunction f, std::span<const double> values)
{
    for (std::size_t i = 0; i < values.size(); ++i)
    {
        f.parameters[i].value = values[i];
    }
    return f;
}

FitOptions options(FitDialect dialect, FitStart start = FitStart::FromScratch)
{
    FitOptions result;
    result.dialect = dialect;
    result.start = start;
    return result;
}

void expectNear(std::span<const double> expected, std::span<const double> actual, double relative)
{
    ASSERT_EQ(expected.size(), actual.size());
    for (std::size_t i = 0; i < expected.size(); ++i)
    {
        EXPECT_NEAR(expected[i], actual[i], relative * std::max(1.0, std::abs(expected[i]))) << "parameter " << i;
    }
}

TEST(CurveFit, BuiltInListsAndNames)
{
    std::vector<BuiltInFit> menu(motrack::science::TRACKER_FIT_MENU_ORDER.begin(), motrack::science::TRACKER_FIT_MENU_ORDER.end());
    std::vector<BuiltInFit> all(ALL_BUILT_IN_FITS.begin(), ALL_BUILT_IN_FITS.end());
    std::ranges::sort(menu);
    std::ranges::sort(all);
    EXPECT_EQ(menu, all);
    EXPECT_EQ(motrack::science::builtInFitDisplayName(BuiltInFit::Sinusoid), "Sine");
    EXPECT_EQ(motrack::science::builtInFitDisplayName(BuiltInFit::DampedSine), "Damped Sine");
    for (const BuiltInFit kind : ALL_BUILT_IN_FITS)
    {
        const FitFunction f = builtInFit(kind);
        EXPECT_FALSE(motrack::science::builtInFitDisplayName(kind).empty());
        EXPECT_EQ(f.builtIn, kind);
        EXPECT_TRUE(static_cast<bool>(f.model));
    }
}

TEST(CurveFit, PolynomialsOfHigherDegree)
{
    EXPECT_FALSE(motrack::science::polynomialFit(0));
    EXPECT_FALSE(motrack::science::polynomialFit(6));
    const auto quartic = motrack::science::polynomialFit(4);
    const auto quintic = motrack::science::polynomialFit(5);
    if (!quartic || !quintic)
    {
        ADD_FAILURE() << "degree 4 and 5 polynomials are supported";
        return;
    }
    EXPECT_EQ(quartic->name, "Poly4");
    EXPECT_EQ(quartic->expression, "A*x^4+B*x^3+C*x^2+D*x+E");
    EXPECT_FALSE(quartic->builtIn);
    EXPECT_EQ(quintic->parameters.back().name, "F");
    const std::vector<double> truth = {0.25, -0.5, 1.5, 2.0, -3.0, 0.75};
    const auto x = grid(-1.0, 0.125, 21);
    const auto y = evaluate(*quintic, truth, x);
    for (const FitDialect dialect : DIALECTS)
    {
        const FitResult result = fitCurve(*quintic, x, y, options(dialect));
        EXPECT_EQ(result.status, FitStatus::Fitted);
        EXPECT_EQ(result.method, FitMethod::ClosedForm);
        expectNear(truth, result.parameters, 1e-9);
    }
}

TEST(CurveFit, RejectsInconsistentRequests)
{
    const FitFunction line = builtInFit(BuiltInFit::Line);
    const std::vector<double> x = {0, 1, 2};
    const std::vector<double> y = {1, 3, 5};
    EXPECT_EQ(fitCurve(line, x, std::span<const double>(y).first(2)).status, FitStatus::InvalidRequest);
    FitOptions badFixed;
    badFixed.fixed = {true};
    EXPECT_EQ(fitCurve(line, x, y, badFixed).status, FitStatus::InvalidRequest);
    FitOptions badFallback;
    badFallback.scratchFallback = {1.0};
    EXPECT_EQ(fitCurve(line, x, y, badFallback).status, FitStatus::InvalidRequest);
    FitFunction noModel = builtInFit(BuiltInFit::Gaussian);
    noModel.model = nullptr;
    EXPECT_EQ(fitCurve(noModel, x, y).status, FitStatus::InvalidRequest);
    FitFunction wrongDegree = line;
    wrongDegree.polynomialDegree = 2;
    const FitResult invalid = fitCurve(wrongDegree, x, y);
    EXPECT_EQ(invalid.status, FitStatus::InvalidRequest);
    EXPECT_EQ(invalid.parameters, (std::vector<double>{0.0, 0.0}));
    EXPECT_TRUE(std::isnan(invalid.rmsDeviation));
}

TEST(CurveFit, LineMatchesClosedFormStandardErrors)
{
    // Independent ordinary least squares: slope, intercept, their standard errors and covariance.
    const auto x = grid(0.5, 0.25, 15);
    std::vector<double> y(x.size());
    std::ranges::transform(x, y.begin(), [](double xi) { return (1.75 * xi) - 0.4; });
    y = wiggle(y, 0.05);
    const auto n = static_cast<double>(x.size());
    double meanX = 0;
    double meanY = 0;
    for (std::size_t i = 0; i < x.size(); ++i)
    {
        meanX += x[i] / n;
        meanY += y[i] / n;
    }
    double sxx = 0;
    double sxy = 0;
    for (std::size_t i = 0; i < x.size(); ++i)
    {
        sxx += (x[i] - meanX) * (x[i] - meanX);
        sxy += (x[i] - meanX) * (y[i] - meanY);
    }
    const double slope = sxy / sxx;
    const double intercept = meanY - (slope * meanX);
    double ssr = 0;
    for (std::size_t i = 0; i < x.size(); ++i)
    {
        const double r = y[i] - ((slope * x[i]) + intercept);
        ssr += r * r;
    }
    const double s2 = ssr / (n - 2);
    const double slopeError = std::sqrt(s2 / sxx);
    const double interceptError = std::sqrt(s2 * ((1 / n) + (meanX * meanX / sxx)));
    for (const FitDialect dialect : DIALECTS)
    {
        SCOPED_TRACE(static_cast<int>(dialect));
        const FitResult result = fitCurve(builtInFit(BuiltInFit::Line), x, y, options(dialect));
        ASSERT_EQ(result.status, FitStatus::Fitted);
        expectNear(std::vector<double>{slope, intercept}, result.parameters, 1e-12);
        ASSERT_EQ(result.uncertainties.size(), 2U);
        EXPECT_NEAR(result.uncertainties[0], slopeError, 1e-6 * slopeError);
        EXPECT_NEAR(result.uncertainties[1], interceptError, 1e-6 * interceptError);
        EXPECT_EQ(result.uncertaintyStatus, (std::vector{UncertaintyStatus::Estimated, UncertaintyStatus::Estimated}));
        EXPECT_NEAR(result.rmsDeviation, std::sqrt(ssr / n), 1e-12);
        ASSERT_EQ(result.uncertaintyBand.size(), 4U);
        EXPECT_NEAR(result.uncertaintyBand[0][0], slope - slopeError, 1e-6 * slopeError);
        if (dialect == FitDialect::Corrected)
        {
            // Holding the slope one sigma low, least squares raises the intercept by x-bar sigma.
            EXPECT_NEAR(result.uncertaintyBand[0][1], intercept + (meanX * slopeError), 1e-6 * interceptError);
        }
        else
        {
            // Tracker keeps the best-fit intercept (its inner `continue` discards the refit).
            EXPECT_EQ(result.uncertaintyBand[0][1], result.parameters[1]);
        }
        const double r = sxy / std::sqrt(sxx * (sxy * sxy / sxx + ssr));
        EXPECT_NEAR(result.correlation, r * r, 1e-12);
    }
}

TEST(CurveFit, ExactSyntheticDataRecoversParameters)
{
    struct Case
    {
        BuiltInFit kind;
        std::vector<double> truth;
        std::vector<double> x;
        FitStart start;
    };
    const std::vector<Case> cases = {
        {.kind = BuiltInFit::Gaussian, .truth = {2.0, 0.7, 0.3}, .x = grid(0, 0.08, 21), .start = FitStart::FromScratch},
        {.kind = BuiltInFit::Exponential, .truth = {1.5, -2.0, 0.25}, .x = grid(0, 0.1, 21), .start = FitStart::FromScratch},
        {.kind = BuiltInFit::Sinusoid, .truth = {0.8, 3.0, 0.4, -0.2}, .x = grid(0, 0.1, 40), .start = FitStart::FromScratch},
        {.kind = BuiltInFit::DampedSine, .truth = {1.5, 5.0, 0.2, 0.05, -0.5}, .x = grid(0, 0.05, 60), .start = FitStart::FromCurrent},
        {.kind = BuiltInFit::Power, .truth = {2.5, 0.75}, .x = grid(0.5, 0.25, 15), .start = FitStart::FromScratch},
        {.kind = BuiltInFit::Log, .truth = {1.2, -0.3}, .x = grid(0.5, 0.25, 15), .start = FitStart::FromScratch},
        {.kind = BuiltInFit::Cubic, .truth = {0.3, -0.6, 0.1, 1.0}, .x = grid(-2, 0.2, 21), .start = FitStart::FromScratch},
    };
    for (const Case& c : cases)
    {
        const FitFunction f = builtInFit(c.kind);
        const auto y = evaluate(f, c.truth, c.x);
        std::vector<double> start = c.truth;
        for (double& value : start)
        {
            value *= 1.03;
        }
        for (const FitDialect dialect : DIALECTS)
        {
            SCOPED_TRACE(f.name + (dialect == FitDialect::Tracker ? " tracker" : " corrected"));
            const FitResult result =
                fitCurve(c.start == FitStart::FromCurrent ? withValues(f, start) : f, c.x, y, options(dialect, c.start));
            EXPECT_TRUE(result.status == FitStatus::Fitted || result.status == FitStatus::NotImproved);
            expectNear(c.truth, result.parameters, 1e-6);
            EXPECT_LT(result.rmsDeviation, 1e-6);
            EXPECT_FALSE(result.nonRealValues);
        }
    }
}

TEST(CurveFit, StatusesWithoutAFit)
{
    const auto x = grid(0, 0.5, 10);
    const auto y = wiggle(grid(1, 0.5, 10), 0.1);
    for (const FitDialect dialect : DIALECTS)
    {
        SCOPED_TRACE(static_cast<int>(dialect));
        FitOptions manual = options(dialect, FitStart::FromCurrent);
        manual.autofit = false;
        const FitFunction gaussian = withValues(builtInFit(BuiltInFit::Gaussian), std::vector<double>{2, 1, 0.5});
        const FitResult evaluated = fitCurve(gaussian, std::span<const double>(x).first(2), std::span<const double>(y).first(2), manual);
        EXPECT_EQ(evaluated.status, FitStatus::Manual);
        EXPECT_EQ(evaluated.parameters, (std::vector<double>{2, 1, 0.5}));
        // Without autofit the deviation is reported even with more parameters than points.
        EXPECT_TRUE(std::isfinite(evaluated.rmsDeviation));
        EXPECT_TRUE(evaluated.uncertainties.empty());

        FitOptions allFixed = options(dialect, FitStart::FromCurrent);
        allFixed.fixed = {true, true, true};
        EXPECT_EQ(fitCurve(gaussian, x, y, allFixed).status, FitStatus::NothingToFit);
        FitFunction constant;
        constant.name = "Twice";
        constant.expression = "2*x";
        constant.model = [](double xi, std::span<const double>)
        {
            return 2 * xi;
        };
        const FitResult none = fitCurve(constant, x, y, options(dialect));
        EXPECT_EQ(none.status, FitStatus::NothingToFit);
        EXPECT_TRUE(std::isfinite(none.rmsDeviation));

        const FitResult few = fitCurve(
            builtInFit(BuiltInFit::Cubic), std::span<const double>(x).first(3), std::span<const double>(y).first(3), options(dialect));
        EXPECT_EQ(few.status, FitStatus::TooFewPoints);
        EXPECT_TRUE(std::isnan(few.rmsDeviation));
        const FitResult fewUser = fitCurve(
            gaussian, std::span<const double>(x).first(2), std::span<const double>(y).first(2), options(dialect, FitStart::FromCurrent));
        EXPECT_EQ(fewUser.status, FitStatus::TooFewPoints);
        EXPECT_EQ(fewUser.method, FitMethod::None);

        FitFunction root;
        root.name = "Root";
        root.expression = "A*sqrt(x-B)";
        root.parameters = {parameter("A", 1.0), parameter("B", 2.0)};
        root.model = [](double xi, std::span<const double> p)
        {
            return p[0] * std::sqrt(xi - p[1]);
        };
        const FitResult undefined = fitCurve(root, x, y, options(dialect, FitStart::FromCurrent));
        EXPECT_EQ(undefined.status, FitStatus::UndefinedAtStart);
        EXPECT_TRUE(undefined.nonRealValues);
        EXPECT_TRUE(std::isnan(undefined.rmsDeviation));
    }
}

TEST(CurveFit, DegenerateAndInvalidData)
{
    const std::vector<double> sameX(5, 0.7);
    const auto y = grid(1, 0.25, 5);
    const FitResult corrected = fitCurve(builtInFit(BuiltInFit::Parabola), sameX, y, options(FitDialect::Corrected));
    EXPECT_EQ(corrected.status, FitStatus::DegenerateData);
    EXPECT_EQ(corrected.parameters, (std::vector<double>{0, 0, 0}));
    EXPECT_TRUE(corrected.uncertainties.empty());
    const FitResult tracker = fitCurve(builtInFit(BuiltInFit::Line),
                                       std::span<const double>(sameX).first(4),
                                       std::span<const double>(y).first(4),
                                       options(FitDialect::Tracker));
    EXPECT_EQ(tracker.status, FitStatus::DegenerateData);
    EXPECT_TRUE(std::isnan(tracker.parameters[0]));

    // Non-finite rows: Tracker repeats the last rows (Dataset.append), Corrected drops them.
    const std::vector<double> x = {0, 1, NOT_A_NUMBER, 3, 4, 5};
    const std::vector<double> yy = {1, 3, 5, INFINITE, 9, 11};
    EXPECT_EQ(fitCurve(builtInFit(BuiltInFit::Line), x, yy, options(FitDialect::Tracker)).points, 6U);
    const FitResult finite = fitCurve(builtInFit(BuiltInFit::Line), x, yy, options(FitDialect::Corrected));
    EXPECT_EQ(finite.points, 4U);
    expectNear(std::vector<double>{2, 1}, finite.parameters, 1e-12);

    const FitResult empty = fitCurve(builtInFit(BuiltInFit::Line), std::span<const double>{}, std::span<const double>{});
    EXPECT_EQ(empty.points, 0U);
    EXPECT_TRUE(std::isnan(empty.rmsDeviation));
    EXPECT_TRUE(std::isnan(empty.correlation));
}

TEST(CurveFit, ConstantDataDiscardsUncertainties)
{
    // A Gaussian cannot reproduce constant data exactly, so its uncertainties exist before
    // doLinearRegression discards them; Tracker keeps drawing the band, Corrected drops it.
    const auto x = grid(0, 0.1, 12);
    const std::vector<double> y(12, 1.5);
    const FitFunction gaussian = withValues(builtInFit(BuiltInFit::Gaussian), std::vector<double>{1.4, 0.55, 2.0});
    const FitResult tracker = fitCurve(gaussian, x, y, options(FitDialect::Tracker, FitStart::FromCurrent));
    const FitResult corrected = fitCurve(gaussian, x, y, options(FitDialect::Corrected, FitStart::FromCurrent));
    EXPECT_TRUE(tracker.uncertainties.empty());
    EXPECT_TRUE(corrected.uncertainties.empty());
    EXPECT_TRUE(std::isnan(tracker.correlation));
    EXPECT_FALSE(tracker.uncertaintyBand.empty());
    EXPECT_TRUE(corrected.uncertaintyBand.empty());
}

TEST(CurveFit, ScratchFallbackSuppliesTheDampedSineRate)
{
    const FitFunction damped = builtInFit(BuiltInFit::DampedSine);
    const auto x = grid(0, 0.05, 60);
    const auto y = evaluate(damped, std::vector<double>{1.5, 5.0, 0.2, 0.05, -0.5}, x);
    for (const FitDialect dialect : DIALECTS)
    {
        FitOptions guess = options(dialect);
        guess.autofit = false;
        EXPECT_EQ(fitCurve(damped, x, y, guess).parameters[4], 0.0);
        guess.scratchFallback = {1, 1, 0, 0, -0.4};
        const FitResult result = fitCurve(damped, x, y, guess);
        EXPECT_EQ(result.parameters[4], -0.4);
        EXPECT_NE(result.parameters[0], 1.0);
    }
}

TEST(CurveFit, FixedParametersKeepTheirValues)
{
    const auto x = grid(0, 0.2, 16);
    const FitFunction exponential = builtInFit(BuiltInFit::Exponential);
    const auto y = wiggle(evaluate(exponential, std::vector<double>{2.0, -1.2, 0.4}, x), 0.01);
    for (const FitDialect dialect : DIALECTS)
    {
        FitOptions fixedOffset = options(dialect, FitStart::FromCurrent);
        fixedOffset.fixed = {false, false, true};
        const FitResult result = fitCurve(withValues(exponential, std::vector<double>{1.8, -1.0, 0.4}), x, y, fixedOffset);
        EXPECT_EQ(result.status, FitStatus::Fitted);
        EXPECT_EQ(result.parameters[2], 0.4);
        ASSERT_EQ(result.uncertainties.size(), 3U);
        EXPECT_TRUE(std::isnan(result.uncertainties[2]));
        EXPECT_EQ(result.uncertaintyStatus[2], UncertaintyStatus::Fixed);
        EXPECT_EQ(result.uncertaintyBand.size(), 4U);
        for (const auto& set : result.uncertaintyBand)
        {
            EXPECT_EQ(set[2], 0.4);
        }
    }
}

TEST(CurveFit, CancellationStopsBeforeOrDuringTheFit)
{
    const auto x = grid(0, 0.1, 30);
    const FitFunction sine = builtInFit(BuiltInFit::Sinusoid);
    const auto y = wiggle(evaluate(sine, std::vector<double>{1, 3, 0.5, 0}, x), 0.02);
    FitOptions immediately;
    immediately.cancelled = []
    {
        return true;
    };
    const FitResult stopped = fitCurve(sine, x, y, immediately);
    EXPECT_EQ(stopped.status, FitStatus::Cancelled);
    EXPECT_EQ(stopped.parameters, (std::vector<double>{1, 1, 0, 0}));
    for (const int polls : {1, 2, 5})
    {
        int calls = 0;
        FitOptions later;
        later.cancelled = [&calls, polls]
        {
            return ++calls > polls;
        };
        EXPECT_EQ(fitCurve(sine, x, y, later).status, FitStatus::Cancelled) << polls;
    }
    FitOptions never;
    never.cancelled = []
    {
        return false;
    };
    EXPECT_EQ(fitCurve(sine, x, y, never).status, FitStatus::Fitted);
}

TEST(CurveFit, CancellationIsPolledDuringAMinimization)
{
    // A large damped-sine fit: once the callback returns true, the fit ends within a few thousand model
    // evaluations rather than after the minimizer or the uncertainty refits finish.
    const auto x = grid(0, 0.001, 5000);
    const FitFunction damped = builtInFit(BuiltInFit::DampedSine);
    const auto y = wiggle(evaluate(damped, std::vector<double>{2, 6, 0.3, 0.1, -0.8}, x), 0.05);
    for (const FitDialect dialect : DIALECTS)
    {
        for (const std::size_t limit : {std::size_t{20'000}, std::size_t{200'000}, std::size_t{2'000'000}})
        {
            std::size_t evaluations = 0;
            FitFunction counted = damped;
            counted.model = [&evaluations, model = damped.model](double xi, std::span<const double> p)
            {
                ++evaluations;
                return model(xi, p);
            };
            FitOptions stop = options(dialect);
            stop.cancelled = [&evaluations, limit]
            {
                return evaluations > limit;
            };
            const FitResult result = fitCurve(counted, x, y, stop);
            EXPECT_EQ(result.status, FitStatus::Cancelled) << limit;
            EXPECT_LE(evaluations, limit + 4096 + (3 * x.size())) << limit;
        }
    }
}

/// Whether the fitted model is defined at every x.
bool definedEverywhere(const FitFunction& f, const FitResult& result, std::span<const double> x)
{
    return std::ranges::none_of(x, [&](double xi) { return std::isnan(f.model(xi, result.parameters)); });
}

TEST(CurveFit, NonRealValuesAreReported)
{
    // Power with negative x is undefined for a fractional exponent. OSP minimizes with those values as 0
    // and reports the fit; Corrected rejects every exponent that leaves the model undefined on the data.
    const auto x = grid(-1, 0.25, 12);
    const auto y = wiggle(grid(-1.7, 0.4, 12), 0.05);
    const FitFunction power = builtInFit(BuiltInFit::Power);
    const FitResult tracker = fitCurve(power, x, y, options(FitDialect::Tracker));
    EXPECT_EQ(tracker.status, FitStatus::Fitted);
    EXPECT_TRUE(tracker.nonRealValues);
    EXPECT_TRUE(std::isnan(tracker.rmsDeviation));
    const FitResult corrected = fitCurve(power, x, y, options(FitDialect::Corrected));
    EXPECT_TRUE(corrected.status == FitStatus::NotImproved || corrected.status == FitStatus::UndefinedAtStart);
    EXPECT_EQ(corrected.nonRealValues, !definedEverywhere(power, corrected, x));
    // A start that is defined everywhere stays defined.
    const FitResult integer =
        fitCurve(withValues(power, std::vector<double>{1, 2}), x, y, options(FitDialect::Corrected, FitStart::FromCurrent));
    EXPECT_TRUE(definedEverywhere(power, integer, x));
    EXPECT_FALSE(integer.nonRealValues);
    EXPECT_FALSE(std::isnan(integer.rmsDeviation));
}

TEST(CurveFit, TrackerKeepsTrackersRowCapacity)
{
    // Tracker's data sets keep the last 32 * 16384 rows; Corrected keeps every row.
    constexpr std::size_t CAPACITY = std::size_t{32} * 16384;
    const auto x = grid(0, 1, CAPACITY + 10);
    std::vector<double> y;
    y.reserve(x.size());
    for (const double xi : x)
    {
        y.push_back(xi < 10 ? 50.0 : (2 * xi) + 1);
    }
    const FitFunction line = withValues(builtInFit(BuiltInFit::Line), std::vector<double>{2, 1});
    FitOptions manual = options(FitDialect::Tracker, FitStart::FromCurrent);
    manual.autofit = false;
    const FitResult tracker = fitCurve(line, x, y, manual);
    EXPECT_EQ(tracker.status, FitStatus::Manual);
    EXPECT_EQ(tracker.points, CAPACITY);
    EXPECT_EQ(tracker.rmsDeviation, 0.0);
    manual.dialect = FitDialect::Corrected;
    const FitResult corrected = fitCurve(line, x, y, manual);
    EXPECT_EQ(corrected.points, CAPACITY + 10);
    EXPECT_GT(corrected.rmsDeviation, 0.0);
    // Fitted, Corrected's line is the least-squares line through every row, the first ten included. With
    // x = 0 ... n - 1 and y = 2x + 1 + d, slope = 2 + sum (x - mean x) d / Sxx with Sxx = n (n^2 - 1) / 12,
    // and intercept = 1 + mean d - (slope - 2) mean x (exactly 1.9999999912693367 and 1.003051655188007).
    const auto n = static_cast<double>(x.size());
    const double meanX = (n - 1) / 2;
    double moment = 0;
    double offsets = 0;
    for (std::size_t i = 0; i < 10; ++i)
    {
        const double d = y[i] - ((2 * x[i]) + 1);
        moment += (x[i] - meanX) * d;
        offsets += d;
    }
    const double tilt = moment / (n * ((n * n) - 1) / 12);
    const FitResult fitted = fitCurve(line, x, y, options(FitDialect::Corrected));
    EXPECT_EQ(fitted.status, FitStatus::Fitted);
    ASSERT_EQ(fitted.parameters.size(), 2U);
    EXPECT_NEAR(fitted.parameters[0], 2 + tilt, 1e-13);
    // The intercept cancels terms of size slope * mean x (5e5) when it leaves the scaled coordinates.
    EXPECT_NEAR(fitted.parameters[1], 1 + (offsets / n) - (tilt * meanX), 1e-8);
    EXPECT_LT(fitted.parameters[0], 2 - 1e-9);
    EXPECT_GT(fitted.parameters[1], 1.003);
}

TEST(CurveFit, CorrectedNeverFitsIntoUndefinedValues)
{
    // A*sqrt(x-B) from (1, 0): every B above 1 leaves the first points undefined.
    FitFunction root;
    root.expression = "A*sqrt(x-B)";
    root.parameters = {parameter("A", 1), parameter("B", 0)};
    root.model = [](double x, std::span<const double> p)
    {
        return p[0] * std::sqrt(x - p[1]);
    };
    const auto x = grid(1, 0.5, 20);
    const auto y = wiggle(grid(0.1, 0.2, 20), 0.02);
    const FitResult tracker = fitCurve(root, x, y, options(FitDialect::Tracker, FitStart::FromCurrent));
    EXPECT_EQ(tracker.status, FitStatus::Fitted);
    EXPECT_FALSE(definedEverywhere(root, tracker, x));
    EXPECT_TRUE(std::isnan(tracker.rmsDeviation));
    const FitResult corrected = fitCurve(root, x, y, options(FitDialect::Corrected, FitStart::FromCurrent));
    EXPECT_EQ(corrected.status, FitStatus::Fitted);
    EXPECT_TRUE(definedEverywhere(root, corrected, x));
    EXPECT_FALSE(corrected.nonRealValues);
    EXPECT_LT(corrected.parameters[1], 1.0);
    double sum = 0;
    for (std::size_t i = 0; i < x.size(); ++i)
    {
        const double deviation = y[i] - root.model(x[i], corrected.parameters);
        sum += deviation * deviation;
    }
    EXPECT_DOUBLE_EQ(corrected.rmsDeviation, std::sqrt(sum / static_cast<double>(x.size())));
}

TEST(CurveFit, CorrectedTreatsInfiniteValuesAsUndefined)
{
    // From (1, 100, 0), exp(100 x) overflows to +inf for x > 7.1, so the start is undefined at the last six
    // points. OSP passes the infinity through: Tracker minimizes from an infinite deviation, cannot leave it,
    // and reports a fit with an infinite rms and no warning. Corrected treats +inf like NaN (#311).
    const auto x = grid(0, 0.5, 21);
    FitFunction exponential = builtInFit(BuiltInFit::Exponential);
    const auto y = wiggle(evaluate(exponential, std::vector<double>{1.5, -0.3, 0.2}, x), 0.01);
    exponential = withValues(exponential, std::vector<double>{1, 100, 0});
    ASSERT_TRUE(std::isinf(exponential.model(x.back(), std::vector<double>{1, 100, 0})));
    const FitResult tracker = fitCurve(exponential, x, y, options(FitDialect::Tracker, FitStart::FromCurrent));
    EXPECT_EQ(tracker.status, FitStatus::Fitted);
    EXPECT_TRUE(std::isinf(tracker.rmsDeviation));
    EXPECT_FALSE(tracker.nonRealValues);
    const FitResult corrected = fitCurve(exponential, x, y, options(FitDialect::Corrected, FitStart::FromCurrent));
    EXPECT_EQ(corrected.status, FitStatus::UndefinedAtStart);
    EXPECT_EQ(corrected.method, FitMethod::None);
    EXPECT_TRUE(corrected.nonRealValues);
    EXPECT_TRUE(std::isnan(corrected.rmsDeviation));
    EXPECT_EQ(corrected.parameters, (std::vector<double>{1, 100, 0}));
    EXPECT_TRUE(corrected.uncertainties.empty());
    // A start defined everywhere fits the same data.
    const FitResult fitted =
        fitCurve(withValues(exponential, std::vector<double>{1, -1, 0}), x, y, options(FitDialect::Corrected, FitStart::FromCurrent));
    EXPECT_EQ(fitted.status, FitStatus::Fitted);
    EXPECT_FALSE(fitted.nonRealValues);
    EXPECT_TRUE(std::isfinite(fitted.rmsDeviation));
    expectNear(std::vector<double>{1.5, -0.3, 0.2}, fitted.parameters, 0.05);
}

TEST(CurveFit, CorrectedTreatsAPowerPoleAsUndefined)
{
    // A*x^B from (1, -1) on data that include x = 0: Math.pow(0, -1) = +inf at the first point.
    const auto x = grid(0, 0.25, 12);
    const FitFunction power = withValues(builtInFit(BuiltInFit::Power), std::vector<double>{1, -1});
    const auto y = wiggle(grid(0.5, 0.2, 12), 0.02);
    ASSERT_TRUE(std::isinf(power.model(0.0, std::vector<double>{1, -1})));
    const FitResult tracker = fitCurve(power, x, y, options(FitDialect::Tracker, FitStart::FromCurrent));
    EXPECT_NE(tracker.status, FitStatus::UndefinedAtStart);
    EXPECT_TRUE(std::isinf(tracker.rmsDeviation));
    EXPECT_FALSE(tracker.nonRealValues);
    const FitResult corrected = fitCurve(power, x, y, options(FitDialect::Corrected, FitStart::FromCurrent));
    EXPECT_EQ(corrected.status, FitStatus::UndefinedAtStart);
    EXPECT_EQ(corrected.method, FitMethod::None);
    EXPECT_TRUE(corrected.nonRealValues);
    EXPECT_TRUE(std::isnan(corrected.rmsDeviation));
    EXPECT_EQ(corrected.parameters, (std::vector<double>{1, -1}));
    EXPECT_TRUE(corrected.uncertainties.empty());
}

/// Gauss-Newton least squares for A*exp(B*x)+C, solving the 3x3 normal equations by Cramer's rule.
std::vector<double> exponentialLeastSquares(std::span<const double> x, std::span<const double> y, std::vector<double> p)
{
    for (int iteration = 0; iteration < 100; ++iteration)
    {
        std::array<std::array<double, 4>, 3> normal{};
        for (std::size_t i = 0; i < x.size(); ++i)
        {
            const double e = std::exp(p[1] * x[i]);
            const std::array<double, 3> gradient = {e, p[0] * x[i] * e, 1.0};
            const double residual = y[i] - ((p[0] * e) + p[2]);
            for (std::size_t r = 0; r < 3; ++r)
            {
                for (std::size_t c = 0; c < 3; ++c)
                {
                    normal[r][c] += gradient[r] * gradient[c];
                }
                normal[r][3] += gradient[r] * residual;
            }
        }
        const auto determinant = [&normal](std::size_t replaced)
        {
            std::array<std::array<double, 3>, 3> m{};
            for (std::size_t r = 0; r < 3; ++r)
            {
                for (std::size_t c = 0; c < 3; ++c)
                {
                    m[r][c] = normal[r][c == replaced ? 3 : c];
                }
            }
            return (m[0][0] * ((m[1][1] * m[2][2]) - (m[1][2] * m[2][1]))) - (m[0][1] * ((m[1][0] * m[2][2]) - (m[1][2] * m[2][0]))) +
                   (m[0][2] * ((m[1][0] * m[2][1]) - (m[1][1] * m[2][0])));
        };
        const double full = determinant(3);
        for (std::size_t k = 0; k < 3; ++k)
        {
            p[k] += determinant(k) / full;
        }
    }
    return p;
}

TEST(CurveFit, CorrectedKeepsEachMinimizersBestVector)
{
    // Exponential decay with N(0, 0.15) noise (mt19937_64 seeds 1004 and 1008, MSVC's normal_distribution),
    // fitted from scratch. Tracker keeps the last vector Newton evaluated: for seed 1004 a rejected one with
    // twice the deviation of Newton's best, for seed 1008 it restores the start although Newton improved it.
    const std::vector<std::pair<int, std::vector<double>>> datasets = {
        {1004,
         {1.9946816534969869,  1.6306392391790667,  1.1207456988462838,  0.98078120911700117, 0.94618514437685608, 0.73758554128610043,
          0.78467773244802841, 0.56986691816074264, 0.69963565729981103, 0.58437649497472333, 0.25548329339118536, 0.43175015851163023,
          0.51059671799503292, 0.73526458205464951, 0.38419026203740936, 0.37086295563893912, 0.24353157075608994, 0.54896028791349594,
          0.14204507279035641, 0.24177921201921659, 0.37305856464576354}},
        {1008,
         {1.8893983046132545,  1.4226391487150523,  1.2073537394716365,  1.1800108560899472,  1.0071897776340688,  0.84025666532458931,
          0.88296205929575355, 0.44779481277851529, 0.68907246467601968, 0.6530300745917923,  0.54645470920045569, 0.33375085806001714,
          0.20717174698185953, 0.41329035671084996, 0.57461282860075114, 0.10440996783372428, 0.56231074825176286, 0.54149173161255471,
          0.15905831282811889, 0.33711862978386287, 0.11559963826003875}},
    };
    const FitFunction exponential = builtInFit(BuiltInFit::Exponential);
    const auto x = grid(0, 0.1, 21);
    for (const auto& [seed, y] : datasets)
    {
        SCOPED_TRACE(seed);
        const std::vector<double> minimum = exponentialLeastSquares(x, y, {1.5, -2.0, 0.25});
        const FitResult result = fitCurve(exponential, x, y, options(FitDialect::Corrected));
        EXPECT_EQ(result.status, FitStatus::Fitted);
        // The minimizers stop at Tracker's tolerances, close to the exact minimum.
        expectNear(minimum, result.parameters, 1e-5);
        ASSERT_EQ(result.uncertaintyStatus.size(), 3U);
        EXPECT_TRUE(std::ranges::all_of(result.uncertaintyStatus, [](UncertaintyStatus s) { return s == UncertaintyStatus::Estimated; }));
        // The reported deviation is that of the reported parameters.
        double sum = 0;
        for (std::size_t i = 0; i < x.size(); ++i)
        {
            const double deviation = y[i] - exponential.model(x[i], result.parameters);
            sum += deviation * deviation;
        }
        EXPECT_DOUBLE_EQ(result.rmsDeviation, std::sqrt(sum / 21));
        double minimumSum = 0;
        for (std::size_t i = 0; i < x.size(); ++i)
        {
            const double deviation = y[i] - exponential.model(x[i], minimum);
            minimumSum += deviation * deviation;
        }
        EXPECT_NEAR(sum, minimumSum, 1e-9 * minimumSum);
        const FitResult tracker = fitCurve(exponential, x, y, options(FitDialect::Tracker));
        EXPECT_GT(tracker.rmsDeviation, result.rmsDeviation * 1.1);
    }
}

TEST(CurveFit, DegenerateDirectionsAreUndetermined)
{
    const auto x = grid(1, 1, 10);
    std::vector<double> y;
    y.reserve(x.size());
    for (const double xi : x)
    {
        y.push_back(2 * xi);
    }
    y = wiggle(y, 0.1);
    double sxy = 0;
    double sxx = 0;
    for (std::size_t i = 0; i < x.size(); ++i)
    {
        sxy += x[i] * y[i];
        sxx += x[i] * x[i];
    }
    const double slope = sxy / sxx;
    // B has no effect: Newton's Hessian is singular, Levenberg-Marquardt fits A, and B stays undetermined.
    FitFunction unused;
    unused.expression = "A*x+0*B";
    unused.parameters = {parameter("A", 1), parameter("B", 1)};
    unused.model = [](double xi, std::span<const double> p)
    {
        return (p[0] * xi) + (0 * p[1]);
    };
    const FitResult tracker = fitCurve(unused, x, y, options(FitDialect::Tracker, FitStart::FromCurrent));
    EXPECT_EQ(tracker.status, FitStatus::Fitted);
    EXPECT_EQ(tracker.parameters[0], 1.0);
    const FitResult fitted = fitCurve(unused, x, y, options(FitDialect::Corrected, FitStart::FromCurrent));
    EXPECT_EQ(fitted.status, FitStatus::Fitted);
    EXPECT_NEAR(fitted.parameters[0], slope, 1e-9);
    ASSERT_EQ(fitted.uncertaintyStatus.size(), 2U);
    EXPECT_EQ(fitted.uncertaintyStatus[0], UncertaintyStatus::Estimated);
    EXPECT_EQ(fitted.uncertaintyStatus[1], UncertaintyStatus::Undetermined);
    double residuals = 0;
    for (std::size_t i = 0; i < x.size(); ++i)
    {
        residuals += (y[i] - (slope * x[i])) * (y[i] - (slope * x[i]));
    }
    // The calibration counts B among the free parameters: sigma_y^2 = SSR / (N - 2).
    EXPECT_NEAR(fitted.uncertainties[0], std::sqrt(residuals / 8 / sxx), 1e-3 * fitted.uncertainties[0]);
    // Only A*B is determined: neither factor has an uncertainty.
    FitFunction product;
    product.expression = "A*B*x";
    product.parameters = {parameter("A", 1), parameter("B", 1)};
    product.model = [](double xi, std::span<const double> p)
    {
        return p[0] * p[1] * xi;
    };
    const FitResult factors = fitCurve(product, x, y, options(FitDialect::Corrected, FitStart::FromCurrent));
    EXPECT_EQ(factors.status, FitStatus::Fitted);
    EXPECT_NEAR(factors.parameters[0] * factors.parameters[1], slope, 1e-6);
    EXPECT_EQ(factors.uncertaintyStatus,
              (std::vector<UncertaintyStatus>{UncertaintyStatus::Undetermined, UncertaintyStatus::Undetermined}));
    EXPECT_TRUE(factors.uncertaintyBand.empty());
}

TEST(CurveFit, CorrectedReportsAnUnconvergedImprovement)
{
    // A Rosenbrock valley 10^6 times narrower than long, as two residuals: the finite-difference minimizers
    // lower the deviation but cannot follow the valley to its minimum at (1, 1).
    FitFunction valley;
    valley.expression = "valley";
    valley.parameters = {parameter("a", -1.2), parameter("b", 1.0)};
    valley.model = [](double x, std::span<const double> p)
    {
        return x == 0 ? 1e6 * (p[1] - (p[0] * p[0])) : p[0];
    };
    const std::vector<double> x = {0, 1, 0, 1};
    const std::vector<double> y = {0, 1, 0, 1};
    const FitResult tracker = fitCurve(valley, x, y, options(FitDialect::Tracker, FitStart::FromCurrent));
    EXPECT_EQ(tracker.status, FitStatus::NotImproved);
    const FitResult result = fitCurve(valley, x, y, options(FitDialect::Corrected, FitStart::FromCurrent));
    EXPECT_EQ(result.status, FitStatus::NotConverged);
    EXPECT_EQ(result.method, FitMethod::LevenbergMarquardt);
    EXPECT_LT(result.rmsDeviation, tracker.rmsDeviation);
    EXPECT_GT(std::abs(result.parameters[0] - 1), 0.1);
    EXPECT_EQ(result.uncertainties, (std::vector<double>{0, 0}));
    EXPECT_EQ(result.uncertaintyStatus, (std::vector<UncertaintyStatus>{UncertaintyStatus::Undetermined, UncertaintyStatus::Undetermined}));
    EXPECT_TRUE(result.uncertaintyBand.empty());
}

TEST(CurveFit, CorrectedRunsLevenbergMarquardtFromNewtonsBestVector)
{
    // From (0.5, 0, 1) Newton improves this Lorentzian without converging, so Corrected restarts
    // Levenberg-Marquardt from Newton's best vector until it reaches its tolerance. The same calls,
    // made here directly (this file rounds like the kernel), give the same parameters bit for bit.
    const auto x = grid(-1, 0.125, 25);
    const std::vector<double> y = {
        0.073834932435113562, 0.097634224398582589, 0.083520924240653646, 0.097707775081886133, 0.15304091926405597,
        0.18467131683080823,  0.20394454633902812,  0.28522120115729394,  0.41944015002789953,  0.57245210274895919,
        0.82706333490795148,  1.2829975441932591,   1.8192230544900421,   1.9782557828892955,   1.5828258599478016,
        1.0991691532725893,   0.73398480063310179,  0.47632726273449993,  0.3413526922959863,   0.27915648457650644,
        0.20509364665688071,  0.13797264210921989,  0.1274513914546983,   0.12474903249116806,  0.082676884656120248};
    FitFunction lorentz;
    lorentz.expression = "A/(1+((x-B)/C)^2)";
    lorentz.parameters = {parameter("A", 0.5), parameter("B", 0), parameter("C", 1)};
    lorentz.model = [](double xi, std::span<const double> p)
    {
        const double u = (xi - p[1]) / p[2];
        return p[0] / (1 + (u * u));
    };
    const auto objective = [&](std::span<const double> p)
    {
        double sum = 0;
        for (std::size_t i = 0; i < x.size(); ++i)
        {
            const double deviation = y[i] - lorentz.model(x[i], p);
            sum += deviation * deviation;
        }
        return sum;
    };
    using motrack::science::TRACKER_FIT_ITERATIONS;
    using motrack::science::TRACKER_FIT_TOLERANCE;
    const std::vector<double> start = {0.5, 0, 1};
    std::vector<double> p = start;
    const auto newton = motrack::science::hessianMinimize(objective, p, TRACKER_FIT_ITERATIONS, TRACKER_FIT_TOLERANCE);
    ASSERT_FALSE(newton.reachedTolerance && newton.keptLastIterate);
    ASSERT_NE(p, start);
    double deviation = objective(p);
    bool converged = false;
    for (int run = 0; run < 100 && !converged; ++run)
    {
        const double before = deviation;
        converged =
            motrack::science::levenbergMarquardtMinimize(objective, p, TRACKER_FIT_ITERATIONS, TRACKER_FIT_TOLERANCE).reachedTolerance;
        deviation = objective(p);
        if (!(deviation < before))
        {
            break;
        }
    }
    ASSERT_TRUE(converged);
    const FitResult result = fitCurve(lorentz, x, y, options(FitDialect::Corrected, FitStart::FromCurrent));
    EXPECT_EQ(result.status, FitStatus::Fitted);
    EXPECT_EQ(result.method, FitMethod::LevenbergMarquardt);
    EXPECT_EQ(result.parameters, p);
    expectNear(std::vector<double>{2, 0.6, 0.3}, result.parameters, 0.01);
}

TEST(CurveFit, CorrectedKeepsAnUnimprovableStartWithoutUncertainties)
{
    // Every move of the exponent leaves Power undefined at negative x, so nothing lowers the deviation.
    const auto x = grid(-1, 0.25, 12);
    const auto y = wiggle(grid(-1.7, 0.4, 12), 0.05);
    FitFunction power = builtInFit(BuiltInFit::Power);
    const FitResult result =
        fitCurve(withValues(power, std::vector<double>{1, 2}), x, y, options(FitDialect::Corrected, FitStart::FromCurrent));
    ASSERT_EQ(result.status, FitStatus::NotImproved);
    EXPECT_EQ(result.parameters, (std::vector<double>{1, 2}));
    EXPECT_EQ(result.uncertainties, (std::vector<double>{0, 0}));
    EXPECT_EQ(result.uncertaintyStatus, (std::vector<UncertaintyStatus>{UncertaintyStatus::Undetermined, UncertaintyStatus::Undetermined}));
    EXPECT_TRUE(result.uncertaintyBand.empty());
}

TEST(CurveFit, SubstitutionCollisions)
{
    using motrack::science::trackerSubstitutionCollisions;
    for (const BuiltInFit kind : ALL_BUILT_IN_FITS)
    {
        EXPECT_TRUE(trackerSubstitutionCollisions(builtInFit(kind)).empty());
    }
    FitFunction cosine;
    cosine.expression = "a*cos(b*x)+c";
    cosine.parameters = {parameter("a", 1), parameter("b", 2), parameter("c", 0)};
    EXPECT_EQ(trackerSubstitutionCollisions(cosine), (std::vector<std::string>{"c"}));
    EXPECT_EQ(trackerSubstitutionCollisions(cosine, {false, false, true}), (std::vector<std::string>{"c"}));
    FitFunction damped = builtInFit(BuiltInFit::DampedSine);
    damped.parameters[3].value = 1.0e-4;
    EXPECT_EQ(trackerSubstitutionCollisions(damped, {false, false, false, true, false}), (std::vector<std::string>{"E"}));
    damped.parameters[3].value = 0.5;
    EXPECT_TRUE(trackerSubstitutionCollisions(damped, {false, false, false, true, false}).empty());
    damped.parameters[3].value = -2.5e7;
    EXPECT_EQ(trackerSubstitutionCollisions(damped, {false, false, false, true, false}), (std::vector<std::string>{"E"}));
    FitFunction exponent;
    exponent.expression = "A+E7*x";
    exponent.parameters = {parameter("A", 1.0e7), parameter("E7", 1)};
    EXPECT_EQ(trackerSubstitutionCollisions(exponent, {true, false}), (std::vector<std::string>{"E7"}));
    exponent.parameters[0].value = NOT_A_NUMBER;
    EXPECT_TRUE(trackerSubstitutionCollisions(exponent, {true, false}).empty());
    exponent.parameters[0].value = 0.0;
    EXPECT_TRUE(trackerSubstitutionCollisions(exponent, {true, false}).empty());
    FitFunction prefix;
    prefix.expression = "k*x+k2";
    prefix.parameters = {parameter("k", 1), parameter("k2", 2), parameter("", 0)};
    EXPECT_EQ(trackerSubstitutionCollisions(prefix), (std::vector<std::string>{"k"}));
}

/// A user fit compiled by the K1 expression kernel over {x, parameters...}, as slice I4 binds it.
FitFunction expressionFit(const std::string& text, std::vector<motrack::science::FitParameter> parameters)
{
    std::vector<std::string> names{"x"};
    for (const auto& p : parameters)
    {
        names.push_back(p.name);
    }
    const auto compiled = motrack::science::parseExpression(text, names);
    EXPECT_TRUE(compiled.hasValue()) << text;
    FitFunction f;
    f.name = "user";
    f.expression = text;
    f.parameters = std::move(parameters);
    if (compiled)
    {
        f.model = [expression = *compiled](double x, std::span<const double> p)
        {
            std::vector<double> values{x};
            values.insert(values.end(), p.begin(), p.end());
            return expression.evaluate(values);
        };
    }
    return f;
}

/// Tracker's Fit Builder parses user fits through OSP's UserFunction, which rejects a `+` exponent and
/// fits the constant 0 instead (#215). MoTrack fits the model as written: the same parameters as for the
/// plus-free spelling, bit for bit, and the synthetic ones that generated the data.
TEST(CurveFit, UserFitWithAPlusExponentLiteral)
{
    struct Case
    {
        std::string text;
        std::string plusFree;
        std::vector<std::string> names;
        std::vector<double> truth;
        std::vector<double> y;
    };
    const auto x = grid(0, 0.1, 21);
    std::vector<double> line;
    std::vector<double> decay;
    line.reserve(x.size());
    decay.reserve(x.size());
    for (const double xi : x)
    {
        line.push_back((0.0025 * xi * 1000.0) - 1.5);
        decay.push_back((2.0 * std::exp(-xi * 10.0 / 3.0)) + 0.5);
    }
    const std::vector<Case> cases = {
        {.text = "m*x*1e+3+b", .plusFree = "m*x*1e3+b", .names = {"m", "b"}, .truth = {0.0025, -1.5}, .y = line},
        {.text = "A*exp(-x*1e+1/T)+C", .plusFree = "A*exp(-x*1e1/T)+C", .names = {"A", "T", "C"}, .truth = {2.0, 3.0, 0.5}, .y = decay},
    };
    for (const Case& c : cases)
    {
        SCOPED_TRACE(c.text);
        std::vector<std::string_view> names{"x"};
        names.insert(names.end(), c.names.begin(), c.names.end());
        const auto parsed = motrack::science::parseExpression(c.text, names);
        const auto parsedPlain = motrack::science::parseExpression(c.plusFree, names);
        ASSERT_TRUE(parsed.hasValue() && parsedPlain.hasValue());
        EXPECT_TRUE(parsed->trackerDifferences().plusExponentSign); // I4 warns that Tracker fits 0 here.
        EXPECT_FALSE(parsedPlain->trackerDifferences().any());
        std::vector<motrack::science::FitParameter> start;
        start.reserve(c.names.size());
        for (std::size_t i = 0; i < c.names.size(); ++i)
        {
            start.push_back(parameter(c.names[i], c.truth[i] * 1.03));
        }
        const FitFunction plus = expressionFit(c.text, start);
        const FitFunction plain = expressionFit(c.plusFree, start);
        ASSERT_TRUE(plus.model && plain.model);
        for (const FitDialect dialect : DIALECTS)
        {
            SCOPED_TRACE(dialect == FitDialect::Tracker ? "tracker" : "corrected");
            const FitResult result = fitCurve(plus, x, c.y, options(dialect, FitStart::FromCurrent));
            EXPECT_TRUE(result.status == FitStatus::Fitted || result.status == FitStatus::NotImproved);
            expectNear(c.truth, result.parameters, 1e-6);
            EXPECT_LT(result.rmsDeviation, 1e-6);
            EXPECT_FALSE(result.nonRealValues);
            const FitResult reference = fitCurve(plain, x, c.y, options(dialect, FitStart::FromCurrent));
            EXPECT_EQ(result.parameters, reference.parameters);
            EXPECT_EQ(result.status, reference.status);
        }
    }
}

TEST(CurveFit, UncertaintyText)
{
    using motrack::science::formatUncertainParameter;
    EXPECT_EQ(formatUncertainParameter(2.0100000000000002, 0.041231056256139244), "2.01 \xC2\xB1 0.04");
    EXPECT_EQ(formatUncertainParameter(-0.0, 0.25), "-0.0 \xC2\xB1 0.3");
    EXPECT_EQ(formatUncertainParameter(9.99, 0.004), "9.990 \xC2\xB1 0.004");
    EXPECT_EQ(formatUncertainParameter(0.996, 0.004), "(9.96 \xC2\xB1 0.04) E-1");
    EXPECT_EQ(formatUncertainParameter(5.0e-7, 0.4), "(0 \xC2\xB1 4) E-1");
    EXPECT_FALSE(formatUncertainParameter(INFINITE, 1.0));
    EXPECT_FALSE(formatUncertainParameter(1.0, INFINITE));
    EXPECT_FALSE(formatUncertainParameter(NOT_A_NUMBER, 1.0));
}

TEST(CurveFit, MinimizersHandleEmptyAndUndefinedObjectives)
{
    std::vector<double> none;
    const auto objective = [](std::span<const double>)
    {
        return 0.0;
    };
    EXPECT_EQ(motrack::science::hessianMinimize(objective, none, 20, 1e-6).iterations, 0);
    EXPECT_FALSE(motrack::science::levenbergMarquardtMinimize(objective, none, 20, 1e-6).converged);
    std::vector<double> x = {0.5};
    const auto undefined = [](std::span<const double>)
    {
        return NOT_A_NUMBER;
    };
    const auto result = motrack::science::hessianMinimize(undefined, x, 20, 1e-6);
    EXPECT_TRUE(std::isnan(result.error));
    EXPECT_FALSE(result.reachedTolerance);
    EXPECT_EQ(x[0], 0.5);
    // OSP's LevenbergMarquardt reports success after a NaN step; reachedTolerance does not.
    const auto lm = motrack::science::levenbergMarquardtMinimize(undefined, x, 20, 1e-6);
    EXPECT_TRUE(lm.converged);
    EXPECT_FALSE(lm.reachedTolerance);
    const auto bowl = [](std::span<const double> p)
    {
        return ((p[0] - 1) * (p[0] - 1)) + 1;
    };
    EXPECT_TRUE(motrack::science::hessianMinimize(bowl, x, 20, 1e-6).reachedTolerance);
    EXPECT_TRUE(motrack::science::levenbergMarquardtMinimize(bowl, x, 20, 1e-6).reachedTolerance);
}

} // namespace
