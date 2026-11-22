#include "science/analysis/data_analysis.hpp"
#include "science/analysis/fft.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Compares the kernel with tests/science/analysis-reference.json, which
// tools/reference/AnalysisOracle.java records from the pinned Tracker 6.3.5 classes. Pure IEEE
// results must be identical; FFT-based values allow the Math.sin/Math.cos tolerance documented in
// docs/data-analysis-kernels.md. Where OSP is wrong, the test asserts both the corrected value and
// that the Java value deviates, so the documented differences cannot silently disappear.

namespace
{

using namespace motrack::science;

constexpr double NOT_A_NUMBER = std::numeric_limits<double>::quiet_NaN();
constexpr double UNIT_ROUNDOFF = 0x1.0p-53;

const QJsonObject& document()
{
    static const QJsonObject loaded = []
    {
        QFile file(QStringLiteral(MOTRACK_ANALYSIS_REFERENCE));
        if (!file.open(QIODevice::ReadOnly))
        {
            return QJsonObject{};
        }
        return QJsonDocument::fromJson(file.readAll()).object();
    }();
    return loaded;
}

std::string idOf(const QJsonObject& object)
{
    return object[QStringLiteral("id")].toString().toStdString();
}

/// The entry with this id in a fixture list ("columns", "series" or "cases").
QJsonObject entry(const char* list, const std::string& id)
{
    for (const QJsonValue value : document()[QLatin1String(list)].toArray())
    {
        if (idOf(value.toObject()) == id)
        {
            return value.toObject();
        }
    }
    return {};
}

std::vector<QJsonObject> casesOf(const char* kind)
{
    std::vector<QJsonObject> cases;
    for (const QJsonValue value : document()[QStringLiteral("cases")].toArray())
    {
        if (value[QStringLiteral("kind")].toString() == QLatin1String(kind))
        {
            cases.push_back(value.toObject());
        }
    }
    return cases;
}
double number(const QJsonValue& value)
{
    if (value.isString())
    {
        const QString text = value.toString();
        if (text == QStringLiteral("NaN"))
        {
            return NOT_A_NUMBER;
        }
        return text.startsWith(QLatin1Char('-')) ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
    }
    return value.toDouble();
}

std::vector<double> numbers(const QJsonValue& value)
{
    std::vector<double> result;
    for (const QJsonValue item : value.toArray())
    {
        result.push_back(number(item));
    }
    return result;
}

std::vector<std::size_t> rowsOf(const QJsonValue& value)
{
    std::vector<std::size_t> rows;
    for (const QJsonValue item : value.toArray())
    {
        rows.push_back(static_cast<std::size_t>(item.toInt()));
    }
    return rows;
}

std::vector<PlotPoint> pointsOf(const QJsonObject& object)
{
    const std::vector<double> x = numbers(object[QStringLiteral("x")]);
    const std::vector<double> y = numbers(object[QStringLiteral("y")]);
    std::vector<PlotPoint> points;
    points.reserve(x.size());
    for (std::size_t i = 0; i < x.size(); ++i)
    {
        points.push_back({.x = x[i], .y = y[i]});
    }
    return points;
}

/// Identical doubles, with NaN equal to NaN (signed zeros compare equal, as in the fixture).
bool same(double first, double second)
{
    return (std::isnan(first) && std::isnan(second)) || first == second;
}

bool samePoints(std::span<const PlotPoint> first, std::span<const PlotPoint> second)
{
    return std::ranges::equal(first, second, [](const PlotPoint& a, const PlotPoint& b) { return same(a.x, b.x) && same(a.y, b.y); });
}

double orNaN(std::optional<double> value)
{
    return value.value_or(NOT_A_NUMBER);
}

/// OSP's working dataset: the x and y columns of a Data Tool tab.
std::vector<PlotPoint> javaWorking(const std::string& series)
{
    return pointsOf(entry("cases", "plot/" + series)[QStringLiteral("working")].toObject());
}

/// OSP's selected dataset (getSelectedData), recorded only where it differs from the working data.
std::vector<PlotPoint> javaSelected(const std::string& series)
{
    const QJsonObject plot = entry("cases", "plot/" + series);
    const QJsonValue selected = plot[QStringLiteral("selected")];
    return pointsOf((selected.isNull() ? plot[QStringLiteral("working")] : selected).toObject());
}

// --- A model of OSP's Dataset.append(double[], double[], int), the defect documented in plotData.
// removeBadData compacts rows without a finite x or with an infinite y, but the point count stays
// the same, so the old tail remains after the compacted points.
std::vector<PlotPoint> ospAppend(std::span<const double> x, std::span<const double> y, std::size_t count)
{
    std::vector<PlotPoint> points;
    points.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        points.push_back({.x = x[i], .y = y[i]});
    }
    std::size_t offset = 0;
    for (std::size_t i = 0; i < count; ++i)
    {
        if (std::isnan(x[i]) || std::isinf(x[i]) || std::isinf(y[i]))
        {
            ++offset;
        }
        else if (offset > 0)
        {
            points[i - offset] = {.x = x[i], .y = y[i]};
        }
    }
    return points;
}

