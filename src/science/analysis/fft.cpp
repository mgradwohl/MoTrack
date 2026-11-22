#include "science/analysis/fft.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <span>
#include <vector>

// Ports org.opensourcephysics.numerics.FFT and FFTReal (OSP d56207b0), which derive from GSL's
// mixed-radix FFT. Every expression keeps the Java operation order so that, with
// -ffp-contract=off, only the Math.sin/Math.cos twiddle factors can differ from Java (see
// docs/data-analysis-kernels.md). Java's `int sign` multiplies a double exactly, as here.

namespace motrack::science
{
namespace
{

constexpr double PI = std::numbers::pi;
constexpr int FORWARD = -1;
// Data and scratch both hold interleaved complex values, so every pass uses a stride of two.
constexpr std::size_t STRIDE = 2;

/// GSL `fft_complex_factorize` as OSP's FFT.factor: preferred factors first, then odd primes.
std::vector<std::size_t> factorize(std::size_t n)
{
    std::vector<std::size_t> factors;
    std::size_t remaining = n;
    for (const std::size_t preferred : {7U, 6U, 5U, 4U, 3U, 2U})
    {
        if (remaining == 1)
        {
            break;
        }
        while (remaining % preferred == 0)
        {
            remaining /= preferred;
            factors.push_back(preferred);
        }
    }
    // OSP then removes remaining factors of two, which the preferred list has already taken.
    std::size_t factor = 3;
    while (remaining != 1)
    {
        while (remaining % factor != 0)
        {
            factor += 2;
        }
        remaining /= factor;
        factors.push_back(factor);
    }
    return factors;
}

/// Twiddle factors of one pass: rows k = 0..q, each {cos, sin} for j = 1..factor-1.
struct PassTwiddles
{
    std::size_t factor = 0;
    std::vector<double> values;

    [[nodiscard]] std::span<const double> row(std::size_t k) const
    {
        const std::size_t width = 2 * (factor - 1);
        return std::span<const double>(values).subspan(k * width, width);
    }
};

class ComplexTransform
{
  public:
    explicit ComplexTransform(std::size_t n) : m_N(n), m_Scratch(2 * n)
    {
        // FFT.setup_wavetable
        const double dTheta = -2.0 * PI / static_cast<double>(n);
        std::size_t product = 1;
        for (const std::size_t factor : factorize(n))
        {
            const std::size_t previousProduct = product;
            product *= factor;
            const std::size_t q = n / product;
            const std::size_t width = 2 * (factor - 1);
            PassTwiddles pass{.factor = factor, .values = std::vector<double>((q + 1) * width)};
            for (std::size_t j = 1; j < factor; ++j)
            {
                pass.values[2 * (j - 1)] = 1.0;
                pass.values[(2 * (j - 1)) + 1] = 0.0;
            }
            for (std::size_t k = 1; k <= q; ++k)
            {
                std::size_t m = 0;
                for (std::size_t j = 1; j < factor; ++j)
                {
                    m += k * previousProduct;
                    m %= n;
                    const double theta = dTheta * static_cast<double>(m);
                    pass.values[(k * width) + (2 * (j - 1))] = std::cos(theta);
                    pass.values[(k * width) + (2 * (j - 1)) + 1] = std::sin(theta);
                }
            }
            m_Passes.push_back(std::move(pass));
        }
    }

    /// FFT.transform_internal with offset 0 and stride 2.
    void forward(std::span<double> data)
    {
        if (m_N == 1)
        {
            return; // the transform of one point is the identity
        }
        std::size_t product = 1;
        bool inScratch = false;
        for (std::size_t index = 0; index < m_Passes.size(); ++index)
        {
            const std::size_t factor = m_Passes[index].factor;
            product *= factor;
            const std::span<double> in = inScratch ? std::span<double>(m_Scratch) : data;
            const std::span<double> out = inScratch ? data : std::span<double>(m_Scratch);
            inScratch = !inScratch;
            switch (factor)
            {
            case 2:
                pass2(index, in, out, FORWARD, product);
                break;
            case 3:
                pass3(index, in, out, FORWARD, product);
                break;
            case 4:
                pass4(index, in, out, FORWARD, product);
                break;
            case 5:
                pass5(index, in, out, FORWARD, product);
                break;
            case 6:
                pass6(index, in, out, FORWARD, product);
                break;
            case 7:
                pass7(index, in, out, FORWARD, product);
                break;
            default:
                passN(index, in, out, FORWARD, factor, product);
            }
        }
        if (inScratch)
        {
            for (std::size_t i = 0; i < 2 * m_N; ++i)
            {
                data[i] = m_Scratch[i];
            }
        }
    }

