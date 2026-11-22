#pragma once

#include <functional>
#include <span>

namespace motrack::science
{

/// A function of several parameters to minimize, such as a sum of squared deviations.
///
/// The minimizers call it synchronously, in the same order and with the same arguments as OSP. It
/// should report an undefined value with NaN rather than throwing; an exception propagates out of the
/// minimizer and leaves the parameter vector partially updated.
using MultiVarObjective = std::function<double(std::span<const double> parameters)>;

/// Iteration limit and tolerance with which Tracker's Data Tool autofit calls both minimizers.
inline constexpr int TRACKER_FIT_ITERATIONS = 20;
inline constexpr double TRACKER_FIT_TOLERANCE = 1.0e-6;

struct HessianMinimizeResult
{
    /// OSP's return value: the length of the last Newton step (9999 when no step ran, NaN after a
    /// singular Hessian).
    double error = 0.0;
    int iterations = 0;
};

/// Ports `org.opensourcephysics.numerics.HessianMinimize.minimize` (OSP d56207b, Tracker 6.3.5).
///
/// Five damped one-parameter Newton cycles (`crudeGuess`) are followed by full Newton steps with a
/// central-difference Hessian and gradient (step `(|x|+1)/1e5`), solved by LU decomposition with partial
/// pivoting. Iteration stops when the step length or the step relative to `|x|^2 + tolerance` falls
/// to `tolerance * 1e-6`, or after `maxIterations` steps. The better of the start and the result is
/// kept in `x` (a result containing NaN is always rejected). An empty `x` does nothing.
HessianMinimizeResult hessianMinimize(const MultiVarObjective& objective, std::span<double> x, int maxIterations, double tolerance);

struct LevenbergMarquardtResult
{
    /// OSP's return value: the last step met the tolerance. False leaves `x` at the best point
    /// visited, without OSP's final comparison with the start.
    bool converged = false;
    int iterations = 0;
};

/// Ports `org.opensourcephysics.numerics.LevenbergMarquardt.minimize` exactly, including its
/// termination once lambda (0.001, divided by 10 per accepted step) is no longer above 1e-6, which
/// limits it to three accepted steps (docs/curve-fitting.md).
LevenbergMarquardtResult
levenbergMarquardtMinimize(const MultiVarObjective& objective, std::span<double> x, int maxIterations, double tolerance);

} // namespace motrack::science
