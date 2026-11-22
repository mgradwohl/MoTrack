#include "science/motion/motion_filters.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

// Each expression keeps the operand order of Tracker's Java source (MovingAverageFilter,
// ButterworthFilter, SavitzkyGolayFilter and MotionFilterSupport); see motion_derivatives.cpp.

namespace motrack::science
{

MovingAverageFilter::MovingAverageFilter(int window) noexcept : m_Window(std::max(window, 1))
{
    if (m_Window % 2 == 0)
    {
        ++m_Window;
    }
}

int MovingAverageFilter::window() const noexcept
{
    return m_Window;
}

ButterworthFilter::ButterworthFilter(int order, double cutoffHz, double sampleRateHz) noexcept
    : m_Order(std::clamp(order, 1, 8)), m_CutoffHz(cutoffHz <= 0 ? 1.0 : cutoffHz), m_SampleRateHz(sampleRateHz <= 0 ? 1.0 : sampleRateHz)
{}

int ButterworthFilter::order() const noexcept
{
    return m_Order;
}

double ButterworthFilter::cutoffHz() const noexcept
{
    return m_CutoffHz;
}

double ButterworthFilter::sampleRateHz() const noexcept
{
    return m_SampleRateHz;
}

SavitzkyGolayFilter::SavitzkyGolayFilter(int window, int polynomialOrder) noexcept
    : m_Window(std::max(window, 3)), m_PolynomialOrder(std::max(polynomialOrder, 1))
{
    if (m_Window % 2 == 0)
    {
        ++m_Window;
    }
    if (m_PolynomialOrder >= m_Window)
    {
        m_PolynomialOrder = m_Window - 1;
    }
}

int SavitzkyGolayFilter::window() const noexcept
{
    return m_Window;
}

int SavitzkyGolayFilter::polynomialOrder() const noexcept
{
    return m_PolynomialOrder;
}

double butterworthSampleRateHz(double meanStepDurationMs) noexcept
{
    return meanStepDurationMs > 0 ? 1000.0 / meanStepDurationMs : 30.0;
}

namespace
{

using Index = std::ptrdiff_t;

/// MotionFilterSupport.Segment: a run [start, end) of present samples.
struct Segment
{
    Index start;
    Index end;

    [[nodiscard]] Index length() const noexcept
    {
        return end - start;
    }
};

double& at(std::vector<double>& values, Index index)
{
    return values[static_cast<std::size_t>(index)];
}

double at(std::span<const double> values, Index index)
{
    return values[static_cast<std::size_t>(index)];
}

std::vector<Segment> contiguousValidSegments(std::span<const double> data)
{
    std::vector<Segment> result;
    const auto size = std::ssize(data);
    Index index = 0;
    while (index < size)
    {
        while (index < size && std::isnan(at(data, index)))
        {
            ++index;
        }
        const Index start = index;
        while (index < size && !std::isnan(at(data, index)))
        {
            ++index;
        }
        if (index > start)
        {
            result.push_back({.start = start, .end = index});
        }
    }
    return result;
}

void movingAverage(const MovingAverageFilter& filter, std::span<const double> data, std::vector<double>& out)
{
    const Index half = filter.window() / 2;
    for (const auto& segment : contiguousValidSegments(data))
    {
        for (Index i = segment.start; i < segment.end; ++i)
        {
            const Index low = std::max(segment.start, i - half);
            const Index high = std::min(segment.end - 1, i + half);
            double sum = 0;
            Index count = 0;
            for (Index k = low; k <= high; ++k)
            {
                sum += at(data, k);
                ++count;
            }
            at(out, i) = sum / static_cast<double>(count);
        }
    }
}

/// A direct-form II transposed biquad section.
struct Biquad
{
    double b0;
    double b1;
    double b2;
    double a1;
    double a2;