/// DataColumn.setPoints: the column's values are the y of (row number, value) points.
std::vector<double> ospColumn(std::span<const double> values)
{
    std::vector<double> rows(values.size());
    for (std::size_t i = 0; i < rows.size(); ++i)
    {
        rows[i] = static_cast<double>(i);
    }
    std::vector<double> stored;
    for (const PlotPoint& point : ospAppend(rows, values, values.size()))
    {
        stored.push_back(point.y);
    }
    return stored;
}

TEST(AnalysisReference, FixtureCoversEveryKind)
{
    ASSERT_FALSE(document()[QStringLiteral("cases")].toArray().isEmpty()) << "cannot read " << MOTRACK_ANALYSIS_REFERENCE;
    EXPECT_EQ(document()[QStringLiteral("schema_version")].toInt(), 1);
    EXPECT_EQ(document()[QStringLiteral("reference_version")].toString(), QStringLiteral("6.3.5"));
    for (const char* kind : {"statistics",
                             "plot",
                             "correlation",
                             "area",
                             "measure",
                             "nearest",
                             "function-area",
                             "function-slope",
                             "fourier",
                             "fft-real",
                             "fft-complex",
                             "column"})
    {
        EXPECT_FALSE(casesOf(kind).empty()) << kind;
    }
}

// --- Statistics ------------------------------------------------------------------------------

/// Corrected values for the columns where OSP's statistics are wrong (the fixture's "defect").
struct Corrected
{
    std::optional<double> maximum;
    std::optional<double> minimum;
    std::optional<double> mean;
    std::optional<double> standardDeviation;
};

double shiftedStandardDeviation(std::span<const double> values, double shift)
{
    // Textbook two-pass sd of the exactly shifted values, independent of the kernel.
    double mean = 0.0;
    for (const double value : values)
    {
        mean += value - shift;
    }
    mean /= static_cast<double>(values.size());
    double squares = 0.0;
    for (const double value : values)
    {
        squares += (value - shift - mean) * (value - shift - mean);
    }
    return std::sqrt(squares / static_cast<double>(values.size() - 1));
}

std::map<std::string, Corrected> correctedStatistics()
{
    const double largest = std::numeric_limits<double>::max();
    const double infinity = std::numeric_limits<double>::infinity();
    return {
        {"constant-tenth", {.standardDeviation = 0.0}},
        {"constant-third", {.standardDeviation = 0.0}},
        {"offset-1e8", {.standardDeviation = 1.0}},
        {"offset-1e9",
         {.standardDeviation = shiftedStandardDeviation(numbers(entry("columns", "offset-1e9")[QStringLiteral("values")]), 1e9)}},
        {"huge", {.standardDeviation = 2e200 / std::numbers::sqrt3}},
        {"tiny", {.standardDeviation = 1e-200}},
        {"overflowing-sum", {.mean = 1e308, .standardDeviation = 0.0}},
        {"overflowing-deviation", {.mean = -5e307, .standardDeviation = std::numbers::sqrt3 * 1e308}},
        {"negative-infinity", {.maximum = -infinity}},
        {"most-negative", {.maximum = -largest, .mean = -largest, .standardDeviation = 0.0}},
        {"most-positive", {.minimum = largest, .mean = largest, .standardDeviation = 0.0}},
    };
}

