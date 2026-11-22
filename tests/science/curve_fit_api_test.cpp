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

TEST(CurveFit, NonRealValuesAreReported)
{
    // Power with negative x is undefined for a fractional exponent; OSP minimizes with those values as 0.
    const auto x = grid(-1, 0.25, 12);
    const auto y = wiggle(grid(-1.7, 0.4, 12), 0.05);
    for (const FitDialect dialect : DIALECTS)
    {
        const FitResult result = fitCurve(builtInFit(BuiltInFit::Power), x, y, options(dialect));
        EXPECT_TRUE(result.nonRealValues);
        EXPECT_TRUE(std::isnan(result.rmsDeviation));
    }
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
    EXPECT_EQ(x[0], 0.5);
}

} // namespace
