#include "science/analysis/data_analysis.hpp"
#include "science/java_math.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <vector>

// Ports of DataToolTable.getSelectedData / WorkingDataset with Dataset.append (point selection),
// DataToolStatsTable.getStatistics and DatasetCurveFitter.doLinearRegression (OSP d56207b0).

namespace motrack::science
{
namespace
{

constexpr double NOT_A_NUMBER = std::numeric_limits<double>::quiet_NaN();

double valueAt(std::span<const double> column, std::size_t row)
{
    return row < column.size() ? column[row] : NOT_A_NUMBER;
}

void appendRow(PlotData& data, std::size_t row, double x, double y)
{
    // Dataset.append skips a NaN or infinite x and an infinite y; a NaN y is a gap.
    if (std::isnan(x) || std::isinf(x) || std::isinf(y))
    {
        ++data.excludedRows;
        return;
    }
    data.points.push_back({.x = x, .y = y});
    data.rows.push_back(row);
}

/// The binary exponent e with |value| = m 2^e, 0.5 <= m < 1 (0 for a zero value).
int binaryExponent(double value)
{
    int exponent = 0;
    static_cast<void>(std::frexp(value, &exponent));
    return exponent;
}

/// Sample standard deviation of finite values, summed in the given order (OSP's iteration order).
///
/// The corrected two-pass formula (sum e^2 - (sum e)^2 / n) / (n - 1), e = d - mean, removes the
/// cancellation of OSP's one-pass formula; scaling every deviation by the same power of two is
/// exact and keeps the squares from overflowing or underflowing.
double sampleStandardDeviation(std::span<const double> values, double mean)
{
    double largest = 0.0;
    for (const double value : values)
    {
        largest = std::max(largest, std::abs(value - mean));
    }
    if (std::isinf(largest))
    {
        // A deviation overflowed (values near the largest double with both signs): rescale first.
        double magnitude = 0.0;
        for (const double value : values)
        {
            magnitude = std::max(magnitude, std::abs(value));
        }
        const int exponent = binaryExponent(magnitude);
        std::vector<double> scaled;
        scaled.reserve(values.size());
        for (const double value : values)
        {
            scaled.push_back(std::ldexp(value, -exponent));
        }
        return std::ldexp(sampleStandardDeviation(scaled, std::ldexp(mean, -exponent)), exponent);
    }
    // Callers handle equal values; frexp(0) would give exponent 0 and a zero result anyway.
    const int exponent = binaryExponent(largest);
    double sum = 0.0;
    double sumOfSquares = 0.0;
    for (const double value : values)
    {
        const double deviation = std::ldexp(value - mean, -exponent);
        sum += deviation;
        sumOfSquares += deviation * deviation;
    }
    const auto n = static_cast<double>(values.size());
    const double variance = std::max(0.0, (sumOfSquares - sum * sum / n) / (n - 1.0));
    return std::ldexp(std::sqrt(variance), exponent);
}

std::optional<double> defined(double value)
{
    return std::isnan(value) ? std::nullopt : std::optional<double>(value);
}

/// DataToolStatsTable.getStatistics exactly, from its sums: what Tracker shows (#225).
TrackerColumnStatistics trackerStatistics(double maximum, double minimum, double sum, double squareSum, std::size_t values)
{
    TrackerColumnStatistics tracker;
    const auto count = static_cast<double>(values);
    const double mean = sum / count;
    const double sd = values < 2 ? std::numeric_limits<double>::quiet_NaN() : std::sqrt((squareSum - count * mean * mean) / (count - 1));
    tracker.maximum = maximum == -std::numeric_limits<double>::max() ? std::nullopt : defined(maximum);
    tracker.minimum = minimum == std::numeric_limits<double>::max() ? std::nullopt : defined(minimum);
    tracker.mean = defined(mean);
    tracker.standardDeviation = defined(sd);
    tracker.standardError = defined(sd / std::sqrt(count));
    return tracker;
}

bool differs(std::optional<double> corrected, std::optional<double> tracker)
{
    if (corrected.has_value() != tracker.has_value())
    {
        return true;
    }
    if (!corrected || !tracker || *corrected == *tracker)
    {
        return false;
    }
    if (!std::isfinite(*corrected) || !std::isfinite(*tracker))
    {
        return true;
    }
    return std::abs(*corrected - *tracker) > TRACKER_DIFFERENCE_TOLERANCE * std::max(std::abs(*corrected), std::abs(*tracker));
}

ColumnStatistics statisticsOf(std::span<const double> data)
{
    ColumnStatistics result;
    // getStatistics iterates from the last value to the first; keep that order for every sum.
    std::vector<double> values;
    values.reserve(data.size());
    bool infinite = false;
    double maximum = 0.0;
    double minimum = 0.0;
    double trackerMaximum = -std::numeric_limits<double>::max();
    double trackerMinimum = std::numeric_limits<double>::max();
    double sum = 0.0;
    double squareSum = 0.0;
    for (const double value : std::views::reverse(data))
    {
        if (std::isnan(value))
        {
            continue;
        }
        // OSP starts from -/+Double.MAX_VALUE and reports NaN if that sentinel survives; starting
        // from the first value gives the same result for every other column.
        maximum = values.empty() ? value : java::max(maximum, value);
        minimum = values.empty() ? value : java::min(minimum, value);
        trackerMaximum = java::max(trackerMaximum, value);
        trackerMinimum = java::min(trackerMinimum, value);
        sum += value;
        squareSum += value * value;
        infinite = infinite || std::isinf(value);
        values.push_back(value);
    }
    result.count = values.size();
    if (values.empty())
    {
        return result;
    }
    result.tracker = trackerStatistics(trackerMaximum, trackerMinimum, sum, squareSum, values.size());
    result.maximum = maximum;
    result.minimum = minimum;
    const auto count = static_cast<double>(values.size());
    double mean = sum / count;
    if (std::isinf(sum) && !infinite)
    {
        // Finite values whose sum overflows: OSP reports an infinite mean.
        const int exponent = binaryExponent(std::max(std::abs(maximum), std::abs(minimum)));
        double scaledSum = 0.0;
        for (const double value : values)
        {
            scaledSum += std::ldexp(value, -exponent);
        }
        mean = std::ldexp(scaledSum / count, exponent);
    }
    result.mean = defined(mean);
    if (values.size() >= 2 && !infinite)
    {
        const double deviation = maximum == minimum ? 0.0 : sampleStandardDeviation(values, mean);
        result.standardDeviation = deviation;
        result.standardError = deviation / std::sqrt(count);
    }
    const TrackerColumnStatistics& tracker = result.tracker;
    result.correctedFromTracker = differs(result.maximum, tracker.maximum) || differs(result.minimum, tracker.minimum) ||
                                  differs(result.mean, tracker.mean) || differs(result.standardDeviation, tracker.standardDeviation) ||
                                  differs(result.standardError, tracker.standardError);
    return result;
}
} // namespace

PlotData plotData(std::span<const double> x, std::span<const double> y)
{
    PlotData data;
    for (std::size_t row = 0; row < x.size(); ++row)
    {
        appendRow(data, row, x[row], valueAt(y, row));
    }
    return data;
}

PlotData plotData(std::span<const double> x, std::span<const double> y, std::span<const std::size_t> rows)
{
    PlotData data;
    for (const std::size_t row : rows)
    {
        appendRow(data, row, valueAt(x, row), valueAt(y, row));
    }
    return data;
}

ColumnStatistics columnStatistics(std::span<const double> values)
{
    return statisticsOf(values);
}

ColumnStatistics columnStatistics(std::span<const double> values, std::span<const std::size_t> rows)
{
    std::vector<double> selected;
    selected.reserve(rows.size());
    for (const std::size_t row : rows)
    {
        selected.push_back(valueAt(values, row));
    }
    return statisticsOf(selected);
}

std::optional<double> squaredCorrelation(std::span<const PlotPoint> points)
{
    // doLinearRegression receives Dataset.getValidXPoints/getValidYPoints: the points with a y.
    std::vector<PlotPoint> valid;
    valid.reserve(points.size());
    std::ranges::copy_if(points, std::back_inserter(valid), [](const PlotPoint& point) { return !std::isnan(point.y); });
    if (valid.size() < 3)
    {
        return std::nullopt;
    }
    const auto n = static_cast<double>(valid.size());
    double meanX = valid[0].x;
    double meanY = valid[0].y;
    for (std::size_t i = 1; i < valid.size(); ++i)
    {
        meanX += valid[i].x;
        meanY += valid[i].y;
    }
    meanX /= n;
    meanY /= n;
    double sumSquaresX = 0.0;
    double sumSquaresY = 0.0;
    double sumCoproduct = 0.0;
    for (const PlotPoint& point : valid)
    {
        const double deltaX = point.x - meanX;
        const double deltaY = point.y - meanY;
        sumSquaresX += deltaX * deltaX;
        sumSquaresY += deltaY * deltaY;
        sumCoproduct += deltaX * deltaY;
    }
    if (sumSquaresX == 0 || sumSquaresY == 0)
    {
        return std::nullopt;
    }
    const double populationVarianceX = sumSquaresX / n;
    const double populationVarianceY = sumSquaresY / n;
    const double covariance = sumCoproduct / n;
    const double correlation = covariance * covariance / (populationVarianceX * populationVarianceY);
    if (!std::isfinite(correlation))
    {
        return std::nullopt;
    }
    return correlation;
}

} // namespace motrack::science