bool matches(double value, double expected)
{
    if (expected == 0.0 || std::isinf(expected))
    {
        return value == expected;
    }
    return std::abs(value - expected) <= 1e-14 * std::abs(expected);
}

TEST(AnalysisReference, StatisticsTable)
{
    const std::map<std::string, Corrected> corrected = correctedStatistics();
    std::size_t compared = 0;
    for (const QJsonObject& item : casesOf("statistics"))
    {
        SCOPED_TRACE(idOf(item));
        const std::string column = item[QStringLiteral("column")].toString().toStdString();
        const std::vector<double> values = numbers(entry("columns", column)[QStringLiteral("values")]);
        std::vector<double> selected = values;
        ColumnStatistics statistics;
        if (item[QStringLiteral("rows")].isNull())
        {
            statistics = columnStatistics(values);
        }
        else
        {
            const std::vector<std::size_t> rows = rowsOf(item[QStringLiteral("rows")]);
            statistics = columnStatistics(values, rows);
            selected.clear();
            for (const std::size_t row : rows)
            {
                selected.push_back(row < values.size() ? values[row] : NOT_A_NUMBER);
            }
        }
        EXPECT_EQ(statistics.count, static_cast<std::size_t>(item[QStringLiteral("count")].toInt()));
        const bool defect = !item[QStringLiteral("defect")].isNull();
        const auto fix = corrected.find(column);
        ASSERT_EQ(defect, fix != corrected.end());
        const Corrected expected = defect ? fix->second : Corrected{};
        const double javaSd = number(item[QStringLiteral("sd")]);
        const double javaSe = number(item[QStringLiteral("se")]);
        const auto check = [&](const char* name, std::optional<double> ours, std::optional<double> fixed)
        {
            const double java = number(item[QLatin1String(name)]);
            if (fixed)
            {
                EXPECT_TRUE(ours.has_value() && matches(ours.value(), fixed.value()))
                    << name << ": " << orNaN(ours) << " instead of " << fixed.value();
                EXPECT_FALSE(matches(java, fixed.value())) << name << ": OSP no longer deviates (" << java << ")";
            }
            else
            {
                EXPECT_TRUE(same(orNaN(ours), java)) << name << ": " << orNaN(ours) << " vs Java " << java;
            }
        };
        check("max", statistics.maximum, expected.maximum);
        check("min", statistics.minimum, expected.minimum);
        check("mean", statistics.mean, expected.mean);
        if (expected.standardDeviation)
        {
            check("sd", statistics.standardDeviation, expected.standardDeviation);
            const double expectedSe = expected.standardDeviation.value() / std::sqrt(static_cast<double>(statistics.count));
            check("se", statistics.standardError, expectedSe);
            continue;
        }
        if (std::isnan(javaSd))
        {
            EXPECT_FALSE(statistics.standardDeviation || statistics.standardError);
            continue;
        }
        ASSERT_TRUE(statistics.standardDeviation && statistics.standardError);
        const double ours = statistics.standardDeviation.value();
        if (ours == 0.0)
        {
            EXPECT_EQ(javaSd, 0.0);
            continue;
        }
        // OSP's one-pass sqrt((S - n m^2) / (n - 1)) has a rounding error of about n u (S + n m^2) / (n - 1)
        // in the variance (Chan, Golub and LeVeque 1983), the corrected two-pass sum about n u var.
        double squares = 0.0;
        double n = 0.0;
        for (const double value : selected)
        {
            if (!std::isnan(value))
            {
                squares += value * value;
                n += 1.0;
            }
        }
        const double mean = statistics.mean.value_or(NOT_A_NUMBER);
        const double bound = 4.0 * (n + 4.0) * UNIT_ROUNDOFF * ((squares + n * mean * mean) / ((n - 1.0) * ours) + ours);
        EXPECT_NEAR(ours, javaSd, bound);
        EXPECT_NEAR(statistics.standardError.value(), javaSe, bound / std::sqrt(n) + 4.0 * UNIT_ROUNDOFF * javaSe);
        ++compared;
    }
    EXPECT_GE(compared, 10U);
}

