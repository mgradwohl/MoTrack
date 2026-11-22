#include "science/analysis/data_analysis.hpp"
#include "science/analysis/fft.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <span>
#include <vector>

// Port of FourierPanel.createFourierData and FourierSinCosAnalysis.doAnalysis/getDatasets with no
// gutter (OSP d56207b0), plus FFTReal.getNaturalFreq/getNaturalOmega. The expressions keep the
// Java order; the Nyquist row and the zero-frequency sine are the documented differences.

namespace motrack::science
{
namespace
{

constexpr double PI2 = 2 * std::numbers::pi;

} // namespace

FourierSpectrum fourierSpectrum(std::span<const PlotPoint> points)
{
    FourierSpectrum spectrum;
    if (points.size() < 2)
    {
        return spectrum;
    }
    // createFourierData drops the last point of an odd count.
    const std::size_t n = points.size() - (points.size() % 2);
    const std::span<const PlotPoint> used = points.first(n);
    spectrum.pointCount = n;
    spectrum.droppedLastPoint = n != points.size();
    spectrum.origin = used[0].x;
    // doAnalysis with gutter 0: xmin = x[0] - 0 dx, xmax = x[n-1] + 1 dx, and FFTReal's frequency
    // axis assumes the spacing (xmax - xmin) / n, which equals dx only for uniform points.
    const double dx = used[1].x - used[0].x;
    const double xmin = used[0].x - 0.0 * dx;
    const double xmax = used[n - 1].x + 1.0 * dx;
    const double delta = (xmax - xmin) / static_cast<double>(n);
    spectrum.spacing = delta;
    if (delta == 0.0 || !std::isfinite(delta))
    {
        spectrum.status = FourierStatus::InvalidSpacing;
        return spectrum;
    }
    for (std::size_t i = 0; i + 1 < n; ++i)
    {
        const double error = std::abs((used[i + 1].x - used[i].x) - delta) / std::abs(delta);
        spectrum.maximumSpacingError = std::max(spectrum.maximumSpacingError, error);
    }

    std::vector<double> data(n);
    std::ranges::transform(used, data.begin(), [](const PlotPoint& point) { return point.y; });
    const bool missing = std::ranges::any_of(data, [](double value) { return std::isnan(value); });
    spectrum.status = missing ? FourierStatus::MissingValues : FourierStatus::Ok;
    static_cast<void>(realFft(data)); // n is even and at least 2

    const std::size_t half = n / 2;
    const double frequencyStep = 0.5 / static_cast<double>(half) / delta;
    const double angularStep = 0.5 / static_cast<double>(half) / (delta / PI2);
    spectrum.frequency.resize(half + 1);
    spectrum.angularFrequency.resize(half + 1);
    double frequency = 0.0;
    double angular = 0.0;
    for (std::size_t k = 0; k <= half; ++k)
    {
        spectrum.frequency[k] = frequency;
        spectrum.angularFrequency[k] = angular;
        frequency += frequencyStep;
        angular += angularStep;
    }

    const double norm = 2.0 / static_cast<double>(n);
    spectrum.cosine.resize(half + 1);
    spectrum.sine.resize(half + 1);
    spectrum.power.resize(half + 1);
    for (std::size_t k = 0; k <= half; ++k)
    {
        double cosineFactor = norm * std::cos(spectrum.angularFrequency[k] * xmin);
        double sineFactor = norm * std::sin(spectrum.angularFrequency[k] * xmin);
        double cosine = 0.0;
        double sine = 0.0;
        if (k == 0 || k == half)
        {
            // The constant and Nyquist terms have half the weight. data[1] holds X_{n/2}.
            cosineFactor *= 0.5;
            sineFactor *= 0.5;
            const double real = data[k == 0 ? 0 : 1];
            if (k == 0)
            {
                // OSP adds X_{n/2} sin(0) here, which only affects the sign of a zero.
                cosine = real * cosineFactor + data[1] * sineFactor;
            }
            else
            {
                cosine = real * cosineFactor;
                sine = real * sineFactor;
            }
        }
        else
        {
            const double real = data[2 * k];
            const double imaginary = data[(2 * k) + 1];
            cosine = real * cosineFactor + imaginary * sineFactor;
            sine = -imaginary * cosineFactor + real * sineFactor;
        }
        spectrum.cosine[k] = cosine;
        spectrum.sine[k] = sine;
        spectrum.power[k] = cosine * cosine + sine * sine;
    }
    if (missing)
    {
        // The zero-frequency sine is zero by definition, but a missing sample leaves no spectrum at all.
        spectrum.sine[0] = std::numeric_limits<double>::quiet_NaN();
    }
    return spectrum;
}

} // namespace motrack::science
