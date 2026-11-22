#include "science/fit/minimizers.hpp"

#include "science/fit/fit_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <utility>
#include <vector>

// Every expression below keeps Java's evaluation order and rounding (no contraction; see CMakeLists.txt).
// Objective calls are sequenced explicitly because C++ leaves operand evaluation order unspecified and
// DatasetCurveFitter observes which parameter vector the objective saw last.

namespace motrack::science
{
namespace fit_detail
{

LupDecomposition::LupDecomposition(std::span<const double> rowMajor, std::size_t size)
    : m_Rows(rowMajor.begin(), rowMajor.end()), m_Permutation(size), m_Size(size)
{}

double& LupDecomposition::at(std::size_t row, std::size_t column)
{
    return m_Rows[(row * m_Size) + column];
}

void LupDecomposition::decompose()
{
    m_Decomposed = true;
    for (std::size_t i = 0; i < m_Size; ++i)
    {
        m_Permutation[i] = i;
    }
    for (std::size_t k = 0; k < m_Size; ++k)
    {
        // largestPivot: the first strictly larger magnitude wins; NaN never does.
        double maximum = std::abs(at(k, k));
        std::size_t index = k;
        for (std::size_t i = k + 1; i < m_Size; ++i)
        {
            const double magnitude = std::abs(at(i, k));
            if (magnitude > maximum)
            {
                maximum = magnitude;
                index = i;
            }
        }
        if (index != k)
        {
            for (std::size_t j = 0; j < m_Size; ++j)
            {
                std::swap(at(k, j), at(index, j));
            }
            std::swap(m_Permutation[k], m_Permutation[index]);
        }
        const double inversePivot = 1 / at(k, k);
        for (std::size_t i = k + 1; i < m_Size; ++i)
        {
            at(i, k) *= inversePivot;
            for (std::size_t j = k + 1; j < m_Size; ++j)
            {
                at(i, j) -= at(i, k) * at(k, j);
            }
        }
    }
}

std::vector<double> LupDecomposition::solve(std::span<const double> constants)
{
    if (!m_Decomposed)
    {
        decompose();
    }
    std::vector<double> forward(m_Size);
    for (std::size_t i = 0; i < m_Size; ++i)
    {
        forward[i] = constants[m_Permutation[i]];
        for (std::size_t j = 0; j < i; ++j)
        {
            forward[i] -= at(i, j) * forward[j];
        }
    }
    std::vector<double> answer(m_Size);
    for (std::size_t i = m_Size; i-- > 0;)
    {
        answer[i] = forward[i];
        for (std::size_t j = i + 1; j < m_Size; ++j)
        {
            answer[i] -= at(i, j) * answer[j];
        }
        answer[i] /= at(i, i);
    }
    return answer;
}

std::optional<std::vector<double>> LupDecomposition::inverse()
{
    std::vector<double> result(m_Size * m_Size);
    std::vector<double> column(m_Size);
    for (std::size_t i = 0; i < m_Size; ++i)
    {
        std::ranges::fill(column, 0.0);
        column[i] = 1;
        column = solve(column);
        for (std::size_t j = 0; j < m_Size; ++j)
        {
            if (std::isnan(column[j]))
            {
                return std::nullopt;
            }
            result[(j * m_Size) + i] = column[j];
        }
    }
    return result;
}

bool solvePolynomialNormalEquations(std::span<const double> x, std::span<const double> y, std::vector<double>& coefficients)
{
    const std::size_t count = coefficients.size();
    if (x.size() < count)
    {
        return false;
    }
    std::vector<double> matrix(count * count, 0.0);
    std::vector<double> constants(count, 0.0);
    for (std::size_t i = 0; i < x.size(); ++i)
    {
        double xp1 = 1;
        for (std::size_t j = 0; j < count; ++j)
        {
            constants[j] += xp1 * y[i];
            double xp2 = xp1;
            for (std::size_t k = 0; k <= j; ++k)
            {
                matrix[(j * count) + k] += xp2;
                xp2 *= x[i];
            }
            xp1 *= x[i];
        }
    }
    for (std::size_t i = 0; i < count; ++i)
    {
        for (std::size_t j = i + 1; j < count; ++j)
        {
            matrix[(i * count) + j] = matrix[(j * count) + i];
        }
    }
    LupDecomposition system(matrix, count);
    if (!system.inverse())
    {
        return false;
    }
    coefficients = system.solve(constants);
    return true;
}

double evaluatePolynomial(std::span<const double> coefficients, double x) noexcept
{
    std::size_t n = coefficients.size();
    double answer = coefficients[--n];
    while (n > 0)
    {
        answer = (answer * x) + coefficients[--n];
    }
    return answer;
}

} // namespace fit_detail

namespace
{

using fit_detail::LupDecomposition;

/// HessianMinimize.getHessian: central differences of the objective, in OSP's evaluation order.
/// `hessian` is row-major m x m; `gradient` receives the negated first derivatives (OSP's D).
void finiteDifferenceHessian(const MultiVarObjective& objective,
                             std::span<const double> x,
                             std::span<double> gradient,
                             std::span<const double> dx,
                             std::vector<double>& hessian)
{
    const std::size_t m = x.size();
    hessian.assign(m * m, 0.0);
    std::vector<double> xp(x.begin(), x.end());
    std::vector<double> xm(x.begin(), x.end());
    std::vector<double> xpp(m);
    std::vector<double> xpm(m);
    std::vector<double> xmp(m);
    std::vector<double> xmm(m);
    for (std::size_t i = 0; i < m; ++i)
    {
        for (std::size_t j = i; j < m; ++j)
        {
            if (i == j)
            {
                std::ranges::copy(x, xp.begin());
                std::ranges::copy(x, xm.begin());
                xp[i] = x[i] + dx[i];
                xm[i] = x[i] - dx[i];
                const double plus = objective(xp);
                const double centre = objective(x);
                const double minus = objective(xm);
                hessian[(i * m) + i] = (plus - (2.0 * centre) + minus) / (dx[i] * dx[i]);
            }
            else
            {
                std::ranges::copy(x, xpp.begin());
                std::ranges::copy(x, xpm.begin());
                std::ranges::copy(x, xmp.begin());
                std::ranges::copy(x, xmm.begin());
                xpp[i] = x[i] + dx[i];
                xpp[j] = x[j] + dx[j];
                xpm[i] = x[i] + dx[i];
                xpm[j] = x[j] - dx[j];
                xmp[i] = x[i] - dx[i];
                xmp[j] = x[j] + dx[j];
                xmm[i] = x[i] - dx[i];
                xmm[j] = x[j] - dx[j];
                const double plusPlus = objective(xpp);
                const double plusMinus = objective(xpm);
                const double minusPlus = objective(xmp);
                const double minusMinus = objective(xmm);
                const double value =
                    (((plusPlus - plusMinus) / (2.0 * dx[j])) - ((minusPlus - minusMinus) / (2.0 * dx[j]))) / (2.0 * dx[i]);
                hessian[(i * m) + j] = value;
                hessian[(j * m) + i] = value;
            }
        }
    }
    for (std::size_t i = 0; i < m; ++i)
    {
        std::ranges::copy(x, xp.begin());
        std::ranges::copy(x, xm.begin());
        xp[i] = x[i] + dx[i];
        xm[i] = x[i] - dx[i];
        const double plus = objective(xp);
        const double minus = objective(xm);
        gradient[i] = -(plus - minus) / (2.0 * dx[i]);
    }
}

/// HessianMinimize.crudeGuess: five cycles of a damped (0.35) Newton step per parameter.
void crudeGuess(const MultiVarObjective& objective, std::span<double> x)
{
    constexpr int CYCLES = 5;
    constexpr double DAMPING = 0.35;
    const std::size_t m = x.size();
    std::vector<double> xp(m);
    std::vector<double> xm(m);
    std::vector<double> dx(m);
    for (std::size_t i = 0; i < m; ++i)
    {
        dx[i] = (std::abs(x[i]) + 1.0) / 1.e3;
    }
    for (int cycle = 0; cycle < CYCLES; ++cycle)
    {
        for (std::size_t i = 0; i < m; ++i)
        {
            for (std::size_t k = 0; k < m; ++k)
            {
                if (k == i)
                {
                    xp[i] = x[i] + dx[i];
                    xm[i] = x[i] - dx[i];
                }
                else
                {
                    xp[k] = x[k];
                    xm[k] = x[k];
                }
            }
            const double plus = objective(xp);
            const double centre = objective(x);
            const double minus = objective(xm);
            x[i] = x[i] - ((((DAMPING * 0.5) * dx[i]) * (plus - minus)) / ((plus - (2.0 * centre)) + minus));
            dx[i] = 0.5 * dx[i];
        }
    }
}

double sum(std::span<const double> values)
{
    double total = 0;
    for (const double value : values)
    {
        total += value;
    }
    return total;
}

/// check_rmsd: keeps the better of the remembered vector and `x`, never one whose sum is NaN. Returns
/// whether `x` was kept.
bool keepBetter(const MultiVarObjective& objective, std::vector<double>& remembered, double& rememberedValue, std::span<double> x)
{
    if (std::isnan(sum(x)))
    {
        std::ranges::copy(remembered, x.begin());
        return false;
    }
    const double value = objective(x);
    if (value <= rememberedValue)
    {
        rememberedValue = value;
        std::ranges::copy(x, remembered.begin());
        return true;
    }
    std::ranges::copy(remembered, x.begin());
    return false;
}

/// One Newton update shared by both minimizers: x becomes x + H^-1 D, returning OSP's squared step
/// and the squared norm of the previous x.
std::pair<double, double> newtonStep(std::vector<double>& hessian, std::span<const double> gradient, std::span<double> x)
{
    const std::size_t m = x.size();
    LupDecomposition lu(hessian, m);
    std::vector<double> next = lu.solve(gradient);
    for (std::size_t i = 0; i < m; ++i)
    {
        next[i] = next[i] + x[i];
    }
    double error = (x[0] - next[0]) * (x[0] - next[0]);
    double norm = x[0] * x[0];
    x[0] = next[0];
    for (std::size_t i = 1; i < m; ++i)
    {
        error = error + ((x[i] - next[i]) * (x[i] - next[i]));
        norm = norm + (x[i] * x[i]);
        x[i] = next[i];
    }
    return {error, norm};
}

constexpr double UNSET_ERROR = 9999.;

} // namespace

HessianMinimizeResult hessianMinimize(const MultiVarObjective& objective, std::span<double> x, int maxIterations, double tolerance)
{
    HessianMinimizeResult result;
    const std::size_t m = x.size();
    if (m == 0)
    {
        return result;
    }
    std::vector<double> gradient(m);
    std::vector<double> dx(m);
    std::vector<double> remembered(x.begin(), x.end());
    std::vector<double> hessian;
    double rememberedValue = objective(x);
    crudeGuess(objective, x);
    static_cast<void>(keepBetter(objective, remembered, rememberedValue, x));
    for (std::size_t i = 0; i < m; ++i)
    {
        dx[i] = (std::abs(x[i]) + 1.0) / 1e5;
    }
    double error = UNSET_ERROR;
    double relative = UNSET_ERROR;
    const double threshold = tolerance * 1.e-6;
    while (error > threshold && relative > threshold && result.iterations < maxIterations)
    {
        ++result.iterations;
        finiteDifferenceHessian(objective, x, gradient, dx, hessian);
        const auto [squared, norm] = newtonStep(hessian, gradient, x);
        error = std::sqrt(squared);
        relative = error / (norm + tolerance);
    }
    result.keptLastIterate = keepBetter(objective, remembered, rememberedValue, x);
    result.error = error;
    result.reachedTolerance = error <= threshold || relative <= threshold;
    return result;
}

LevenbergMarquardtResult
levenbergMarquardtMinimize(const MultiVarObjective& objective, std::span<double> x, int maxIterations, double tolerance)
{
    constexpr double LAMBDA_START = 0.001;
    constexpr double LAMBDA_FLOOR = 1e-6;
    LevenbergMarquardtResult result;
    const std::size_t m = x.size();
    if (m == 0)
    {
        return result;
    }
    std::vector<double> gradient(m);
    std::vector<double> dx(m);
    std::vector<double> hessian;
    double startValue = objective(x);
    double bestValue = startValue;
    std::vector<double> start(x.begin(), x.end());
    std::vector<double> best(x.begin(), x.end());
    for (std::size_t i = 0; i < m; ++i)
    {
        dx[i] = (std::abs(x[i]) + 1.0) / 1e5;
    }
    double lambda = LAMBDA_START;
    double error = UNSET_ERROR;
    double relative = UNSET_ERROR;
    const double threshold = tolerance * 1.e-6;
    while (error > threshold && relative > threshold && result.iterations < maxIterations && lambda > LAMBDA_FLOOR)
    {
        ++result.iterations;
        finiteDifferenceHessian(objective, x, gradient, dx, hessian);
        for (std::size_t i = 0; i < m; ++i)
        {
            hessian[(i * m) + i] = hessian[(i * m) + i] + lambda;
        }
        const auto [squared, norm] = newtonStep(hessian, gradient, x);
        const double value = objective(x);
        if (value < bestValue)
        {
            lambda = lambda / 10.;
            bestValue = value;
            std::ranges::copy(x, best.begin());
        }
        else
        {
            std::ranges::copy(best, x.begin());
            lambda = 10. * lambda;
        }
        error = std::sqrt(squared);
        relative = error / (norm + tolerance);
    }
    result.reachedTolerance = error <= threshold || relative <= threshold;
    if (error > threshold && relative > threshold)
    {
        return result;
    }
    static_cast<void>(keepBetter(objective, start, startValue, x));
    result.converged = true;
    return result;
}

} // namespace motrack::science