  private:
    void pass2(std::size_t fi, std::span<const double> in, std::span<double> out, int sign, std::size_t product) const
    {
        const std::size_t factor = 2;
        const std::size_t m = m_N / factor;
        const std::size_t q = m_N / product;
        const std::size_t previousProduct = product / factor;
        const std::size_t di = STRIDE * m;
        const std::size_t dj = STRIDE * previousProduct;
        std::size_t i = 0;
        std::size_t j = 0;
        for (std::size_t k = 0; k < q; ++k)
        {
            const std::span<const double> twids = m_Passes[fi].row(k);
            const double wReal = twids[0];
            const double wImag = static_cast<double>(-sign) * twids[1];
            for (std::size_t k1 = 0; k1 < previousProduct; ++k1)
            {
                const double z0Real = in[i];
                const double z0Imag = in[i + 1];
                const double z1Real = in[i + di];
                const double z1Imag = in[i + di + 1];
                i += STRIDE;
                out[j] = z0Real + z1Real;
                out[j + 1] = z0Imag + z1Imag;
                const double xReal = z0Real - z1Real;
                const double xImag = z0Imag - z1Imag;
                out[j + dj] = wReal * xReal - wImag * xImag;
                out[j + dj + 1] = wReal * xImag + wImag * xReal;
                j += STRIDE;
            }
            j += (factor - 1) * dj;
        }
    }

    void pass3(std::size_t fi, std::span<const double> in, std::span<double> out, int sign, std::size_t product) const
    {
        const std::size_t factor = 3;
        const std::size_t m = m_N / factor;
        const std::size_t q = m_N / product;
        const std::size_t previousProduct = product / factor;
        const double tau = static_cast<double>(sign) * std::numbers::sqrt3 / 2.0; // Math.sqrt(3.0) is correctly rounded too
        const std::size_t di = STRIDE * m;
        const std::size_t dj = STRIDE * previousProduct;
        std::size_t i = 0;
        std::size_t j = 0;
        for (std::size_t k = 0; k < q; ++k)
        {
            const std::span<const double> twids = m_Passes[fi].row(k);
            const double w1Real = twids[0];
            const double w1Imag = static_cast<double>(-sign) * twids[1];
            const double w2Real = twids[2];
            const double w2Imag = static_cast<double>(-sign) * twids[3];
            for (std::size_t k1 = 0; k1 < previousProduct; ++k1)
            {
                const double z0Real = in[i];
                const double z0Imag = in[i + 1];
                const double z1Real = in[i + di];
                const double z1Imag = in[i + di + 1];
                const double z2Real = in[i + (2 * di)];
                const double z2Imag = in[i + (2 * di) + 1];
                i += STRIDE;
                const double t1Real = z1Real + z2Real;
                const double t1Imag = z1Imag + z2Imag;
                const double t2Real = z0Real - t1Real / 2.0;
                const double t2Imag = z0Imag - t1Imag / 2.0;
                const double t3Real = tau * (z1Real - z2Real);
                const double t3Imag = tau * (z1Imag - z2Imag);
                out[j] = z0Real + t1Real;
                out[j + 1] = z0Imag + t1Imag;
                double xReal = t2Real - t3Imag;
                double xImag = t2Imag + t3Real;
                out[j + dj] = w1Real * xReal - w1Imag * xImag;
                out[j + dj + 1] = w1Real * xImag + w1Imag * xReal;
                xReal = t2Real + t3Imag;
                xImag = t2Imag - t3Real;
                out[j + (2 * dj)] = w2Real * xReal - w2Imag * xImag;
                out[j + (2 * dj) + 1] = w2Real * xImag + w2Imag * xReal;
                j += STRIDE;
            }
            j += (factor - 1) * dj;
        }
    }