    /// Filters with states initialised as if x[0] had been the input forever (unity DC gain). The padded
    /// runs passed here always hold at least three samples.
    [[nodiscard]] std::vector<double> filter(const std::vector<double>& x) const
    {
        const double v = x.front();
        double s1 = v * (1.0 - b0);
        double s2 = v * (b2 - a2);
        std::vector<double> y(x.size());
        for (std::size_t i = 0; i < x.size(); ++i)
        {
            const double xi = x[i];
            const double yi = b0 * xi + s1;
            s1 = b1 * xi - a1 * yi + s2;
            s2 = b2 * xi - a2 * yi;
            y[i] = yi;
        }
        return y;
    }
};

/// ButterworthFilter.designBiquads: analog prototype poles, prewarped bilinear transform, a first-order
/// section first for an odd order, and every section normalised to unity gain at DC.
std::vector<Biquad> designBiquads(int order, double fc, double fs)
{
    const double omegaPrewarped = 2.0 * fs * std::tan(std::numbers::pi * fc / fs);
    const double k2 = 2.0 * fs;
    std::vector<Biquad> sections;
    sections.reserve(static_cast<std::size_t>((order + 1) / 2));
    if (order % 2 == 1)
    {
        const double pole = -omegaPrewarped;
        const double zPole = (k2 + pole) / (k2 - pole);
        const double a1 = -zPole;
        const double b0 = (1.0 + a1) / 2.0;
        sections.push_back({.b0 = b0, .b1 = b0, .b2 = 0.0, .a1 = a1, .a2 = 0.0});
    }
    const int pairs = order / 2;
    for (int k = 1; k <= pairs; ++k)
    {
        const double theta = std::numbers::pi * (2 * k - 1 + order) / (2.0 * order);
        const double pr = omegaPrewarped * std::cos(theta);
        const double pi = omegaPrewarped * std::sin(theta);
        const double aReal = k2 - pr;
        const double aImag = -pi;
        const double bReal = k2 + pr;
        const double bImag = pi;
        const double denomMag = aReal * aReal + aImag * aImag;
        const double zReal = (bReal * aReal + bImag * aImag) / denomMag;
        const double zImag = (bImag * aReal - bReal * aImag) / denomMag;
        const double a1 = -2.0 * zReal;
        const double a2 = zReal * zReal + zImag * zImag;
        const double b0 = (1.0 + a1 + a2) / 4.0;
        sections.push_back({.b0 = b0, .b1 = 2.0 * b0, .b2 = b0, .a1 = a1, .a2 = a2});
    }
    return sections;
}

/// MotionFilterSupport.reflectPad: odd reflection about each end of [start, end).
std::vector<double> reflectPad(std::span<const double> source, Index start, Index end, Index pad)
{
    const Index n = end - start;
    std::vector<double> out(static_cast<std::size_t>(n + 2 * pad));
    for (Index i = 0; i < n; ++i)
    {
        at(out, pad + i) = at(source, start + i);
    }
    const double s0 = at(source, start);
    const double sN = at(source, end - 1);
    for (Index k = 0; k < pad; ++k)
    {
        const Index sourceIndex = std::min(n - 1, k + 1);
        at(out, pad - 1 - k) = 2 * s0 - at(source, start + sourceIndex);
        const Index sourceIndex2 = std::max(Index{0}, n - 2 - k);
        at(out, pad + n + k) = 2 * sN - at(source, start + sourceIndex2);
    }
    return out;
}

std::vector<double> filterCascade(const std::vector<Biquad>& sections, std::vector<double> x)
{
    for (const auto& section : sections)
    {
        x = section.filter(x);
    }
    return x;
}

void butterworth(const ButterworthFilter& filter, std::span<const double> data, std::vector<double>& out)
{
    const double nyquist = filter.sampleRateHz() / 2.0;
    if (filter.cutoffHz() >= nyquist)
    {
        return;
    }
    const auto sections = designBiquads(filter.order(), filter.cutoffHz(), filter.sampleRateHz());
    const auto sectionCount = std::ssize(sections);
    const Index minLength = 3 * (sectionCount * 2 + 1);
    for (const auto& segment : contiguousValidSegments(data))
    {
        if (segment.length() < 3)
        {
            continue;
        }
        const Index pad = std::min(segment.length() - 1, std::max(minLength, sectionCount * 6));
        auto forward = filterCascade(sections, reflectPad(data, segment.start, segment.end, pad));
        std::ranges::reverse(forward);
        auto backward = filterCascade(sections, std::move(forward));
        std::ranges::reverse(backward);
        for (Index i = 0; i < segment.length(); ++i)
        {
            at(out, segment.start + i) = at(backward, pad + i);
        }
    }
}

using Matrix = std::vector<std::vector<double>>;

/// SavitzkyGolayFilter.invert: Gauss-Jordan elimination with partial pivoting. The normal matrix is
/// positive definite (window > order), so no pivot is zero.
Matrix invert(const Matrix& m)
{
    const std::size_t n = m.size();
    Matrix a(n, std::vector<double>(2 * n));
    for (std::size_t i = 0; i < n; ++i)
    {
        std::ranges::copy(m[i], a[i].begin());
        a[i][n + i] = 1.0;
    }
    for (std::size_t col = 0; col < n; ++col)
    {
        std::size_t pivot = col;
        double pivotMag = std::fabs(a[col][col]);
        for (std::size_t r = col + 1; r < n; ++r)
        {
            if (std::fabs(a[r][col]) > pivotMag)
            {
                pivot = r;
                pivotMag = std::fabs(a[r][col]);
            }
        }
        if (pivot != col)
        {
            std::swap(a[col], a[pivot]);
        }
        const double diag = a[col][col];
        for (std::size_t j = 0; j < 2 * n; ++j)
        {
            a[col][j] /= diag;
        }
        for (std::size_t r = 0; r < n; ++r)
        {
            if (r == col)
            {
                continue;
            }
            const double factor = a[r][col];
            if (factor == 0)
            {
                continue;
            }
            for (std::size_t j = 0; j < 2 * n; ++j)
            {
                a[r][j] -= factor * a[col][j];
            }
        }
    }
    Matrix inverse(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        inverse[i].assign(a[i].begin() + static_cast<Index>(n), a[i].end());
    }
    return inverse;
}

/// SavitzkyGolayFilter.computeCoefficients: weights of a window of w samples whose least-squares
/// polynomial of order p is evaluated at integer offset t from the window centre.
std::vector<double> savitzkyGolayCoefficients(Index w, Index p, Index t)
{
    const Index half = w / 2;
    const auto columns = static_cast<std::size_t>(p + 1);
    Matrix j(static_cast<std::size_t>(w), std::vector<double>(columns));
    for (Index i = 0; i < w; ++i)
    {
        const auto xi = static_cast<double>(i - half);
        double power = 1.0;
        for (std::size_t k = 0; k < columns; ++k)
        {
            j[static_cast<std::size_t>(i)][k] = power;
            power *= xi;
        }
    }
    Matrix normal(columns, std::vector<double>(columns));
    for (std::size_t a = 0; a < columns; ++a)
    {
        for (std::size_t b = 0; b < columns; ++b)
        {
            double sum = 0;
            for (const auto& row : j)
            {
                sum += row[a] * row[b];
            }
            normal[a][b] = sum;
        }
    }
    const Matrix inverse = invert(normal);
    std::vector<double> tPow(columns);
    double tp = 1.0;
    for (auto& value : tPow)
    {
        value = tp;
        tp *= static_cast<double>(t);
    }
    std::vector<double> evaluation(columns);
    for (std::size_t a = 0; a < columns; ++a)
    {
        double sum = 0;
        for (std::size_t b = 0; b < columns; ++b)
        {
            sum += inverse[a][b] * tPow[b];
        }
        evaluation[a] = sum;
    }
    std::vector<double> coefficients(static_cast<std::size_t>(w));
    for (std::size_t i = 0; i < coefficients.size(); ++i)
    {
        double sum = 0;
        for (std::size_t a = 0; a < columns; ++a)
        {
            sum += evaluation[a] * j[i][a];
        }
        coefficients[i] = sum;
    }
    return coefficients;
}

double weightedSum(const std::vector<double>& coefficients, std::span<const double> data, Index first)
{
    double sum = 0;
    for (std::size_t k = 0; k < coefficients.size(); ++k)
    {
        sum += coefficients[k] * at(data, first + static_cast<Index>(k));
    }
    return sum;
}

void savitzkyGolay(const SavitzkyGolayFilter& filter, std::span<const double> data, std::vector<double>& out)
{
    const Index order = filter.polynomialOrder();
    for (const auto& segment : contiguousValidSegments(data))
    {
        const Index n = segment.length();
        if (n < order + 1)
        {
            continue;
        }
        Index effective = std::min(Index{filter.window()}, n);
        if (effective % 2 == 0)
        {
            --effective;
        }
        const Index halfEffective = effective / 2;
        const Index p = std::min(order, effective - 1);
        const auto centre = savitzkyGolayCoefficients(effective, p, 0);
        for (Index i = halfEffective; i < n - halfEffective; ++i)
        {
            at(out, segment.start + i) = weightedSum(centre, data, segment.start + i - halfEffective);
        }
        for (Index i = 0; i < halfEffective; ++i)
        {
            at(out, segment.start + i) = weightedSum(savitzkyGolayCoefficients(effective, p, i - halfEffective), data, segment.start);
        }
        for (Index i = n - halfEffective; i < n; ++i)
        {
            const auto coefficients = savitzkyGolayCoefficients(effective, p, (i - (n - 1)) + halfEffective);
            at(out, segment.start + i) = weightedSum(coefficients, data, segment.start + n - effective);
        }
    }
}

constexpr double NOT_A_NUMBER = std::numeric_limits<double>::quiet_NaN();

/// Gaps (missing or non-finite samples) become NaN, which no filter reads.
std::vector<double> withGaps(std::span<const std::optional<double>> samples)
{
    std::vector<double> data(samples.size(), NOT_A_NUMBER);
    for (std::size_t i = 0; i < samples.size(); ++i)
    {
        if (const auto& sample = samples[i]; sample && std::isfinite(*sample))
        {
            data[i] = *sample;
        }
    }
    return data;
}

std::vector<double> filtered(const MotionFilter& filter, const std::vector<double>& data)
{
    std::vector<double> out = data;
    std::visit(
        [&](const auto& concrete)
        {
            using Filter = std::decay_t<decltype(concrete)>;
            if constexpr (std::is_same_v<Filter, MovingAverageFilter>)
            {
                movingAverage(concrete, data, out);
            }
            else if constexpr (std::is_same_v<Filter, ButterworthFilter>)
            {
                butterworth(concrete, data, out);
            }
            else
            {
                savitzkyGolay(concrete, data, out);
            }
        },
        filter);
    return out;
}

} // namespace

std::vector<std::optional<double>> applyMotionFilter(const MotionFilter& filter, std::span<const std::optional<double>> samples)
{
    const auto data = withGaps(samples);
    const auto out = filtered(filter, data);
    std::vector<std::optional<double>> result(samples.size());
    for (std::size_t i = 0; i < samples.size(); ++i)
    {
        if (!std::isnan(data[i]) && std::isfinite(out[i]))
        {
            result[i] = out[i];
        }
    }
    return result;
}

std::vector<std::optional<PlanarValue>> filterPositions(const MotionFilter& filter, std::span<const std::optional<PlanarValue>> positions)
{
    std::vector<std::optional<double>> x(positions.size());
    std::vector<std::optional<double>> y(positions.size());
    for (std::size_t i = 0; i < positions.size(); ++i)
    {
        // FilteredPointMass: a step is valid only if both world coordinates are numbers.
        if (const auto& position = positions[i]; position && std::isfinite(position->x) && std::isfinite(position->y))
        {
            x[i] = position->x;
            y[i] = position->y;
        }
    }
    const auto filteredX = applyMotionFilter(filter, x);
    const auto filteredY = applyMotionFilter(filter, y);
    std::vector<std::optional<PlanarValue>> result(positions.size());
    for (std::size_t i = 0; i < positions.size(); ++i)
    {
        const auto& smoothX = filteredX[i];
        const auto& smoothY = filteredY[i];
        if (smoothX && smoothY)
        {
            result[i] = PlanarValue{.x = *smoothX, .y = *smoothY};
        }
    }
    return result;
}

std::optional<FilterDeviation> filterRmsDeviation(std::span<const std::optional<PlanarValue>> source,
                                                  std::span<const std::optional<PlanarValue>> filtered)
{
    if (source.size() != filtered.size())
    {
        throw std::invalid_argument("Source and filtered positions differ in length");
    }
    double sumOfSquaresX = 0;
    double sumOfSquaresY = 0;
    std::size_t count = 0;
    for (std::size_t i = 0; i < source.size(); ++i)
    {
        const auto& original = source[i];
        const auto& smoothed = filtered[i];
        // FilteredPointMass counts only valid steps: present, with numeric world coordinates.
        if (!original || !smoothed || !std::isfinite(original->x) || !std::isfinite(original->y) || !std::isfinite(smoothed->x) ||
            !std::isfinite(smoothed->y))
        {
            continue;
        }
        ++count;
        double deviation = smoothed->x - original->x;
        sumOfSquaresX += deviation * deviation;
        deviation = smoothed->y - original->y;
        sumOfSquaresY += deviation * deviation;
    }
    if (count == 0)
    {
        return std::nullopt;
    }
    const double x = std::sqrt(sumOfSquaresX / static_cast<double>(count));
    const double y = std::sqrt(sumOfSquaresY / static_cast<double>(count));
    return FilterDeviation{.x = x, .y = y, .combined = std::sqrt(x * x + y * y)};
}

} // namespace motrack::science
