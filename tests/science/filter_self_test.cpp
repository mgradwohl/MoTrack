#include "science/motion/motion_filters.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

// The six checks of Tracker's src/test/FilterSelfTest.java (SwingJS tip 34b1b503, unchanged since it was
// added in 197225dc), with the same signals and pass conditions. Tracker passes a data array and a valid
// array; here an invalid sample is std::nullopt, and "passes through unchanged" means it stays missing.

namespace
{

namespace science = motrack::science;

/// The value of an optional that the test requires to be present; a missing value fails the test.
template <typename T> const T& present(const std::optional<T>& value)
{
    if (!value)
    {
        throw std::logic_error("expected a value");
    }
    return *value;
}

std::vector<std::optional<double>> present(std::span<const double> values)
{
    return {values.begin(), values.end()};
}

double rms(std::span<const std::optional<double>> values)
{
    double sum = 0;
    int count = 0;
    for (const auto& value : values)
    {
        if (value)
        {
            sum += *value * *value;
            ++count;
        }
    }
    return std::sqrt(sum / count);
}

double peakAmplitude(std::span<const std::optional<double>> values, std::size_t from, std::size_t to)
{
    double peak = 0;
    for (std::size_t i = from; i < to; ++i)
    {
        peak = std::max(peak, std::fabs(values[i].value_or(0.0)));
    }
    return peak;
}

TEST(FilterSelfTest, MovingAverageReducesNoise)
{
    constexpr int COUNT = 200;
    std::vector<double> x(COUNT);
    double seedState = 42;
    for (int i = 0; i < COUNT; ++i)
    {
        seedState = std::fmod(seedState * 1103515245.0 + 12345.0, static_cast<double>(std::int64_t{1} << 31));
        const double noise = ((seedState / static_cast<double>(std::int64_t{1} << 31)) - 0.5) * 0.4;
        x[i] = std::sin(2 * std::numbers::pi * i / 50.0) + noise;
    }
    const auto input = present(x);
    const auto output = science::applyMotionFilter(science::MovingAverageFilter(7), input);
    EXPECT_LT(rms(output), rms(input)) << "MovingAverage reduces RMS energy of noisy signal";
}

TEST(FilterSelfTest, MovingAverageHandlesInvalidGap)
{
    const std::vector<std::optional<double>> x = {0, 1, 2, std::nullopt, std::nullopt, 5, 6, 7};
    const auto y = science::applyMotionFilter(science::MovingAverageFilter(3), x);
    // Invalid entries pass through; the segment after the gap is averaged within itself:
    // y[5] uses [5, 6] (clamped to the segment start) -> 5.5.
    EXPECT_FALSE(y[3].has_value());
    EXPECT_FALSE(y[4].has_value());
    ASSERT_TRUE(y[5] && y[6] && y[7]);
    EXPECT_NEAR(present(y[5]), 5.5, 1e-9);
    EXPECT_NEAR(present(y[6]), 6.0, 1e-9);
    EXPECT_NEAR(present(y[7]), 6.5, 1e-9);
}

TEST(FilterSelfTest, ButterworthAttenuatesAboveCutoff)
{
    constexpr double FS = 100.0;
    constexpr std::size_t COUNT = 1024;
    std::vector<double> x(COUNT);
    for (std::size_t i = 0; i < COUNT; ++i)
    {
        x[i] = std::sin(2 * std::numbers::pi * 20.0 * static_cast<double>(i) / FS);
    }
    const auto input = present(x);
    const auto output = science::applyMotionFilter(science::ButterworthFilter(4, 5.0, FS), input);
    const double ratio = peakAmplitude(output, 50, COUNT - 50) / peakAmplitude(input, 50, COUNT - 50);
    EXPECT_LT(ratio, 0.05) << "Butterworth attenuates 20 Hz with 5 Hz cutoff: ratio=" << ratio;
}

TEST(FilterSelfTest, ButterworthPreservesDc)
{
    constexpr std::size_t COUNT = 256;
    const std::vector<std::optional<double>> x(COUNT, 3.7);
    const auto y = science::applyMotionFilter(science::ButterworthFilter(4, 5.0, 100.0), x);
    double maxError = 0;
    for (std::size_t i = 20; i < COUNT - 20; ++i)
    {
        ASSERT_TRUE(y[i].has_value());
        maxError = std::max(maxError, std::fabs(present(y[i]) - 3.7));
    }
    EXPECT_LT(maxError, 1e-6) << "Butterworth preserves DC: " << maxError;
}

TEST(FilterSelfTest, ButterworthZeroPhase)
{
    constexpr double FS = 100.0;
    constexpr std::size_t COUNT = 512;
    std::vector<double> x(COUNT);
    for (std::size_t i = 0; i < COUNT; ++i)
    {
        x[i] = std::sin(2 * std::numbers::pi * 2.0 * static_cast<double>(i) / FS);
    }
    const auto y = science::applyMotionFilter(science::ButterworthFilter(4, 10.0, FS), present(x));
    int zeroX = -1;
    int zeroY = -1;
    for (int i = 100; i < 200; ++i)
    {
        const auto index = static_cast<std::size_t>(i);
        if (zeroX < 0 && x[index] >= 0 && x[index + 1] < 0)
        {
            zeroX = i;
        }
        if (zeroY < 0 && y[index].value_or(-1) >= 0 && y[index + 1].value_or(1) < 0)
        {
            zeroY = i;
        }
    }
    EXPECT_GT(zeroX, 0);
    EXPECT_GT(zeroY, 0);
    EXPECT_LE(std::abs(zeroX - zeroY), 2) << "Butterworth zero-phase: zeroX=" << zeroX << " zeroY=" << zeroY;
}

TEST(FilterSelfTest, SavitzkyGolayPreservesQuadratic)
{
    constexpr int COUNT = 50;
    std::vector<double> x(COUNT);
    for (int i = 0; i < COUNT; ++i)
    {
        const double t = i;
        x[static_cast<std::size_t>(i)] = 1.0 + 2.0 * t + 0.5 * t * t;
    }
    const auto y = science::applyMotionFilter(science::SavitzkyGolayFilter(7, 2), present(x));
    double maxError = 0;
    for (std::size_t i = 0; i < x.size(); ++i)
    {
        ASSERT_TRUE(y[i].has_value());
        maxError = std::max(maxError, std::fabs(present(y[i]) - x[i]));
    }
    EXPECT_LT(maxError, 1e-6) << "SavitzkyGolay (poly=2) reproduces quadratic exactly: maxErr=" << maxError;
}

} // namespace