    void pass4(std::size_t fi, std::span<const double> in, std::span<double> out, int sign, std::size_t product) const
    {
        const std::size_t factor = 4;
        const std::size_t m = m_N / factor;
        const std::size_t q = m_N / product;
        const std::size_t previousProduct = product / factor;
        const std::size_t di = STRIDE * m;
        const std::size_t dj = STRIDE * previousProduct;
        const auto signedValue = static_cast<double>(sign);
        std::size_t i = 0;
        std::size_t j = 0;
        for (std::size_t k = 0; k < q; ++k)
        {
            const std::span<const double> twids = m_Passes[fi].row(k);
            const double w1Real = twids[0];
            const double w1Imag = static_cast<double>(-sign) * twids[1];
            const double w2Real = twids[2];
            const double w2Imag = static_cast<double>(-sign) * twids[3];
            const double w3Real = twids[4];
            const double w3Imag = static_cast<double>(-sign) * twids[5];
            for (std::size_t k1 = 0; k1 < previousProduct; ++k1)
            {
                const double z0Real = in[i];
                const double z0Imag = in[i + 1];
                const double z1Real = in[i + di];
                const double z1Imag = in[i + di + 1];
                const double z2Real = in[i + (2 * di)];
                const double z2Imag = in[i + (2 * di) + 1];
                const double z3Real = in[i + (3 * di)];
                const double z3Imag = in[i + (3 * di) + 1];
                i += STRIDE;
                const double t1Real = z0Real + z2Real;
                const double t1Imag = z0Imag + z2Imag;
                const double t2Real = z1Real + z3Real;
                const double t2Imag = z1Imag + z3Imag;
                const double t3Real = z0Real - z2Real;
                const double t3Imag = z0Imag - z2Imag;
                const double t4Real = signedValue * (z1Real - z3Real);
                const double t4Imag = signedValue * (z1Imag - z3Imag);
                out[j] = t1Real + t2Real;
                out[j + 1] = t1Imag + t2Imag;
                double xReal = t3Real - t4Imag;
                double xImag = t3Imag + t4Real;
                out[j + dj] = w1Real * xReal - w1Imag * xImag;
                out[j + dj + 1] = w1Real * xImag + w1Imag * xReal;
                xReal = t1Real - t2Real;
                xImag = t1Imag - t2Imag;
                out[j + (2 * dj)] = w2Real * xReal - w2Imag * xImag;
                out[j + (2 * dj) + 1] = w2Real * xImag + w2Imag * xReal;
                xReal = t3Real + t4Imag;
                xImag = t3Imag - t4Real;
                out[j + (3 * dj)] = w3Real * xReal - w3Imag * xImag;
                out[j + (3 * dj) + 1] = w3Real * xImag + w3Imag * xReal;
                j += STRIDE;
            }
            j += (factor - 1) * dj;
        }
    }

