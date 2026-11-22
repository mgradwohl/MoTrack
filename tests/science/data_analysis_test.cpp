#include "science/analysis/data_analysis.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <optional>
#include <span>
#include <vector>

// Independent expectations for the Data Tool analysis kernel: textbook statistics, analytic
// integrals and slopes, and Fourier coefficients of signals with known frequencies. The Java
// comparison lives in analysis_reference_test.cpp; these tests would fail for a port that matched
// a consistently wrong reference.

namespace
{

using namespace motrack::science;

constexpr double NOT_A_NUMBER = std::numeric_limits<double>::quiet_NaN();
constexpr double INFINITE = std::numeric_limits<double>::infinity();
constexpr double PI = std::numbers::pi;

std::vector<double> uniformTimes(std::size_t count, double start, double step)
{
    std::vector<double> times(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        times[i] = start + static_cast<double>(i) * step;
    }
    return times;
}

std::vector<PlotPoint> pointsOf(std::span<const double> x, std::span<const double> y)
{
    return plotData(x, y).points;
}

TEST(DataAnalysisStatistics, TextbookColumn)
{
    const std::vector<double> values{2, 4, 4, 4, 5, 5, 7, 9};
    const ColumnStatistics statistics = columnStatistics(values);
    EXPECT_EQ(statistics.count, 8U);
    EXPECT_EQ(statistics.maximum, 9.0);
    EXPECT_EQ(statistics.minimum, 2.0);
    EXPECT_EQ(statistics.mean, 5.0);
    // Sum of squared deviations 32: sample sd sqrt(32/7), standard error sd / sqrt(8).
    ASSERT_TRUE(statistics.standardDeviation);
    EXPECT_NEAR(statistics.standardDeviation.value_or(NOT_A_NUMBER), std::sqrt(32.0 / 7.0), 1e-15);
    EXPECT_NEAR(statistics.standardError.value_or(NOT_A_NUMBER), std::sqrt(32.0 / 7.0) / std::sqrt(8.0), 1e-15);
    // Tracker's one-pass value agrees here to rounding, so no correction is indicated.
    EXPECT_FALSE(statistics.correctedFromTracker);
    EXPECT_NEAR(statistics.tracker.standardDeviation.value_or(NOT_A_NUMBER), std::sqrt(32.0 / 7.0), 1e-14);
    EXPECT_FALSE(columnStatistics(std::vector<double>{}).correctedFromTracker);
    EXPECT_FALSE(columnStatistics(std::vector<double>{1.0, INFINITE}).correctedFromTracker);
    EXPECT_TRUE(columnStatistics(std::vector<double>(7, 0.1)).correctedFromTracker);
    const ColumnStatistics extreme = columnStatistics(std::vector<double>{-INFINITE, -INFINITE});
    EXPECT_TRUE(extreme.correctedFromTracker); // Tracker shows max NaN
    EXPECT_FALSE(extreme.tracker.maximum);
}

TEST(DataAnalysisStatistics, MissingValuesAreIgnoredAndNeverZero)
{
    const std::vector<double> values{1.0, NOT_A_NUMBER, 3.0, NOT_A_NUMBER, 5.0, 7.0};
    const ColumnStatistics all = columnStatistics(values);
    EXPECT_EQ(all.count, 4U);
    EXPECT_EQ(all.mean, 4.0);
    EXPECT_NEAR(all.standardDeviation.value_or(NOT_A_NUMBER), std::sqrt(20.0 / 3.0), 1e-15);

    const ColumnStatistics none = columnStatistics(std::vector<double>{NOT_A_NUMBER, NOT_A_NUMBER});
    EXPECT_EQ(none.count, 0U);
    EXPECT_FALSE(none.maximum || none.minimum || none.mean || none.standardDeviation || none.standardError);
    EXPECT_FALSE(columnStatistics(std::vector<double>{}).mean);

    const ColumnStatistics one = columnStatistics(std::vector<double>{2.5});
    EXPECT_EQ(one.count, 1U);
    EXPECT_EQ(one.mean, 2.5);
    EXPECT_FALSE(one.standardDeviation);
    EXPECT_FALSE(one.standardError);
}

TEST(DataAnalysisStatistics, SelectedRowsIncludingRowsPastTheEnd)
{
    const std::vector<double> values{1.0, NOT_A_NUMBER, 3.0, NOT_A_NUMBER, 5.0, 7.0};
    const std::vector<std::size_t> rows{4, 5, 6, 9};
    const ColumnStatistics selected = columnStatistics(values, rows);
    EXPECT_EQ(selected.count, 2U);
    EXPECT_EQ(selected.mean, 6.0);
    EXPECT_EQ(selected.minimum, 5.0);
    EXPECT_NEAR(selected.standardDeviation.value_or(NOT_A_NUMBER), std::sqrt(2.0), 1e-15);
    EXPECT_EQ(columnStatistics(values, std::vector<std::size_t>{1, 3}).count, 0U);
    EXPECT_EQ(columnStatistics(values, std::vector<std::size_t>{}).count, 0U);
}

TEST(DataAnalysisStatistics, ConstantAndOffsetColumnsKeepTheirStandardDeviation)
{
    // OSP's one-pass formula gives 1.5e-9 for seven times 0.1 and 0 for 1e8 + {0, 1, 2}.
    EXPECT_EQ(columnStatistics(std::vector<double>(7, 0.1)).standardDeviation, 0.0);
    EXPECT_EQ(columnStatistics(std::vector<double>(5, 1.0 / 3.0)).standardDeviation, 0.0);
    const ColumnStatistics offset = columnStatistics(std::vector<double>{1e8, 1e8 + 1, 1e8 + 2});
    EXPECT_EQ(offset.standardDeviation, 1.0);
    // Tracker shows 0 here: the correction indicator reports it, with Tracker's value.
    EXPECT_TRUE(offset.correctedFromTracker);
    EXPECT_EQ(offset.tracker.standardDeviation, 0.0);
    EXPECT_EQ(offset.tracker.mean, offset.mean);
    std::vector<double> clock(50);
    for (std::size_t i = 0; i < clock.size(); ++i)
    {
        clock[i] = 1e9 + (0.5 + 0.01 * static_cast<double>(i));
    }
    // Subtracting 1e9 from the stored clock values is exact, so the reference is unshifted data.
    std::vector<double> exact(clock.size());
    std::ranges::transform(clock, exact.begin(), [](double value) { return value - 1e9; });
    EXPECT_NEAR(columnStatistics(clock).standardDeviation.value_or(NOT_A_NUMBER),
                columnStatistics(exact).standardDeviation.value_or(NOT_A_NUMBER),
                1e-15);
    // Storing 1e9 + s rounds s to multiples of 2^-23, which moves the sd by less than 1e-7.
    EXPECT_NEAR(columnStatistics(exact).standardDeviation.value_or(NOT_A_NUMBER), 0.01 * std::sqrt(50.0 * 51.0 / 12.0), 1e-7);
}

TEST(DataAnalysisStatistics, ExtremeMagnitudesNeitherOverflowNorUnderflow)
{
    const ColumnStatistics tiny = columnStatistics(std::vector<double>{1e-200, 2e-200, 3e-200});
    EXPECT_NEAR(tiny.standardDeviation.value_or(NOT_A_NUMBER) / 1e-200, 1.0, 1e-15);
    const ColumnStatistics huge = columnStatistics(std::vector<double>{1e200, -1e200, 1e200});
    EXPECT_NEAR(huge.standardDeviation.value_or(NOT_A_NUMBER) / 1e200, 2.0 / std::numbers::sqrt3, 1e-15);
    const ColumnStatistics sum = columnStatistics(std::vector<double>{1e308, 1e308});
    EXPECT_EQ(sum.mean, 1e308);
    EXPECT_EQ(sum.standardDeviation, 0.0);
    const ColumnStatistics deviation = columnStatistics(std::vector<double>{1.5e308, -1.5e308, -1.5e308});
    EXPECT_NEAR(deviation.mean.value_or(NOT_A_NUMBER) / -5e307, 1.0, 1e-15);
    EXPECT_NEAR(deviation.standardDeviation.value_or(NOT_A_NUMBER) / 1e308, std::numbers::sqrt3, 1e-15);
}

TEST(DataAnalysisStatistics, InfinitiesAndSignedZeros)
{
    const ColumnStatistics infinite = columnStatistics(std::vector<double>{1.0, INFINITE, 3.0});
    EXPECT_EQ(infinite.count, 3U);
    EXPECT_EQ(infinite.maximum, INFINITE);
    EXPECT_EQ(infinite.mean, INFINITE);
    EXPECT_FALSE(infinite.standardDeviation);
    const ColumnStatistics opposite = columnStatistics(std::vector<double>{-INFINITE, INFINITE});
    EXPECT_FALSE(opposite.mean);
    EXPECT_EQ(opposite.minimum, -INFINITE);
    // OSP reports NaN for these maxima and minima.
    EXPECT_EQ(columnStatistics(std::vector<double>{-INFINITE, -INFINITE}).maximum, -INFINITE);
    const double lowest = std::numeric_limits<double>::lowest();
    EXPECT_EQ(columnStatistics(std::vector<double>{lowest, lowest}).maximum, lowest);
    const ColumnStatistics zeros = columnStatistics(std::vector<double>{0.0, -0.0, 0.0});
    EXPECT_FALSE(std::signbit(zeros.maximum.value_or(NOT_A_NUMBER)));
    EXPECT_TRUE(std::signbit(zeros.minimum.value_or(NOT_A_NUMBER)));
    EXPECT_EQ(zeros.standardDeviation, 0.0);
}

TEST(DataAnalysisCorrelation, SquaredPearsonCorrelation)
{
    const std::vector<double> x{0, 1, 2, 3, 4};
    EXPECT_NEAR(squaredCorrelation(pointsOf(x, std::vector<double>{1, 3, 5, 7, 9})).value_or(NOT_A_NUMBER), 1.0, 1e-15);
    EXPECT_NEAR(squaredCorrelation(pointsOf(x, std::vector<double>{9, 7, 5, 3, 1})).value_or(NOT_A_NUMBER), 1.0, 1e-15);
    // r^2 = Sxy^2 / (Sxx Syy) for y = {1, 2, 1, 2, 4}: Sxy = 6, Sxx = 10, Syy = 6.
    EXPECT_NEAR(squaredCorrelation(pointsOf(x, std::vector<double>{1, 2, 1, 2, 4})).value_or(NOT_A_NUMBER), 36.0 / 60.0, 1e-15);
    // Gaps are skipped; fewer than three points or no variation is undefined.
    EXPECT_NEAR(squaredCorrelation(pointsOf(x, std::vector<double>{1, NOT_A_NUMBER, 5, 7, 9})).value_or(NOT_A_NUMBER), 1.0, 1e-15);
    EXPECT_FALSE(squaredCorrelation(pointsOf(x, std::vector<double>{1, NOT_A_NUMBER, NOT_A_NUMBER, NOT_A_NUMBER, 9})));
    EXPECT_FALSE(squaredCorrelation(pointsOf(x, std::vector<double>(5, 2.0))));
    EXPECT_FALSE(squaredCorrelation(pointsOf(std::vector<double>(4, 1.0), std::vector<double>{1, 2, 3, 4})));
    EXPECT_FALSE(squaredCorrelation(pointsOf(x, std::vector<double>{1e200, -1e200, 1e200, -1e200, 3e200})));
}

TEST(DataAnalysisPlotData, ExcludesRowsWithoutFiniteXAndKeepsGaps)
{
    const std::vector<double> x{NOT_A_NUMBER, 1, 2, INFINITE, 4, 5, 6};
    const std::vector<double> y{0, 1, NOT_A_NUMBER, 3, -INFINITE, 5};
    const PlotData data = plotData(x, y);
    // Row 6 has no y (the y column is shorter): a gap, like Data Tool's selected data.
    ASSERT_EQ(data.points.size(), 4U);
    EXPECT_EQ(data.rows, (std::vector<std::size_t>{1, 2, 5, 6}));
    EXPECT_EQ(data.excludedRows, 3U);
    EXPECT_EQ(data.points[0], (PlotPoint{.x = 1, .y = 1}));
    EXPECT_TRUE(std::isnan(data.points[1].y));
    EXPECT_EQ(data.points[2], (PlotPoint{.x = 5, .y = 5}));
    EXPECT_TRUE(std::isnan(data.points[3].y));
    const PlotData selected = plotData(x, y, std::vector<std::size_t>{5, 1, 9, 0});
    EXPECT_EQ(selected.rows, (std::vector<std::size_t>{5, 1}));
    EXPECT_EQ(selected.excludedRows, 2U);
    EXPECT_TRUE(plotData(std::vector<double>{}, y).points.empty());
}

TEST(DataAnalysisMeasures, CoordinatesAndSlopes)
{
    const std::vector<double> x = uniformTimes(21, 0.0, 0.1);
    std::vector<double> line(x.size());
    for (std::size_t i = 0; i < x.size(); ++i)
    {
        line[i] = 3.0 * x[i] - 2.0;
    }
    const std::vector<PlotPoint> points = pointsOf(x, line);
    EXPECT_EQ(pointCoordinates(points, 4), points[4]);
    EXPECT_FALSE(pointCoordinates(points, 21));
    for (std::size_t i = 1; i + 1 < points.size(); ++i)
    {
        EXPECT_NEAR(pointSlope(points, i).value_or(NOT_A_NUMBER), 3.0, 1e-13) << i;
    }
    EXPECT_FALSE(pointSlope(points, 0));
    EXPECT_FALSE(pointSlope(points, 20));
    EXPECT_FALSE(pointSlope(points, 21));
    // A gap or a missing neighbour leaves the slope undefined; equal neighbour x is vertical.
    const std::vector<PlotPoint> gaps{{.x = 0, .y = 0}, {.x = 1, .y = NOT_A_NUMBER}, {.x = 2, .y = 4}, {.x = 3, .y = 9}, {.x = 4, .y = 16}};
    EXPECT_FALSE(pointCoordinates(gaps, 1));
    EXPECT_FALSE(pointSlope(gaps, 1));
    EXPECT_FALSE(pointSlope(gaps, 2));
    EXPECT_EQ(pointSlope(gaps, 3), 6.0); // the chord through x = 2 and 4 of y = x^2
    const std::vector<PlotPoint> vertical{{.x = 0, .y = 0}, {.x = 1, .y = 1}, {.x = 0, .y = 2}};
    EXPECT_EQ(pointSlope(vertical, 1), INFINITE);
    const std::vector<PlotPoint> flat{{.x = 0, .y = 0}, {.x = 1, .y = 1}, {.x = 0, .y = 0}};
    EXPECT_FALSE(pointSlope(flat, 1));
    // f(x) = x^3: the central difference is 3 x^2 + dx^2.
    const ScalarFunction cube = [](double value)
    {
        return value * value * value;
    };
    EXPECT_NEAR(functionSlope(cube, 2.0, 0.01).value_or(NOT_A_NUMBER), 12.0001, 1e-10);
    EXPECT_FALSE(functionSlope([](double) { return NOT_A_NUMBER; }, 0.0, 0.1));
}

TEST(DataAnalysisMeasures, NearestPointWithinTheVisibleRange)
{
    const std::vector<PlotPoint> points{
        {.x = 0, .y = 0}, {.x = 1, .y = 1}, {.x = 2, .y = NOT_A_NUMBER}, {.x = 3, .y = 3}, {.x = 4, .y = 4}};
    EXPECT_EQ(nearestPointIndex(points, 1.4, -1, 5), 1U);
    EXPECT_EQ(nearestPointIndex(points, 2.1, -1, 5), 3U); // skips the gap
    EXPECT_EQ(nearestPointIndex(points, 2.0, -1, 5), 1U); // ties go to the first point
    EXPECT_EQ(nearestPointIndex(points, -9, -1, 5), 0U);
    EXPECT_EQ(nearestPointIndex(points, 9, -1, 5), 4U);
    // OSP's adjustments for a nearest point outside the view (they assume ascending x): left of
    // the view takes the next index; right of it, or past the end, takes the last index.
    EXPECT_EQ(nearestPointIndex(points, 0.2, 0.4, 5), 1U);
    const std::vector<PlotPoint> ramp{{.x = 0, .y = 0}, {.x = 1, .y = 1}, {.x = 2, .y = 2}, {.x = 3, .y = 3}};
    EXPECT_EQ(nearestPointIndex(ramp, 1.9, -1, 1.6), 1U); // OSP jumps to the last index, x = 3
    // The nearest visible point wins over a nearer off-screen one (#267), in either x order.
    std::vector<PlotPoint> ascending;
    std::vector<PlotPoint> descending;
    for (int i = 0; i <= 5; ++i)
    {
        ascending.push_back({.x = static_cast<double>(i), .y = 1.0});
        descending.push_back({.x = static_cast<double>(5 - i), .y = 1.0});
    }
    EXPECT_EQ(nearestPointIndex(ascending, 2.6, 0, 2.6), 2U);  // OSP: index 5
    EXPECT_EQ(nearestPointIndex(descending, 0.4, 0.4, 5), 4U); // x = 1; OSP: x = 0
    EXPECT_EQ(nearestPointIndex(points, 6, 5, 10), 4U);
    EXPECT_FALSE(nearestPointIndex({}, 0, 0, 1));
    EXPECT_FALSE(nearestPointIndex(std::vector<PlotPoint>{{.x = 0, .y = NOT_A_NUMBER}}, 0, 0, 1));
    EXPECT_FALSE(nearestPointIndex(points, NOT_A_NUMBER, 0, 1));
}

TEST(DataAnalysisArea, TrapezoidalRuleMatchesTheAnalyticSum)
{
    // For y = x^2 with spacing h the trapezoidal rule overestimates by (b - a) h^2 / 6.
    const std::vector<double> x = uniformTimes(21, 0.0, 0.1);
    std::vector<double> parabola(x.size());
    for (std::size_t i = 0; i < x.size(); ++i)
    {
        parabola[i] = x[i] * x[i];
    }
    const std::vector<PlotPoint> points = pointsOf(x, parabola);
    const AreaMeasurement full = dataArea(points, 0.0, 2.0);
    ASSERT_TRUE(full.area);
    EXPECT_NEAR(full.area.value_or(NOT_A_NUMBER), 8.0 / 3.0 + 2.0 * 0.01 / 6.0, 1e-13);
    EXPECT_EQ(full.pointCount, 21U);
    EXPECT_EQ(full.firstX, 0.0);
    EXPECT_EQ(full.lastX, 2.0);
    EXPECT_EQ(dataArea(points, 2.0, 0.0).area, full.area);
    // Limits between points use the points inside; a limit on a point includes it.
    const AreaMeasurement inner = dataArea(points, 0.35, 1.25);
    EXPECT_EQ(inner.pointCount, 9U);
    EXPECT_NEAR(inner.area.value_or(NOT_A_NUMBER), (1.2 * 1.2 * 1.2 - 0.4 * 0.4 * 0.4) / 3.0 + 0.8 * 0.01 / 6.0, 1e-13);
    EXPECT_EQ(dataArea(points, 0.3, 1.2).pointCount, 10U);
    // Fewer than two points: no area (OSP shows 0).
    const AreaMeasurement none = dataArea(points, 0.31, 0.39);
    EXPECT_EQ(none.pointCount, 0U);
    EXPECT_FALSE(none.area || none.firstX || none.lastX);
    // Between two distinct limits one point has no area; equal limits are a zero-width integral.
    const AreaMeasurement single = dataArea(std::vector<PlotPoint>{{.x = 1, .y = 2}}, 0.0, 3.0);
    EXPECT_EQ(single.pointCount, 1U);
    EXPECT_FALSE(single.area);
    EXPECT_EQ(single.firstX, 1.0);
    const AreaMeasurement one = dataArea(points, 0.5, 0.5);
    EXPECT_EQ(one.pointCount, 1U);
    EXPECT_EQ(one.area, 0.0);
    EXPECT_EQ(one.firstX, 0.5);
    const AreaMeasurement between = dataArea(points, 0.55, 0.55);
    EXPECT_EQ(between.pointCount, 0U);
    EXPECT_EQ(between.area, 0.0);
    EXPECT_FALSE(between.firstX);
    EXPECT_FALSE(dataArea(points, NOT_A_NUMBER, 1.0).area);
}

TEST(DataAnalysisArea, GapsAreBridgedAndPathsAreSigned)
{
    // A straight line is integrated exactly, so bridging its gaps changes nothing.
    const std::vector<PlotPoint> line{{.x = 0, .y = 1},
                                      {.x = 0.5, .y = 2},
                                      {.x = 1, .y = NOT_A_NUMBER},
                                      {.x = 1.5, .y = NOT_A_NUMBER},
                                      {.x = 2, .y = 5},
                                      {.x = 2.5, .y = 6}};
    EXPECT_DOUBLE_EQ(dataArea(line, 0.0, 2.5).area.value_or(NOT_A_NUMBER), 2.5 * (1.0 + 6.0) / 2.0);
    // The upper unit semicircle traced from x = 1 to x = -1: the signed integral of y dx is -pi/2.
    std::vector<PlotPoint> path;
    path.reserve(601);
    for (int i = 0; i <= 600; ++i)
    {
        const double t = PI * i / 600.0;
        path.push_back({.x = std::cos(t), .y = std::sin(t)});
    }
    EXPECT_NEAR(dataArea(path, -1.0, 1.0).area.value_or(NOT_A_NUMBER), -PI / 2.0, 1e-4);
    // Unequal spacing: sin x on [0, 3] within the trapezoid error bound (b - a) h^2 / 12.
    std::vector<PlotPoint> sine;
    sine.reserve(41);
    double position = 0.0;
    for (int i = 0; i < 40; ++i)
    {
        sine.push_back({.x = position, .y = std::sin(position)});
        position += i % 2 == 0 ? 0.05 : 0.1;
    }
    sine.push_back({.x = 3.0, .y = std::sin(3.0)});
    const AreaMeasurement uneven = dataArea(sine, 0.0, 3.0);
    EXPECT_NEAR(uneven.area.value_or(NOT_A_NUMBER), 1.0 - std::cos(3.0), 3.0 * 0.1 * 0.1 / 12.0);
}

TEST(DataAnalysisArea, FunctionAreaIncludesTheLastInterval)
{
    // f(x) = 1 + 2x - x^2/2 + x^3/4 integrates to 2 + 4 - 4/3 + 1 on [0, 2]; |f''| <= 2.
    const ScalarFunction cubic = [](double x)
    {
        return ((0.25 * x - 0.5) * x + 2.0) * x + 1.0;
    };
    const AreaMeasurement area = functionArea(cubic, 2.0, 0.0, 400);
    EXPECT_EQ(area.pointCount, 401U);
    EXPECT_EQ(area.firstX, 0.0);
    EXPECT_EQ(area.lastX, 2.0);
    EXPECT_NEAR(area.area.value_or(NOT_A_NUMBER), 17.0 / 3.0, 2.0 * 2.0 * (0.005 * 0.005) / 12.0);
    // A constant is exact; equal limits give zero; NaN samples are bridged like data gaps.
    EXPECT_NEAR(functionArea([](double) { return 4.0; }, -3.0, 5.0, 320).area.value_or(NOT_A_NUMBER), 32.0, 1e-12);
    EXPECT_EQ(functionArea(cubic, 1.0, 1.0, 10).area, 0.0);
    EXPECT_EQ(functionArea([](double) { return NOT_A_NUMBER; }, 1.0, 1.0, 0).area, 0.0); // no sampling needed
    const ScalarFunction holed = [](double x)
    {
        return x > 0.25 && x < 0.55 ? NOT_A_NUMBER : 2.0 * x;
    };
    const AreaMeasurement bridged = functionArea(holed, 0.0, 1.0, 10);
    EXPECT_EQ(bridged.pointCount, 8U);
    EXPECT_NEAR(bridged.area.value_or(NOT_A_NUMBER), 1.0, 1e-14);
    const ScalarFunction positive = [](double x)
    {
        return x < 0.0 ? NOT_A_NUMBER : x;
    };
    const AreaMeasurement half = functionArea(positive, -1.0, 1.0, 4);
    EXPECT_EQ(half.pointCount, 3U);
    EXPECT_EQ(half.firstX, 0.0);
    EXPECT_EQ(half.area, 0.5);
    EXPECT_FALSE(functionArea(cubic, 0.0, 1.0, 0).area);
    EXPECT_FALSE(functionArea(cubic, 0.0, INFINITE, 10).area);
    EXPECT_FALSE(functionArea(positive, -2.0, -1.0, 10).area);
}

/// Evaluates sum_k a_k cos(w_k x) + b_k sin(w_k x), the series the spectrum describes.
double reconstruct(const FourierSpectrum& spectrum, double x)
{
    double sum = 0.0;
    for (std::size_t k = 0; k < spectrum.cosine.size(); ++k)
    {
        sum +=
            spectrum.cosine[k] * std::cos(spectrum.angularFrequency[k] * x) + spectrum.sine[k] * std::sin(spectrum.angularFrequency[k] * x);
    }
    return sum;
}

TEST(DataAnalysisFourier, KnownAmplitudesFrequenciesAndPhases)
{
    // y = 0.5 + 3 sin(2 pi 2.5 t) + 2 cos(2 pi 1.25 t) on 64 samples 0.05 apart, from t = 0.
    const std::vector<double> t = uniformTimes(64, 0.0, 0.05);
    std::vector<double> y(t.size());
    for (std::size_t i = 0; i < t.size(); ++i)
    {
        y[i] = 0.5 + 3.0 * std::sin(2.0 * PI * 2.5 * t[i]) + 2.0 * std::cos(2.0 * PI * 1.25 * t[i]);
    }
    const FourierSpectrum spectrum = fourierSpectrum(pointsOf(t, y));
    ASSERT_EQ(spectrum.status, FourierStatus::Ok);
    ASSERT_EQ(spectrum.frequency.size(), 33U);
    EXPECT_EQ(spectrum.pointCount, 64U);
    EXPECT_FALSE(spectrum.droppedLastPoint);
    EXPECT_NEAR(spectrum.spacing, 0.05, 1e-16);
    EXPECT_GT(spectrum.maximumSpacingError, 0.0); // t = 0.05 i rounds
    EXPECT_LT(spectrum.maximumSpacingError, UNIFORM_SPACING_TOLERANCE * 1e-6);
    // Bins are 1 / (64 * 0.05) = 0.3125 Hz apart up to the 10 Hz Nyquist frequency.
    EXPECT_NEAR(spectrum.frequency[4], 1.25, 1e-14);
    EXPECT_NEAR(spectrum.frequency[8], 2.5, 1e-14);
    EXPECT_NEAR(spectrum.frequency[32], 10.0, 1e-13);
    EXPECT_NEAR(spectrum.angularFrequency[8], 2.0 * PI * 2.5, 1e-13);
    for (std::size_t k = 0; k <= 32; ++k)
    {
        const double cosine = k == 0 ? 0.5 : k == 4 ? 2.0 : 0.0;
        const double sine = k == 8 ? 3.0 : 0.0;
        EXPECT_NEAR(spectrum.cosine[k], cosine, 1e-13) << k;
        EXPECT_NEAR(spectrum.sine[k], sine, 1e-13) << k;
        EXPECT_NEAR(spectrum.power[k], cosine * cosine + sine * sine, 1e-12) << k;
    }
    EXPECT_EQ(spectrum.sine[0], 0.0);
}

TEST(DataAnalysisFourier, CoefficientsReferToTheOriginAndIncludeNyquist)
{
    // Twelve samples from t = 0.3, 0.1 apart (bins 1/1.2 Hz apart): a constant, 1.5 cos(2 pi (f t - 0.2))
    // at f = 5/3 Hz (bin 2) and the Nyquist term 0.75 cos(pi i) = 0.75 cos(2 pi 5 t - 3 pi).
    const std::vector<double> t = uniformTimes(12, 0.3, 0.1);
    std::vector<double> y(t.size());
    for (std::size_t i = 0; i < t.size(); ++i)
    {
        y[i] = -1.0 + 1.5 * std::cos(2.0 * PI * (5.0 / 3.0 * t[i] - 0.2)) + 0.75 * std::cos(PI * static_cast<double>(i));
    }
    const FourierSpectrum spectrum = fourierSpectrum(pointsOf(t, y));
    ASSERT_EQ(spectrum.status, FourierStatus::Ok);
    ASSERT_EQ(spectrum.cosine.size(), 7U);
    EXPECT_EQ(spectrum.origin, 0.3);
    EXPECT_NEAR(spectrum.cosine[0], -1.0, 1e-14);
    EXPECT_NEAR(spectrum.frequency[2], 5.0 / 3.0, 1e-14);
    // Relative to t = 0: 1.5 cos(w t - 0.4 pi) = 1.5 cos(0.4 pi) cos(w t) + 1.5 sin(0.4 pi) sin(w t).
    EXPECT_NEAR(spectrum.cosine[2], 1.5 * std::cos(0.4 * PI), 1e-13);
    EXPECT_NEAR(spectrum.sine[2], 1.5 * std::sin(0.4 * PI), 1e-13);
    EXPECT_NEAR(spectrum.frequency[6], 5.0, 1e-13);
    EXPECT_NEAR(spectrum.cosine[6], 0.75 * std::cos(3.0 * PI), 1e-13);
    EXPECT_NEAR(spectrum.sine[6], 0.0, 1e-13);
    EXPECT_NEAR(spectrum.power[6], 0.75 * 0.75, 1e-13);
    for (const std::size_t k : {1U, 3U, 4U, 5U})
    {
        EXPECT_NEAR(spectrum.power[k], 0.0, 1e-26) << k;
    }
    // The series reproduces every sample, which checks the constant and Nyquist weights.
    for (std::size_t i = 0; i < t.size(); ++i)
    {
        EXPECT_NEAR(reconstruct(spectrum, t[i]), y[i], 1e-12) << i;
    }
}

TEST(DataAnalysisFourier, OddCountsDropTheLastPointAndSpacingIsReported)
{
    const std::vector<double> t = uniformTimes(61, 0.0, 0.1);
    std::vector<double> y(t.size());
    for (std::size_t i = 0; i < t.size(); ++i)
    {
        y[i] = 2.0 * std::sin(2.0 * PI * t[i]);
    }
    const FourierSpectrum odd = fourierSpectrum(pointsOf(t, y));
    EXPECT_TRUE(odd.droppedLastPoint);
    EXPECT_EQ(odd.pointCount, 60U);
    EXPECT_NEAR(odd.sine[6], 2.0, 1e-13); // 1 Hz is bin 6 of 1/6 Hz
    // Unequal spacing still uses (x[n-1] - x[0] + x[1] - x[0]) / n and reports the deviation.
    const std::vector<PlotPoint> uneven{{.x = 0, .y = 1}, {.x = 1, .y = 2}, {.x = 3, .y = 1}, {.x = 4, .y = 0}};
    const FourierSpectrum skewed = fourierSpectrum(uneven);
    EXPECT_EQ(skewed.status, FourierStatus::Ok);
    EXPECT_DOUBLE_EQ(skewed.spacing, 1.25);
    EXPECT_DOUBLE_EQ(skewed.maximumSpacingError, 0.6);
    // Descending x gives negative frequencies, as in OSP.
    const FourierSpectrum descending =
        fourierSpectrum(std::vector<PlotPoint>{{.x = 1, .y = 0}, {.x = 0.5, .y = 1}, {.x = 0, .y = 0}, {.x = -0.5, .y = -1}});
    EXPECT_DOUBLE_EQ(descending.frequency[1], -0.5);
    EXPECT_NEAR(descending.power[1], 1.0, 1e-15);
}

TEST(DataAnalysisFourier, DegenerateInputsHaveAStatus)
{
    EXPECT_EQ(fourierSpectrum({}).status, FourierStatus::TooFewPoints);
    const FourierSpectrum one = fourierSpectrum(std::vector<PlotPoint>{{.x = 0, .y = 1}});
    EXPECT_EQ(one.status, FourierStatus::TooFewPoints);
    EXPECT_TRUE(one.frequency.empty());
    const FourierSpectrum gap =
        fourierSpectrum(std::vector<PlotPoint>{{.x = 0, .y = 1}, {.x = 1, .y = NOT_A_NUMBER}, {.x = 2, .y = 1}, {.x = 3, .y = 0}});
    EXPECT_EQ(gap.status, FourierStatus::MissingValues);
    ASSERT_EQ(gap.frequency.size(), 3U);
    EXPECT_EQ(gap.frequency[1], 0.25);
    EXPECT_TRUE(std::isnan(gap.cosine[1]) && std::isnan(gap.power[0]) && std::isnan(gap.sine[0]));
    const FourierSpectrum flat =
        fourierSpectrum(std::vector<PlotPoint>{{.x = 1, .y = 0}, {.x = 1, .y = 1}, {.x = 1, .y = 0}, {.x = 1, .y = -1}});
    EXPECT_EQ(flat.status, FourierStatus::InvalidSpacing);
    EXPECT_TRUE(flat.cosine.empty());
    // Two points: the mean and the Nyquist term (OSP folds the latter into the zero-frequency sine).
    const FourierSpectrum two = fourierSpectrum(std::vector<PlotPoint>{{.x = 0, .y = 1}, {.x = 0.5, .y = 3}});
    ASSERT_EQ(two.power.size(), 2U);
    EXPECT_EQ(two.cosine[0], 2.0);
    EXPECT_EQ(two.sine[0], 0.0);
    EXPECT_EQ(two.frequency[1], 1.0);
    EXPECT_NEAR(two.cosine[1], -1.0, 1e-15);
    EXPECT_NEAR(two.power[1], 1.0, 1e-15);
}

TEST(DataAnalysisUnits, DerivedUnitsOfTheMeasurements)
{
    EXPECT_EQ(SLOPE_UNIT, (UnitPowers{1, -1}));
    EXPECT_EQ(AREA_UNIT, (UnitPowers{1, 1}));
    EXPECT_EQ(FREQUENCY_UNIT, (UnitPowers{0, -1}));
    EXPECT_EQ(FOURIER_COEFFICIENT_UNIT, (UnitPowers{1, 0}));
    EXPECT_EQ(FOURIER_POWER_UNIT, (UnitPowers{2, 0}));
}

} // namespace
