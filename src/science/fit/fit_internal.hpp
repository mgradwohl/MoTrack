#pragma once

#include "science/fit/curve_fit.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace motrack::science::fit_detail
{

/// Ports `org.opensourcephysics.numerics.LUPDecomposition`: Doolittle LU with partial pivoting on a
/// copy of a square row-major matrix, decomposed on first use. Like Java's double arithmetic it never
/// fails: a zero pivot yields infinities and NaN in the solution.
class LupDecomposition
{
  public:
    LupDecomposition(std::span<const double> rowMajor, std::size_t size);

    /// `solve(c)`: forward then backward substitution.
    [[nodiscard]] std::vector<double> solve(std::span<const double> constants);

    /// `inverseMatrixComponents()`: nothing when any column of the inverse contains NaN.
    [[nodiscard]] std::optional<std::vector<double>> inverse();

  private:
    void decompose();
    [[nodiscard]] double& at(std::size_t row, std::size_t column);

    std::vector<double> m_Rows;
    std::vector<std::size_t> m_Permutation;
    std::size_t m_Size;
    bool m_Decomposed = false;
};

/// `PolynomialLeastSquareFit.fitData` on coefficients ordered constant term first. Returns false,
/// leaving them unchanged, when there are fewer points than coefficients or the inverse of the normal
/// matrix contains NaN.
[[nodiscard]] bool solvePolynomialNormalEquations(std::span<const double> x, std::span<const double> y, std::vector<double>& coefficients);

/// `Polynomial.evaluate`: Horner's rule on coefficients ordered constant term first.
[[nodiscard]] double evaluatePolynomial(std::span<const double> coefficients, double x) noexcept;

/// `DatasetCurveFitter.getScratchParams`: the built-in fit's initial guess for every parameter from the
/// valid points, or nothing (polynomials, user fits, too few points, or an unusable heuristic). `fallback`
/// supplies values the heuristic does not estimate.
[[nodiscard]] std::optional<std::vector<double>>
initialGuess(BuiltInFit kind, std::span<const double> x, std::span<const double> y, std::span<const double> fallback, FitDialect dialect);

/// Corrected only: the Sinusoid or DampedSine guess with zero crossings counted with hysteresis (MoTrack #310),
/// or nothing when it finds fewer than two crossings, a value is not finite, or `kind` is not a sine fit.
/// `fitCurve` starts from it instead of Tracker's guess when it has the lower, defined starting deviation.
[[nodiscard]] std::optional<std::vector<double>>
hysteresisSineGuess(BuiltInFit kind, std::span<const double> x, std::span<const double> y, std::span<const double> fallback);

/// Default parameter values of a built-in fit.
[[nodiscard]] std::vector<double> builtInDefaults(BuiltInFit kind);

} // namespace motrack::science::fit_detail