    void pass5(std::size_t fi, std::span<const double> in, std::span<double> out, int sign, std::size_t product) const
    {
        const std::size_t factor = 5;
        const std::size_t m = m_N / factor;
        const std::size_t q = m_N / product;
        const std::size_t previousProduct = product / factor;
        const double tau = std::sqrt(5.0) / 4.0;
        const double sin2PiBy5 = static_cast<double>(sign) * std::sin(2.0 * PI / 5.0);
        const double sin2PiBy10 = static_cast<double>(sign) * std::sin(2.0 * PI / 10.0);
        const std::size_t di = STRIDE * m;
        const std::size_t dj = STRIDE * previousProduct;
        std::size_t i = 0;
        std::size_t j = 0;
        for (std::size_t k = 0; k < q; ++k)
        {
            const std::span<const double> twids = m_Passes[fi].row(k);
            const double w1Real = twids[0];
            const double w1Imag = static_cast<double>(-sign) * twids[1];
            const double w2Real = twids[2];
            const double w2Imag = static_cast<double>(-sign) * twids[3];
            const double w3Real = twids[4];
            const double w3Imag = static_cast<double>(-sign) * twids[5];
            const double w4Real = twids[6];
            const double w4Imag = static_cast<double>(-sign) * twids[7];
            for (std::size_t k1 = 0; k1 < previousProduct; ++k1)
            {
                const double z0Real = in[i];
                const double z0Imag = in[i + 1];
                const double z1Real = in[i + di];
                const double z1Imag = in[i + di + 1];
                const double z2Real = in[i + (2 * di)];
                const double z2Imag = in[i + (2 * di) + 1];
                const double z3Real = in[i + (3 * di)];
                const double z3Imag = in[i + (3 * di) + 1];
                const double z4Real = in[i + (4 * di)];
                const double z4Imag = in[i + (4 * di) + 1];
                i += STRIDE;
                const double t1Real = z1Real + z4Real;
                const double t1Imag = z1Imag + z4Imag;
                const double t2Real = z2Real + z3Real;
                const double t2Imag = z2Imag + z3Imag;
                const double t3Real = z1Real - z4Real;
                const double t3Imag = z1Imag - z4Imag;
                const double t4Real = z2Real - z3Real;
                const double t4Imag = z2Imag - z3Imag;
                const double t5Real = t1Real + t2Real;
                const double t5Imag = t1Imag + t2Imag;
                const double t6Real = tau * (t1Real - t2Real);
                const double t6Imag = tau * (t1Imag - t2Imag);
                const double t7Real = z0Real - t5Real / 4.0;
                const double t7Imag = z0Imag - t5Imag / 4.0;
                const double t8Real = t7Real + t6Real;
                const double t8Imag = t7Imag + t6Imag;
                const double t9Real = t7Real - t6Real;
                const double t9Imag = t7Imag - t6Imag;
                const double t10Real = sin2PiBy5 * t3Real + sin2PiBy10 * t4Real;
                const double t10Imag = sin2PiBy5 * t3Imag + sin2PiBy10 * t4Imag;
                const double t11Real = sin2PiBy10 * t3Real - sin2PiBy5 * t4Real;
                const double t11Imag = sin2PiBy10 * t3Imag - sin2PiBy5 * t4Imag;
                out[j] = z0Real + t5Real;
                out[j + 1] = z0Imag + t5Imag;
                double xReal = t8Real - t10Imag;
                double xImag = t8Imag + t10Real;
                out[j + dj] = w1Real * xReal - w1Imag * xImag;
                out[j + dj + 1] = w1Real * xImag + w1Imag * xReal;
                xReal = t9Real - t11Imag;
                xImag = t9Imag + t11Real;
                out[j + (2 * dj)] = w2Real * xReal - w2Imag * xImag;
                out[j + (2 * dj) + 1] = w2Real * xImag + w2Imag * xReal;
                xReal = t9Real + t11Imag;
                xImag = t9Imag - t11Real;
                out[j + (3 * dj)] = w3Real * xReal - w3Imag * xImag;
                out[j + (3 * dj) + 1] = w3Real * xImag + w3Imag * xReal;
                xReal = t8Real + t10Imag;
                xImag = t8Imag - t10Real;
                out[j + (4 * dj)] = w4Real * xReal - w4Imag * xImag;
                out[j + (4 * dj) + 1] = w4Real * xImag + w4Imag * xReal;
                j += STRIDE;
            }
            j += (factor - 1) * dj;
        }
    }

    void pass6(std::size_t fi, std::span<const double> in, std::span<double> out, int sign, std::size_t product) const
    {
        const std::size_t factor = 6;
        const std::size_t m = m_N / factor;
        const std::size_t q = m_N / product;
        const std::size_t previousProduct = product / factor;
        const double tau = static_cast<double>(sign) * std::numbers::sqrt3 / 2.0; // Math.sqrt(3.0) is correctly rounded too
        const std::size_t di = STRIDE * m;
        const std::size_t dj = STRIDE * previousProduct;
        std::size_t i = 0;
        std::size_t j = 0;
        for (std::size_t k = 0; k < q; ++k)
        {
            const std::span<const double> twids = m_Passes[fi].row(k);
            std::array<double, 6> wReal{};
            std::array<double, 6> wImag{};
            for (std::size_t w = 1; w < factor; ++w)
            {
                wReal[w] = twids[2 * (w - 1)];
                wImag[w] = static_cast<double>(-sign) * twids[(2 * (w - 1)) + 1];
            }
            for (std::size_t k1 = 0; k1 < previousProduct; ++k1)
            {
                const double z0Real = in[i];
                const double z0Imag = in[i + 1];
                const double z1Real = in[i + di];
                const double z1Imag = in[i + di + 1];
                const double z2Real = in[i + (2 * di)];
                const double z2Imag = in[i + (2 * di) + 1];
                const double z3Real = in[i + (3 * di)];
                const double z3Imag = in[i + (3 * di) + 1];
                const double z4Real = in[i + (4 * di)];
                const double z4Imag = in[i + (4 * di) + 1];
                const double z5Real = in[i + (5 * di)];
                const double z5Imag = in[i + (5 * di) + 1];
                i += STRIDE;
                // W(6) combines W(3) acting on the even and the odd elements of z.
                const double ta1Real = z2Real + z4Real;
                const double ta1Imag = z2Imag + z4Imag;
                const double ta2Real = z0Real - ta1Real / 2;
                const double ta2Imag = z0Imag - ta1Imag / 2;
                const double ta3Real = tau * (z2Real - z4Real);
                const double ta3Imag = tau * (z2Imag - z4Imag);
                const double a0Real = z0Real + ta1Real;
                const double a0Imag = z0Imag + ta1Imag;
                const double a1Real = ta2Real - ta3Imag;
                const double a1Imag = ta2Imag + ta3Real;
                const double a2Real = ta2Real + ta3Imag;
                const double a2Imag = ta2Imag - ta3Real;
                const double tb1Real = z5Real + z1Real;
                const double tb1Imag = z5Imag + z1Imag;
                const double tb2Real = z3Real - tb1Real / 2;
                const double tb2Imag = z3Imag - tb1Imag / 2;
                const double tb3Real = tau * (z5Real - z1Real);
                const double tb3Imag = tau * (z5Imag - z1Imag);
                const double b0Real = z3Real + tb1Real;
                const double b0Imag = z3Imag + tb1Imag;
                const double b1Real = tb2Real - tb3Imag;
                const double b1Imag = tb2Imag + tb3Real;
                const double b2Real = tb2Real + tb3Imag;
                const double b2Imag = tb2Imag - tb3Real;
                out[j] = a0Real + b0Real;
                out[j + 1] = a0Imag + b0Imag;
                const std::array<double, 6> xReal{0.0, a1Real - b1Real, a2Real + b2Real, a0Real - b0Real, a1Real + b1Real, a2Real - b2Real};
                const std::array<double, 6> xImag{0.0, a1Imag - b1Imag, a2Imag + b2Imag, a0Imag - b0Imag, a1Imag + b1Imag, a2Imag - b2Imag};
                for (std::size_t w = 1; w < factor; ++w)
                {
                    out[j + (w * dj)] = wReal[w] * xReal[w] - wImag[w] * xImag[w];
                    out[j + (w * dj) + 1] = wReal[w] * xImag[w] + wImag[w] * xReal[w];
                }
                j += STRIDE;
            }
            j += (factor - 1) * dj;
        }
    }