// --- Plot data and correlation -----------------------------------------------------------------

TEST(AnalysisReference, PlotDataAndTheDatasetAppendDefect)
{
    for (const QJsonObject& item : casesOf("plot"))
    {
        SCOPED_TRACE(idOf(item));
        const QJsonObject& series = entry("series", item[QStringLiteral("series")].toString().toStdString());
        const std::vector<double> x = numbers(series[QStringLiteral("x")]);
        const std::vector<double> y = numbers(series[QStringLiteral("y")]);
        const std::vector<double> storedX = ospColumn(x);
        const std::vector<double> storedY = ospColumn(y);
        if (item[QStringLiteral("rows")].isNull())
        {
            // The model reproduces what DataColumn stores and what both datasets contain.
            const QJsonValue stored = item[QStringLiteral("stored_columns")];
            const bool aligned = stored.isNull();
            const auto sameValues = [](std::span<const double> a, std::span<const double> b)
            {
                return std::ranges::equal(a, b, same);
            };
            EXPECT_EQ(aligned, sameValues(storedX, x) && sameValues(storedY, y));
            if (!aligned)
            {
                EXPECT_TRUE(sameValues(storedX, numbers(stored[QStringLiteral("x")])));
                EXPECT_TRUE(sameValues(storedY, numbers(stored[QStringLiteral("y")])));
            }
            const std::vector<PlotPoint> working = pointsOf(item[QStringLiteral("working")].toObject());
            EXPECT_TRUE(samePoints(working, ospAppend(storedX, storedY, std::min(storedX.size(), storedY.size()))));
            std::vector<double> paddedY(storedX.size(), NOT_A_NUMBER);
            std::copy_n(storedY.begin(), std::min(storedY.size(), paddedY.size()), paddedY.begin());
            const std::vector<PlotPoint> selected = javaSelected(item[QStringLiteral("series")].toString().toStdString());
            EXPECT_TRUE(samePoints(selected, ospAppend(storedX, paddedY, storedX.size())));
            // MoTrack keeps the same points without the repeated tail. A column with an infinite value
            // is shifted up by DataColumn itself, so there the OSP rows are misaligned instead.
            const PlotData ours = plotData(x, y);
            if (aligned)
            {
                ASSERT_EQ(selected.size(), x.size());
                EXPECT_EQ(ours.points.size() + ours.excludedRows, x.size());
                EXPECT_TRUE(samePoints(ours.points, std::span<const PlotPoint>(selected).first(ours.points.size())));
            }
            else
            {
                EXPECT_FALSE(samePoints(ours.points, std::span<const PlotPoint>(selected).first(ours.points.size())));
            }
        }
        else
        {
            const std::vector<std::size_t> rows = rowsOf(item[QStringLiteral("rows")]);
            const std::vector<PlotPoint> selected = pointsOf(item[QStringLiteral("selected")].toObject());
            const PlotData ours = plotData(x, y, rows);
            ASSERT_EQ(selected.size(), rows.size());
            EXPECT_EQ(ours.points.size() + ours.excludedRows, rows.size());
            EXPECT_TRUE(samePoints(ours.points, std::span<const PlotPoint>(selected).first(ours.points.size())));
        }
    }
}

