#include "science/matching/matching_internal.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace motrack::science::matching_detail
{
namespace
{

constexpr int BYTE_VALUES = 256;
using AlphaTable = std::array<std::array<std::uint8_t, BYTE_VALUES>, BYTE_VALUES>;

struct AlphaTables
{
    AlphaTable mul8{};
    AlphaTable div8{};
};

// libawt AlphaMath.c initAlphaTables(): row and column 0 stay zero as in the static C arrays.
const AlphaTables& alphaTables()
{
    static const AlphaTables tables = []
    {
        AlphaTables result;
        for (std::uint32_t i = 1; i < BYTE_VALUES; ++i)
        {
            const std::uint32_t increment = (i << 16U) + (i << 8U) + i; // approx. (1 << 24) * i / 255
            std::uint32_t value = increment + (1U << 23U);
            for (std::uint32_t j = 1; j < BYTE_VALUES; ++j)
            {
                result.mul8[i][j] = static_cast<std::uint8_t>(value >> 24U);
                value += increment;
            }
        }
        for (std::uint32_t i = 1; i < BYTE_VALUES; ++i)
        {
            const std::uint32_t increment = ((0xffU << 24U) + i / 2) / i;
            std::uint32_t value = 1U << 23U;
            for (std::uint32_t j = 0; j < i; ++j)
            {
                result.div8[i][j] = static_cast<std::uint8_t>(value >> 24U);
                value += increment;
            }
            for (std::uint32_t j = i; j < BYTE_VALUES; ++j)
            {
                result.div8[i][j] = 0xff;
            }
        }
        return result;
    }();
    return tables;
}

constexpr int channel(ArgbPixel pixel, unsigned shift) noexcept
{
    return static_cast<int>((pixel >> shift) & 0xffU);
}

constexpr std::size_t PARABOLA_TERMS = 3;
using Matrix = std::array<std::array<double, PARABOLA_TERMS>, PARABOLA_TERMS>;
constexpr std::array<double, PARABOLA_TERMS> PARABOLA_X = {-1.0, 0.0, 1.0};

struct LupSystem
{
    Matrix rows{};
    std::array<std::size_t, PARABOLA_TERMS> permutation{0, 1, 2};
};

// PolynomialLeastSquareFit's normal matrix for x = {-1, 0, 1} (independent of the fitted values),
// symmetrized by computeCoefficients() and decomposed by OSP's LUPDecomposition. Its inverse has no
// NaN, so computeCoefficients() always proceeds to solve(). The matrix {{3, 0, 2}, {0, 2, 0},
// {2, 0, 2}} never needs a row swap; the general pivot search is kept as a literal port.
const LupSystem& parabolaSystem()
{
    static const LupSystem system = []
    {
        LupSystem result;
        auto& rows = result.rows;
        for (const double x : PARABOLA_X)
        {
            double xp1 = 1;
            for (std::size_t j = 0; j < PARABOLA_TERMS; ++j)
            {
                double xp2 = xp1;
                for (std::size_t k = 0; k <= j; ++k)
                {
                    rows[j][k] += xp2;
                    xp2 *= x;
                }
                xp1 *= x;
            }
        }
        for (std::size_t i = 0; i < PARABOLA_TERMS; ++i)
        {
            for (std::size_t j = i + 1; j < PARABOLA_TERMS; ++j)
            {
                rows[i][j] = rows[j][i];
            }
        }
        for (std::size_t k = 0; k < PARABOLA_TERMS; ++k)
        {
            // largestPivot(k) and swapRows(k, pivot).
            double maximum = std::abs(rows[k][k]);
            std::size_t pivot = k;
            for (std::size_t i = k + 1; i < PARABOLA_TERMS; ++i)
            {
                if (const double magnitude = std::abs(rows[i][k]); magnitude > maximum)
                {
                    maximum = magnitude;
                    pivot = i;
                }
            }
            if (pivot != k)
            {
                std::swap(rows[k], rows[pivot]);
                std::swap(result.permutation[k], result.permutation[pivot]);
            }
            // pivot(k).
            const double inversePivot = 1 / rows[k][k];
            for (std::size_t i = k + 1; i < PARABOLA_TERMS; ++i)
            {
                rows[i][k] *= inversePivot;
                for (std::size_t j = k + 1; j < PARABOLA_TERMS; ++j)
                {
                    rows[i][j] -= rows[i][k] * rows[k][j];
                }
            }
        }
        return result;
    }();
    return system;
}

} // namespace

int mul8(int a, int b) noexcept
{
    return alphaTables().mul8.at(static_cast<std::size_t>(a)).at(static_cast<std::size_t>(b));
}

int div8(int a, int b) noexcept
{
    return alphaTables().div8.at(static_cast<std::size_t>(b)).at(static_cast<std::size_t>(a));
}

int compositeExtraAlpha(int alpha) noexcept
{
    // TemplateMatcher.getComposite: float a = 1.0f * alpha / 255; libawt: (jint) (a * 255.0 + 0.5).
    const float extra = 1.0F * static_cast<float>(alpha) / 255.0F;
    // C truncation of a non-negative value plus one half, i.e. floor.
    return static_cast<int>(std::floor(static_cast<double>(extra) * 255.0 + 0.5));
}

ArgbPixel srcOver(ArgbPixel source, ArgbPixel destination, int extraAlpha) noexcept
{
    // DEFINE_SRCOVER_MASKBLIT(IntArgb, IntArgb, 4ByteArgb), pMask == NULL branch. Every table index is a
    // byte: channels are masked, extraAlpha is clamped and sums of products stay within resA <= 0xff.
    const auto& tables = alphaTables();
    const auto multiply = [&](int a, int b)
    {
        return static_cast<int>(tables.mul8[static_cast<std::size_t>(a)][static_cast<std::size_t>(b)]);
    };
    const auto divide = [&](int a, int b)
    {
        return static_cast<int>(tables.div8[static_cast<std::size_t>(b)][static_cast<std::size_t>(a)]);
    };
    int resultAlpha = multiply(std::clamp(extraAlpha, 0, 0xff), channel(source, 24));
    if (resultAlpha == 0)
    {
        return destination;
    }
    const int sourceFactor = resultAlpha; // IntArgb is not premultiplied
    int red = channel(source, 16);
    int green = channel(source, 8);
    int blue = channel(source, 0);
    if (resultAlpha < 0xff)
    {
        const int destinationAlpha = multiply(0xff - resultAlpha, channel(destination, 24));
        const int destinationFactor = destinationAlpha;
        resultAlpha += destinationAlpha;
        red = multiply(destinationFactor, channel(destination, 16)) + multiply(sourceFactor, red);
        green = multiply(destinationFactor, channel(destination, 8)) + multiply(sourceFactor, green);
        blue = multiply(destinationFactor, channel(destination, 0)) + multiply(sourceFactor, blue);
    }
    // The "srcF < 0xff" scaling cannot apply: srcF equals resA, which is 0xff in that branch.
    if (resultAlpha < 0xff)
    {
        red = divide(red, resultAlpha);
        green = divide(green, resultAlpha);
        blue = divide(blue, resultAlpha);
    }
    return (static_cast<ArgbPixel>(resultAlpha) << 24U) | (static_cast<ArgbPixel>(red) << 16U) | (static_cast<ArgbPixel>(green) << 8U) |
           static_cast<ArgbPixel>(blue);
}

Parabola fitParabola(double previous, double center, double next) noexcept
{
    // PolynomialLeastSquareFit.fitData: accumulate the constants in the same order as Java.
    const std::array<double, PARABOLA_TERMS> values = {previous, center, next};
    std::array<double, PARABOLA_TERMS> constants{};
    for (std::size_t i = 0; i < PARABOLA_TERMS; ++i)
    {
        double xp1 = 1;
        for (double& constant : constants)
        {
            constant += xp1 * values.at(i);
            xp1 *= PARABOLA_X.at(i);
        }
    }
    // LUPDecomposition.solve: forward then backward substitution.
    const auto& system = parabolaSystem();
    std::array<double, PARABOLA_TERMS> forward{};
    for (std::size_t i = 0; i < PARABOLA_TERMS; ++i)
    {
        forward.at(i) = constants.at(system.permutation.at(i));
        for (std::size_t j = 0; j < i; ++j)
        {
            forward.at(i) -= system.rows.at(i).at(j) * forward.at(j);
        }
    }
    std::array<double, PARABOLA_TERMS> answer{};
    for (std::size_t i = PARABOLA_TERMS; i-- > 0;)
    {
        answer.at(i) = forward.at(i);
        for (std::size_t j = i + 1; j < PARABOLA_TERMS; ++j)
        {
            answer.at(i) -= system.rows.at(i).at(j) * answer.at(j);
        }
        answer.at(i) /= system.rows.at(i).at(i);
    }
    return {.c0 = answer[0], .c1 = answer[1], .c2 = answer[2]};
}

} // namespace motrack::science::matching_detail