    void pass7(std::size_t fi, std::span<const double> in, std::span<double> out, int sign, std::size_t product) const
    {
        const std::size_t factor = 7;
        const std::size_t m = m_N / factor;
        const std::size_t q = m_N / product;
        const std::size_t previousProduct = product / factor;
        const double c1 = std::cos(1.0 * 2.0 * PI / 7.0);
        const double c2 = std::cos(2.0 * 2.0 * PI / 7.0);
        const double c3 = std::cos(3.0 * 2.0 * PI / 7.0);
        const double s1 = static_cast<double>(-sign) * std::sin(1.0 * 2.0 * PI / 7.0);
        const double s2 = static_cast<double>(-sign) * std::sin(2.0 * 2.0 * PI / 7.0);
        const double s3 = static_cast<double>(-sign) * std::sin(3.0 * 2.0 * PI / 7.0);
        const std::size_t di = STRIDE * m;
        const std::size_t dj = STRIDE * previousProduct;
        std::size_t i = 0;
        std::size_t j = 0;
        for (std::size_t k = 0; k < q; ++k)
        {
            const std::span<const double> twids = m_Passes[fi].row(k);
            std::array<double, 7> wReal{};
            std::array<double, 7> wImag{};
            for (std::size_t w = 1; w < factor; ++w)
            {
                wReal[w] = twids[2 * (w - 1)];
                wImag[w] = static_cast<double>(-sign) * twids[(2 * (w - 1)) + 1];
            }
            for (std::size_t k1 = 0; k1 < previousProduct; ++k1)
            {
                const double z0Real = in[i];
                const double z0Imag = in[i + 1];
                const double z1Real = in[i + di];
                const double z1Imag = in[i + di + 1];
                const double z2Real = in[i + (2 * di)];
                const double z2Imag = in[i + (2 * di) + 1];
                const double z3Real = in[i + (3 * di)];
                const double z3Imag = in[i + (3 * di) + 1];
                const double z4Real = in[i + (4 * di)];
                const double z4Imag = in[i + (4 * di) + 1];
                const double z5Real = in[i + (5 * di)];
                const double z5Imag = in[i + (5 * di) + 1];
                const double z6Real = in[i + (6 * di)];
                const double z6Imag = in[i + (6 * di) + 1];
                i += STRIDE;
                const double t0Real = z1Real + z6Real;
                const double t0Imag = z1Imag + z6Imag;
                const double t1Real = z1Real - z6Real;
                const double t1Imag = z1Imag - z6Imag;
                const double t2Real = z2Real + z5Real;
                const double t2Imag = z2Imag + z5Imag;
                const double t3Real = z2Real - z5Real;
                const double t3Imag = z2Imag - z5Imag;
                const double t4Real = z4Real + z3Real;
                const double t4Imag = z4Imag + z3Imag;
                const double t5Real = z4Real - z3Real;
                const double t5Imag = z4Imag - z3Imag;
                const double t6Real = t2Real + t0Real;
                const double t6Imag = t2Imag + t0Imag;
                const double t7Real = t5Real + t3Real;
                const double t7Imag = t5Imag + t3Imag;
                const double b0Real = z0Real + t6Real + t4Real;
                const double b0Imag = z0Imag + t6Imag + t4Imag;
                const double b1Real = (((c1 + c2 + c3) / 3.0 - 1.0) * (t6Real + t4Real));
                const double b1Imag = (((c1 + c2 + c3) / 3.0 - 1.0) * (t6Imag + t4Imag));
                const double b2Real = (((2.0 * c1 - c2 - c3) / 3.0) * (t0Real - t4Real));
                const double b2Imag = (((2.0 * c1 - c2 - c3) / 3.0) * (t0Imag - t4Imag));
                const double b3Real = (((c1 - 2.0 * c2 + c3) / 3.0) * (t4Real - t2Real));
                const double b3Imag = (((c1 - 2.0 * c2 + c3) / 3.0) * (t4Imag - t2Imag));
                const double b4Real = (((c1 + c2 - 2.0 * c3) / 3.0) * (t2Real - t0Real));
                const double b4Imag = (((c1 + c2 - 2.0 * c3) / 3.0) * (t2Imag - t0Imag));
                const double b5Real = ((s1 + s2 - s3) / 3.0) * (t7Real + t1Real);
                const double b5Imag = ((s1 + s2 - s3) / 3.0) * (t7Imag + t1Imag);
                const double b6Real = ((2.0 * s1 - s2 + s3) / 3.0) * (t1Real - t5Real);
                const double b6Imag = ((2.0 * s1 - s2 + s3) / 3.0) * (t1Imag - t5Imag);
                const double b7Real = ((s1 - 2.0 * s2 - s3) / 3.0) * (t5Real - t3Real);
                const double b7Imag = ((s1 - 2.0 * s2 - s3) / 3.0) * (t5Imag - t3Imag);
                const double b8Real = ((s1 + s2 + 2.0 * s3) / 3.0) * (t3Real - t1Real);
                const double b8Imag = ((s1 + s2 + 2.0 * s3) / 3.0) * (t3Imag - t1Imag);
                const double bigT0Real = b0Real + b1Real;
                const double bigT0Imag = b0Imag + b1Imag;
                const double bigT1Real = b2Real + b3Real;
                const double bigT1Imag = b2Imag + b3Imag;
                const double bigT2Real = b4Real - b3Real;
                const double bigT2Imag = b4Imag - b3Imag;
                const double bigT3Real = -b2Real - b4Real;
                const double bigT3Imag = -b2Imag - b4Imag;
                const double bigT4Real = b6Real + b7Real;
                const double bigT4Imag = b6Imag + b7Imag;
                const double bigT5Real = b8Real - b7Real;
                const double bigT5Imag = b8Imag - b7Imag;
                const double bigT6Real = -b8Real - b6Real;
                const double bigT6Imag = -b8Imag - b6Imag;
                const double bigT7Real = bigT0Real + bigT1Real;
                const double bigT7Imag = bigT0Imag + bigT1Imag;
                const double bigT8Real = bigT0Real + bigT2Real;
                const double bigT8Imag = bigT0Imag + bigT2Imag;
                const double bigT9Real = bigT0Real + bigT3Real;
                const double bigT9Imag = bigT0Imag + bigT3Imag;
                const double bigT10Real = bigT4Real + b5Real;
                const double bigT10Imag = bigT4Imag + b5Imag;
                const double bigT11Real = bigT5Real + b5Real;
                const double bigT11Imag = bigT5Imag + b5Imag;
                const double bigT12Real = bigT6Real + b5Real;
                const double bigT12Imag = bigT6Imag + b5Imag;
                out[j] = b0Real;
                out[j + 1] = b0Imag;
                const std::array<double, 7> xReal{0.0,
                                                  bigT7Real + bigT10Imag,
                                                  bigT9Real + bigT12Imag,
                                                  bigT8Real - bigT11Imag,
                                                  bigT8Real + bigT11Imag,
                                                  bigT9Real - bigT12Imag,
                                                  bigT7Real - bigT10Imag};
                const std::array<double, 7> xImag{0.0,
                                                  bigT7Imag - bigT10Real,
                                                  bigT9Imag - bigT12Real,
                                                  bigT8Imag + bigT11Real,
                                                  bigT8Imag - bigT11Real,
                                                  bigT9Imag + bigT12Real,
                                                  bigT7Imag + bigT10Real};
                for (std::size_t w = 1; w < factor; ++w)
                {
                    out[j + (w * dj)] = wReal[w] * xReal[w] - wImag[w] * xImag[w];
                    out[j + (w * dj) + 1] = wReal[w] * xImag[w] + wImag[w] * xReal[w];
                }
                j += STRIDE;
            }
            j += (factor - 1) * dj;
        }
    }