TEST(AnalysisReference, CorrelationStatus)
{
    for (const QJsonObject& item : casesOf("correlation"))
    {
        SCOPED_TRACE(idOf(item));
        const std::vector<PlotPoint> points = javaSelected(item[QStringLiteral("series")].toString().toStdString());
        EXPECT_TRUE(same(orNaN(squaredCorrelation(points)), number(item[QStringLiteral("correlation")])));
    }
}

// --- Measuring tools ---------------------------------------------------------------------------

TEST(AnalysisReference, AreaTool)
{
    for (const QJsonObject& item : casesOf("area"))
    {
        SCOPED_TRACE(idOf(item));
        const std::vector<PlotPoint> points = javaWorking(item[QStringLiteral("series")].toString().toStdString());
        const std::vector<double> limits = numbers(item[QStringLiteral("limits")]);
        const AreaMeasurement area = dataArea(points, limits[0], limits[1]);
        const auto count = static_cast<std::size_t>(item[QStringLiteral("points")].toInt());
        EXPECT_EQ(area.pointCount, count);
        const double javaArea = number(item[QStringLiteral("area")]);
        if (count >= 2)
        {
            ASSERT_TRUE(area.area);
            EXPECT_TRUE(same(area.area.value(), javaArea)) << area.area.value() << " vs Java " << javaArea;
        }
        else
        {
            EXPECT_FALSE(area.area); // OSP shows 0
            EXPECT_EQ(javaArea, 0.0);
        }
        const std::vector<double> trueLimits = numbers(item[QStringLiteral("true_limits")]);
        if (count == 0)
        {
            EXPECT_FALSE(area.firstX || area.lastX); // OSP's empty dataset reports +/-Double.MAX_VALUE
            EXPECT_EQ(trueLimits[0], std::numeric_limits<double>::max());
            EXPECT_EQ(trueLimits[1], -std::numeric_limits<double>::max());
        }
        else
        {
            EXPECT_EQ(area.firstX, trueLimits[0]);
            EXPECT_EQ(area.lastX, trueLimits[1]);
        }
    }
}

TEST(AnalysisReference, CoordinatesAndSlopeTools)
{
    for (const QJsonObject& item : casesOf("measure"))
    {
        SCOPED_TRACE(idOf(item));
        const std::vector<PlotPoint> points = javaWorking(item[QStringLiteral("series")].toString().toStdString());
        const auto index = static_cast<std::size_t>(item[QStringLiteral("index")].toInt());
        const std::vector<double> coordinates = numbers(item[QStringLiteral("coordinates")]);
        const std::optional<PlotPoint> ours = pointCoordinates(points, index);
        EXPECT_TRUE(same(ours ? ours->x : NOT_A_NUMBER, coordinates[0]));
        EXPECT_TRUE(same(ours ? ours->y : NOT_A_NUMBER, coordinates[1]));
        EXPECT_TRUE(same(orNaN(pointSlope(points, index)), number(item[QStringLiteral("slope")])));
    }
}

TEST(AnalysisReference, NearestMeasuredPoint)
{
    for (const QJsonObject& item : casesOf("nearest"))
    {
        SCOPED_TRACE(idOf(item));
        const std::vector<PlotPoint> points = javaWorking(item[QStringLiteral("series")].toString().toStdString());
        const std::vector<double> visible = numbers(item[QStringLiteral("visible")]);
        const std::optional<std::size_t> ours = nearestPointIndex(points, number(item[QStringLiteral("x")]), visible[0], visible[1]);
        const QJsonValue java = item[QStringLiteral("index")];
        if (java.isString())
        {
            EXPECT_EQ(java.toString(), QStringLiteral("ArrayIndexOutOfBoundsException"));
            EXPECT_FALSE(ours);
        }
        else
        {
            EXPECT_EQ(ours, static_cast<std::size_t>(java.toInt()));
        }
    }
}

