#include "science/analysis/fft.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <vector>

namespace
{

using motrack::science::complexFft;
using motrack::science::realFft;

// Independent reference: the O(n^2) definition X_k = sum_j z_j exp(-2 pi i j k / n), with the
// angle reduced exactly (j k mod n) so the reference itself stays accurate for every length.
std::vector<std::complex<double>> naiveDft(std::span<const std::complex<double>> input)
{
    const std::size_t n = input.size();
    std::vector<std::complex<double>> output(n);
    for (std::size_t k = 0; k < n; ++k)
    {
        std::complex<double> sum;
        for (std::size_t j = 0; j < n; ++j)
        {
            const double angle = -2.0 * std::numbers::pi * static_cast<double>((j * k) % n) / static_cast<double>(n);
            sum += input[j] * std::complex<double>(std::cos(angle), std::sin(angle));
        }
        output[k] = sum;
    }
    return output;
}

/// Deterministic values in [-1, 1) from the oracle's LCG (tools/reference/AnalysisOracle.java).
std::vector<double> signedValues(std::uint64_t seed, std::size_t count)
{
    std::vector<double> values(count);
    for (double& value : values)
    {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        value = 2.0 * (static_cast<double>(seed >> 11U) * 0x1.0p-53) - 1.0;
    }
    return values;
}

double magnitudeSum(std::span<const double> values)
{
    double sum = 0.0;
    for (const double value : values)
    {
        sum += std::abs(value);
    }
    return sum;
}

/// Rounding error bound of an FFT: a few units of epsilon per stage, relative to the input scale.
double fftTolerance(std::span<const double> values, std::size_t n)
{
    return 16.0 * (1.0 + std::log2(static_cast<double>(n))) * 0x1.0p-52 * magnitudeSum(values);
}

TEST(Fft, ComplexTransformMatchesTheDefinitionForEveryFactorization)
{
    // 1..64 covers radix 2, 3, 4, 5, 6, 7 and the general pass (11, 13, 17, ...), alone and mixed.
    for (std::size_t n = 1; n <= 64; ++n)
    {
        const std::vector<double> input = signedValues(n, 2 * n);
        std::vector<std::complex<double>> values(n);
        for (std::size_t j = 0; j < n; ++j)
        {
            values[j] = {input[2 * j], input[(2 * j) + 1]};
        }
        const auto expected = naiveDft(values);
        std::vector<double> data = input;
        ASSERT_TRUE(complexFft(data));
        const double tolerance = fftTolerance(input, n);
        for (std::size_t k = 0; k < n; ++k)
        {
            EXPECT_NEAR(data[2 * k], expected[k].real(), tolerance) << "n = " << n << ", k = " << k;
            EXPECT_NEAR(data[(2 * k) + 1], expected[k].imag(), tolerance) << "n = " << n << ", k = " << k;
        }
    }
}

TEST(Fft, LargePrimeAndCompositeLengthsMatchTheDefinition)
{
    for (const std::size_t n : {97U, 121U, 143U, 210U, 343U, 1024U})
    {
        const std::vector<double> input = signedValues(1000 + n, 2 * n);
        std::vector<std::complex<double>> values(n);
        for (std::size_t j = 0; j < n; ++j)
        {
            values[j] = {input[2 * j], input[(2 * j) + 1]};
        }
        const auto expected = naiveDft(values);
        std::vector<double> data = input;
        ASSERT_TRUE(complexFft(data));
        const double tolerance = fftTolerance(input, n);
        for (std::size_t k = 0; k < n; ++k)
        {
            ASSERT_NEAR(data[2 * k], expected[k].real(), tolerance) << "n = " << n << ", k = " << k;
            ASSERT_NEAR(data[(2 * k) + 1], expected[k].imag(), tolerance) << "n = " << n << ", k = " << k;
        }
    }
}

TEST(Fft, RealTransformPacksTheHalfSpectrumForEveryEvenLength)
{
    // n = 10, 14, 18, ... have an odd half length, where OSP leaves one pair untransformed.
    for (std::size_t n = 2; n <= 130; n += 2)
    {
        const std::vector<double> input = signedValues(500 + n, n);
        std::vector<std::complex<double>> values(input.begin(), input.end());
        const auto expected = naiveDft(values);
        std::vector<double> data = input;
        ASSERT_TRUE(realFft(data));
        const double tolerance = fftTolerance(input, n);
        EXPECT_NEAR(data[0], expected[0].real(), tolerance) << "n = " << n;
        EXPECT_NEAR(data[1], expected[n / 2].real(), tolerance) << "n = " << n;
        for (std::size_t k = 1; k < n / 2; ++k)
        {
            EXPECT_NEAR(data[2 * k], expected[k].real(), tolerance) << "n = " << n << ", k = " << k;
            EXPECT_NEAR(data[(2 * k) + 1], expected[k].imag(), tolerance) << "n = " << n << ", k = " << k;
        }
    }
}

TEST(Fft, TransformsAreLinearAndConserveEnergy)
{
    const std::size_t n = 60;
    const std::vector<double> first = signedValues(3, n);
    const std::vector<double> second = signedValues(4, n);
    std::vector<double> sum(n);
    double energy = 0.0;
    for (std::size_t i = 0; i < n; ++i)
    {
        sum[i] = first[i] + 2.0 * second[i];
        energy += sum[i] * sum[i];
    }
    std::vector<double> a = first;
    std::vector<double> b = second;
    std::vector<double> c = sum;
    ASSERT_TRUE(realFft(a));
    ASSERT_TRUE(realFft(b));
    ASSERT_TRUE(realFft(c));
    // Parseval for the packed half spectrum: n sum y^2 = X_0^2 + X_{n/2}^2 + 2 sum_{0<k<n/2} |X_k|^2.
    double spectral = c[0] * c[0] + c[1] * c[1];
    for (std::size_t i = 0; i < n; ++i)
    {
        EXPECT_NEAR(c[i], a[i] + 2.0 * b[i], 1e-12);
        if (i >= 2)
        {
            spectral += 2.0 * c[i] * c[i];
        }
    }
    EXPECT_NEAR(spectral, static_cast<double>(n) * energy, 1e-10 * static_cast<double>(n) * energy);
}

TEST(Fft, KnownSignalsHaveExactBins)
{
    // A constant and the alternating sequence are the zero and Nyquist frequencies.
    std::vector<double> constant(8, 1.5);
    ASSERT_TRUE(realFft(constant));
    EXPECT_DOUBLE_EQ(constant[0], 12.0);
    EXPECT_DOUBLE_EQ(constant[1], 0.0);
    std::vector<double> alternating{1, -1, 1, -1, 1, -1};
    ASSERT_TRUE(realFft(alternating));
    EXPECT_DOUBLE_EQ(alternating[0], 0.0);
    EXPECT_DOUBLE_EQ(alternating[1], 6.0);
    // One complex point is its own transform.
    std::vector<double> single{0.25, -4.0};
    ASSERT_TRUE(complexFft(single));
    EXPECT_EQ(single, (std::vector<double>{0.25, -4.0}));
    // A cosine of three cycles in ten samples (the case OSP mis-unpacks) lands in bin 3 only.
    std::vector<double> cosine(10);
    for (std::size_t j = 0; j < cosine.size(); ++j)
    {
        cosine[j] = std::cos(2.0 * std::numbers::pi * 3.0 * static_cast<double>(j) / 10.0);
    }
    ASSERT_TRUE(realFft(cosine));
    for (std::size_t k = 1; k < 5; ++k)
    {
        EXPECT_NEAR(cosine[2 * k], k == 3 ? 5.0 : 0.0, 1e-14) << k;
        EXPECT_NEAR(cosine[(2 * k) + 1], 0.0, 1e-14) << k;
    }
}

TEST(Fft, RejectsEmptyAndOddInputsWithoutChangingThem)
{
    std::vector<double> empty;
    EXPECT_FALSE(complexFft(empty));
    EXPECT_FALSE(realFft(empty));
    std::vector<double> odd{1.0, 2.0, 3.0};
    EXPECT_FALSE(complexFft(odd));
    EXPECT_FALSE(realFft(odd));
    EXPECT_EQ(odd, (std::vector<double>{1.0, 2.0, 3.0}));
}

TEST(Fft, MissingValuesPropagateToEveryOutput)
{
    std::vector<double> data{1.0, 2.0, NAN, 4.0, 5.0, 6.0, 7.0, 8.0};
    ASSERT_TRUE(realFft(data));
    for (const double value : data)
    {
        EXPECT_TRUE(std::isnan(value));
    }
}

} // namespace