    /// FFT.pass_n for factors other than 2..7. It uses `in` as workspace, like the Java code.
    void passN(std::size_t fi, std::span<double> in, std::span<double> out, int sign, std::size_t factor, std::size_t product) const
    {
        const std::size_t m = m_N / factor;
        const std::size_t q = m_N / product;
        const std::size_t previousProduct = product / factor;
        const std::size_t jump = (factor - 1) * previousProduct;
        const std::size_t half = ((factor - 1) / 2) + 1;
        for (std::size_t i = 0; i < m; ++i)
        {
            out[STRIDE * i] = in[STRIDE * i];
            out[(STRIDE * i) + 1] = in[(STRIDE * i) + 1];
        }
        for (std::size_t e = 1; e < half; ++e)
        {
            for (std::size_t i = 0; i < m; ++i)
            {
                const std::size_t idx = i + (e * m);
                const std::size_t idxc = i + ((factor - e) * m);
                out[STRIDE * idx] = in[STRIDE * idx] + in[STRIDE * idxc];
                out[(STRIDE * idx) + 1] = in[(STRIDE * idx) + 1] + in[(STRIDE * idxc) + 1];
                out[STRIDE * idxc] = in[STRIDE * idx] - in[STRIDE * idxc];
                out[(STRIDE * idxc) + 1] = in[(STRIDE * idx) + 1] - in[(STRIDE * idxc) + 1];
            }
        }
        // e = 0
        for (std::size_t i = 0; i < m; ++i)
        {
            in[STRIDE * i] = out[STRIDE * i];
            in[(STRIDE * i) + 1] = out[(STRIDE * i) + 1];
        }
        for (std::size_t e1 = 1; e1 < half; ++e1)
        {
            for (std::size_t i = 0; i < m; ++i)
            {
                in[STRIDE * i] += out[STRIDE * (i + (e1 * m))];
                in[(STRIDE * i) + 1] += out[(STRIDE * (i + (e1 * m))) + 1];
            }
        }
        // OSP reads row q of the twiddle table here (GSL's twiddle[q]).
        std::span<const double> twiddles = m_Passes[fi].row(q);
        for (std::size_t e = 1; e < half; ++e)
        {
            std::size_t idx = e;
            const std::size_t em = e * m;
            const std::size_t ecm = (factor - e) * m;
            for (std::size_t i = 0; i < m; ++i)
            {
                in[STRIDE * (i + em)] = out[STRIDE * i];
                in[(STRIDE * (i + em)) + 1] = out[(STRIDE * i) + 1];
                in[STRIDE * (i + ecm)] = out[STRIDE * i];
                in[(STRIDE * (i + ecm)) + 1] = out[(STRIDE * i) + 1];
            }
            for (std::size_t e1 = 1; e1 < half; ++e1)
            {
                double wReal = 1;
                double wImag = 0;
                if (idx != 0)
                {
                    wReal = twiddles[2 * (idx - 1)];
                    wImag = static_cast<double>(-sign) * twiddles[(2 * (idx - 1)) + 1];
                }
                for (std::size_t i = 0; i < m; ++i)
                {
                    const double ap = wReal * out[STRIDE * (i + (e1 * m))];
                    const double am = wImag * out[(STRIDE * (i + ((factor - e1) * m))) + 1];
                    const double bp = wReal * out[(STRIDE * (i + (e1 * m))) + 1];
                    const double bm = wImag * out[STRIDE * (i + ((factor - e1) * m))];
                    in[STRIDE * (i + em)] += (ap - am);
                    in[(STRIDE * (i + em)) + 1] += (bp + bm);
                    in[STRIDE * (i + ecm)] += (ap + am);
                    in[(STRIDE * (i + ecm)) + 1] += (bp - bm);
                }
                idx += e;
                idx %= factor;
            }
        }
        // k = 0
        for (std::size_t k1 = 0; k1 < previousProduct; ++k1)
        {
            out[STRIDE * k1] = in[STRIDE * k1];
            out[(STRIDE * k1) + 1] = in[(STRIDE * k1) + 1];
        }
        for (std::size_t e1 = 1; e1 < factor; ++e1)
        {
            for (std::size_t k1 = 0; k1 < previousProduct; ++k1)
            {
                out[STRIDE * (k1 + (e1 * previousProduct))] = in[STRIDE * (k1 + (e1 * m))];
                out[(STRIDE * (k1 + (e1 * previousProduct))) + 1] = in[(STRIDE * (k1 + (e1 * m))) + 1];
            }
        }
        std::size_t i = previousProduct;
        std::size_t j = product;
        for (std::size_t k = 1; k < q; ++k)
        {
            for (std::size_t k1 = 0; k1 < previousProduct; ++k1)
            {
                out[STRIDE * j] = in[STRIDE * i];
                out[(STRIDE * j) + 1] = in[(STRIDE * i) + 1];
                ++i;
                ++j;
            }
            j += jump;
        }
        i = previousProduct;
        j = product;
        for (std::size_t k = 1; k < q; ++k)
        {
            twiddles = m_Passes[fi].row(k);
            for (std::size_t k1 = 0; k1 < previousProduct; ++k1)
            {
                for (std::size_t e1 = 1; e1 < factor; ++e1)
                {
                    const double xReal = in[STRIDE * (i + (e1 * m))];
                    const double xImag = in[(STRIDE * (i + (e1 * m))) + 1];
                    const double wReal = twiddles[2 * (e1 - 1)];
                    const double wImag = static_cast<double>(-sign) * twiddles[(2 * (e1 - 1)) + 1];
                    out[STRIDE * (j + (e1 * previousProduct))] = wReal * xReal - wImag * xImag;
                    out[(STRIDE * (j + (e1 * previousProduct))) + 1] = wReal * xImag + wImag * xReal;
                }
                ++i;
                ++j;
            }
            j += jump;
        }
    }