ScalarFunction polynomial(std::vector<double> coefficients)
{
    // OSP's Polynomial.evaluate (Horner's rule in the same order).
    return [coefficients = std::move(coefficients)](double x)
    {
        std::size_t n = coefficients.size();
        double answer = coefficients[--n];
        while (n > 0)
        {
            answer = answer * x + coefficients[--n];
        }
        return answer;
    };
}

double integral(std::span<const double> coefficients, double a, double b)
{
    double sum = 0.0;
    for (std::size_t k = 0; k < coefficients.size(); ++k)
    {
        const auto power = static_cast<double>(k + 1);
        sum += coefficients[k] * (std::pow(b, power) - std::pow(a, power)) / power;
    }
    return sum;
}

TEST(AnalysisReference, AreaToolOnAFitOmitsTheLastIntervalInOsp)
{
    for (const QJsonObject& item : casesOf("function-area"))
    {
        SCOPED_TRACE(idOf(item));
        const std::vector<double> coefficients = numbers(item[QStringLiteral("coefficients")]);
        const ScalarFunction function = polynomial(coefficients);
        const std::vector<double> limits = numbers(item[QStringLiteral("limits")]);
        const auto samples = static_cast<std::size_t>(item[QStringLiteral("samples")].toInt());
        const double javaArea = number(item[QStringLiteral("area")]);
        if (samples == 0)
        {
            EXPECT_EQ(javaArea, 0.0);
            EXPECT_FALSE(functionArea(function, limits[0], limits[1], 0).area);
            continue;
        }
        const double lower = std::min(limits[0], limits[1]);
        const double upper = std::max(limits[0], limits[1]);
        const double exact = integral(coefficients, lower, upper);
        // Trapezoidal error bound (b - a) h^2 max|f''| / 12; f'' of a cubic is extreme at an end.
        const double c2 = coefficients.size() > 2 ? coefficients[2] : 0.0;
        const double c3 = coefficients.size() > 3 ? coefficients[3] : 0.0;
        const double curvature = std::max(std::abs(2 * c2 + 6 * c3 * lower), std::abs(2 * c2 + 6 * c3 * upper));
        const double h = (upper - lower) / static_cast<double>(samples);
        const double bound = (upper - lower) * h * h * curvature / 12.0 + 1e-12 * std::abs(exact);
        const AreaMeasurement ours = functionArea(function, limits[0], limits[1], samples);
        ASSERT_TRUE(ours.area);
        EXPECT_NEAR(ours.area.value(), exact, bound);
        EXPECT_GT(std::abs(javaArea - exact), 10.0 * bound) << "OSP no longer omits the last interval";
        // OSP's value is the trapezoidal sum from the widened lower limit over samples - 1 intervals.
        const double first = number(item[QStringLiteral("first_sample")]);
        const double last = number(item[QStringLiteral("last_sample")]);
        EXPECT_LT(first, lower);
        EXPECT_LT(last, upper);
        const AreaMeasurement osp = functionArea(function, first, last, samples - 1);
        EXPECT_NEAR(osp.area.value_or(NOT_A_NUMBER), javaArea, 1e-12 * std::max(1.0, std::abs(javaArea)));
    }
}

TEST(AnalysisReference, SlopeToolOnAFit)
{
    for (const QJsonObject& item : casesOf("function-slope"))
    {
        SCOPED_TRACE(idOf(item));
        const ScalarFunction function = polynomial(numbers(item[QStringLiteral("coefficients")]));
        const double x = number(item[QStringLiteral("x")]);
        EXPECT_TRUE(same(function(x), number(item[QStringLiteral("value")])));
        const std::optional<double> slope = functionSlope(function, x, number(item[QStringLiteral("dx")]));
        EXPECT_TRUE(same(orNaN(slope), number(item[QStringLiteral("slope")])));
    }
}

// --- Fourier spectrum and transforms ------------------------------------------------------------

double magnitudeSum(std::span<const double> values)
{
    double sum = 0.0;
    for (const double value : values)
    {
        if (!std::isnan(value))
        {
            sum += std::abs(value);
        }
    }
    return sum;
}