    std::size_t m_N;
    std::vector<double> m_Scratch;
    std::vector<PassTwiddles> m_Passes;
};

} // namespace

bool complexFft(std::span<double> interleaved)
{
    if (interleaved.empty() || interleaved.size() % 2 != 0)
    {
        return false;
    }
    ComplexTransform(interleaved.size() / 2).forward(interleaved);
    return true;
}

bool realFft(std::span<double> data)
{
    const std::size_t n = data.size();
    if (n == 0 || n % 2 != 0)
    {
        return false;
    }
    // FFTReal.transform: a complex transform of the n/2 even/odd pairs, then FFTReal.shuffle(+1).
    ComplexTransform(n / 2).forward(data);
    const std::size_t nh = n / 2;
    // OSP uses n / 4 (2 for n = 6), which skips the middle pair whenever n/2 is odd and n > 6.
    const std::size_t pairs = (nh + 1) / 2;
    const int sign = 1;
    const double c1 = 0.5;
    const double c2 = -0.5 * sign;
    const double theta = sign * PI / static_cast<double>(nh);
    double wtemp = std::sin(0.5 * theta);
    const double wpr = -2.0 * wtemp * wtemp;
    const double wpi = -std::sin(theta);
    double wr = 1.0 + wpr;
    double wi = wpi;
    for (std::size_t i = 1; i < pairs; ++i)
    {
        const std::size_t i1 = 2 * i;
        const std::size_t i3 = n - i1;
        const double h1r = c1 * (data[i1] + data[i3]);
        const double h1i = c1 * (data[i1 + 1] - data[i3 + 1]);
        const double h2r = -c2 * (data[i1 + 1] + data[i3 + 1]);
        const double h2i = c2 * (data[i1] - data[i3]);
        data[i1] = h1r + wr * h2r - wi * h2i;
        data[i1 + 1] = h1i + wr * h2i + wi * h2r;
        data[i3] = h1r - wr * h2r + wi * h2i;
        data[i3 + 1] = -h1i + wr * h2i + wi * h2r;
        wtemp = wr;
        wr += wtemp * wpr - wi * wpi;
        wi += wtemp * wpi + wi * wpr;
    }
    const double d0 = data[0];
    data[0] = d0 + data[1];
    data[1] = d0 - data[1];
    if (n % 4 == 0)
    {
        data[nh + 1] *= -1;
    }
    return true;
}

} // namespace motrack::science