/// tools/corpus.py's ANALYSIS_FFT_ULPS rule: 16 units of 2^-52 per stage of the input scale.
double fftTolerance(std::span<const double> values, std::size_t n)
{
    return 16.0 * (1.0 + std::log2(static_cast<double>(n))) * 0x1.0p-52 * magnitudeSum(values);
}

/// OSP's FFTReal skips the middle pair of bins when n/2 is odd and n > 6.
bool ospMisunpacks(std::size_t n, std::size_t bin)
{
    const std::size_t half = n / 2;
    return n >= 10 && half % 2 == 1 && (bin == (half - 1) / 2 || bin == (half + 1) / 2);
}

TEST(AnalysisReference, FourierSpectrum)
{
    std::size_t misunpacked = 0;
    for (const QJsonObject& item : casesOf("fourier"))
    {
        SCOPED_TRACE(idOf(item));
        const std::vector<PlotPoint> points = javaSelected(item[QStringLiteral("series")].toString().toStdString());
        const FourierSpectrum ours = fourierSpectrum(points);
        const QJsonValue spectrumValue = item[QStringLiteral("spectrum")];
        if (spectrumValue.isNull())
        {
            EXPECT_EQ(ours.status, FourierStatus::TooFewPoints);
            continue;
        }
        const QJsonObject spectrum = spectrumValue.toObject();
        const auto n = static_cast<std::size_t>(item[QStringLiteral("transformed_points")].toInt());
        EXPECT_EQ(ours.pointCount, n);
        EXPECT_EQ(ours.droppedLastPoint, item[QStringLiteral("input_points")].toInt() % 2 == 1);
        const std::size_t half = n / 2;
        const auto rows = static_cast<std::size_t>(spectrum[QStringLiteral("rows")].toInt());
        if (ours.status == FourierStatus::InvalidSpacing)
        {
            // OSP lists infinite frequencies with NaN coefficients after the first row.
            EXPECT_TRUE(ours.frequency.empty() && ours.cosine.empty());
            const std::vector<double> frequency = numbers(spectrum[QStringLiteral("frequency")]);
            const std::vector<double> power = numbers(spectrum[QStringLiteral("power")]);
            ASSERT_GE(frequency.size(), 2U);
            EXPECT_TRUE(std::isinf(frequency[1]) && std::isnan(power[1]));
            continue;
        }
        ASSERT_EQ(rows, half);
        ASSERT_EQ(ours.frequency.size(), half + 1);
        const std::vector<double> frequency = numbers(spectrum[QStringLiteral("frequency")]);
        const std::vector<double> omega = numbers(spectrum[QStringLiteral("omega")]);
        const std::vector<double> cosine = numbers(spectrum[QStringLiteral("cosine")]);
        const std::vector<double> sine = numbers(spectrum[QStringLiteral("sine")]);
        const std::vector<double> power = numbers(spectrum[QStringLiteral("power")]);
        std::vector<double> y;
        double nyquist = 0.0;
        for (std::size_t j = 0; j < n; ++j)
        {
            y.push_back(points[j].y);
            nyquist += (j % 2 == 0 ? points[j].y : -points[j].y) / static_cast<double>(n);
        }
        const double scale = 2.0 * magnitudeSum(y) / static_cast<double>(n);
        const double tolerance = fftTolerance(y, n) * 2.0 / static_cast<double>(n);
        const double powerTolerance = (4.0 * scale + 2.0 * tolerance) * tolerance;
        const bool missing = ours.status == FourierStatus::MissingValues;
        EXPECT_EQ(missing, std::ranges::any_of(y, [](double value) { return std::isnan(value); }));
        const auto near = [&](double value, double java, double allowed)
        {
            return missing ? std::isnan(value) && std::isnan(java) : std::abs(value - java) <= allowed;
        };
        for (std::size_t k = 0; k < half; ++k)
        {
            SCOPED_TRACE(k);
            EXPECT_EQ(ours.frequency[k], frequency[k]);
            EXPECT_EQ(ours.angularFrequency[k], omega[k]);
            if (k == 0)
            {
                // OSP puts -X_{n/2} / n in the zero-frequency sine and its square in the power.
                EXPECT_TRUE(near(ours.cosine[0], cosine[0], tolerance));
                EXPECT_EQ(ours.sine[0], 0.0);
                EXPECT_TRUE(near(-nyquist, sine[0], tolerance));
                EXPECT_TRUE(near(ours.power[0] + nyquist * nyquist, power[0], powerTolerance));
                EXPECT_TRUE(near(ours.power[half], nyquist * nyquist, powerTolerance));
            }
            else if (ospMisunpacks(n, k))
            {
                EXPECT_GT(std::abs(ours.power[k] - power[k]), 1e6 * powerTolerance) << "OSP no longer misunpacks bin " << k;
                ++misunpacked;
            }
            else
            {
                EXPECT_TRUE(near(ours.cosine[k], cosine[k], tolerance)) << ours.cosine[k] << " vs " << cosine[k];
                EXPECT_TRUE(near(ours.sine[k], sine[k], tolerance)) << ours.sine[k] << " vs " << sine[k];
                EXPECT_TRUE(near(ours.power[k], power[k], powerTolerance)) << ours.power[k] << " vs " << power[k];
            }
        }
    }
    EXPECT_GE(misunpacked, 10U);
}

TEST(AnalysisReference, RealTransform)
{
    std::size_t misunpacked = 0;
    for (const QJsonObject& item : casesOf("fft-real"))
    {
        SCOPED_TRACE(idOf(item));
        const std::vector<double> input = numbers(item[QStringLiteral("input")]);
        const std::vector<double> java = numbers(item[QStringLiteral("output")]);
        std::vector<double> ours = input;
        ASSERT_TRUE(realFft(ours));
        const std::size_t n = input.size();
        const double tolerance = fftTolerance(input, n);
        for (std::size_t i = 0; i < n; ++i)
        {
            // data[2k], data[2k + 1] hold bin k for 0 < k < n/2.
            if (i >= 2 && ospMisunpacks(n, i / 2))
            {
                EXPECT_GT(std::abs(ours[i] - java[i]), 1e6 * tolerance) << i;
                ++misunpacked;
            }
            else
            {
                EXPECT_NEAR(ours[i], java[i], tolerance) << i;
            }
        }
    }
    EXPECT_GE(misunpacked, 20U);
}

TEST(AnalysisReference, ComplexTransform)
{
    for (const QJsonObject& item : casesOf("fft-complex"))
    {
        SCOPED_TRACE(idOf(item));
        const std::vector<double> input = numbers(item[QStringLiteral("input")]);
        const std::vector<double> java = numbers(item[QStringLiteral("output")]);
        std::vector<double> ours = input;
        ASSERT_TRUE(complexFft(ours));
        const double tolerance = fftTolerance(input, input.size() / 2);
        for (std::size_t i = 0; i < input.size(); ++i)
        {
            EXPECT_NEAR(ours[i], java[i], tolerance) << i;
        }
    }
}

TEST(AnalysisReference, DataColumnsShiftValuesAfterAnInfinity)
{
    // Not a kernel computation: evidence for the column model that DataColumn.setPoints drops an
    // infinite value, moves the following values up a row and repeats the last one.
    for (const QJsonObject& item : casesOf("column"))
    {
        SCOPED_TRACE(idOf(item));
        const std::vector<double> input = numbers(item[QStringLiteral("input")]);
        const std::vector<double> stored = ospColumn(input);
        EXPECT_TRUE(std::ranges::equal(stored, numbers(item[QStringLiteral("values")]), same));
        EXPECT_EQ(stored.size(), input.size());
        EXPECT_FALSE(std::ranges::equal(stored, input, same));
    }
}

} // namespace
