#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>

namespace motrack::science
{

// A port of OSP's org.opensourcephysics.numerics.Derivative (d56207b0, unchanged on the swingJS tip):
// finite-difference derivatives of a function, with OSP's operand order. Tracker 6.3.5 does not call
// this class (its point-mass derivatives are in motion_derivatives.hpp); it is ported for the Data Tool
// and function slices and checked with the cases of OSP's src/test/Derivative.java.

using RealFunction = std::function<double(double)>;
using MultivariateFunction = std::function<double(std::span<const double>)>;

/// Derivative.first / centered / getFirst: (f(x + h) - f(x - h)) / h / 2.
[[nodiscard]] double centeredDerivative(const RealFunction& f, double x, double h);

/// Derivative.forward: (-f(x + 2h) + 4 f(x + h) - 3 f(x)) / h / 2.
[[nodiscard]] double forwardDerivative(const RealFunction& f, double x, double h);

/// Derivative.backward: (f(x - 2h) - 4 f(x - h) + 3 f(x)) / h / 2.
[[nodiscard]] double backwardDerivative(const RealFunction& f, double x, double h);

/// Derivative.second / getSecond: (f(x + h) - 2 f(x) + f(x - h)) / h / h.
[[nodiscard]] double secondDerivative(const RealFunction& f, double x, double h);

/// Derivative.firstPartial: the centred difference along coordinate `index`, (f(x + h e) - f(x - h e)) / 2 / h.
/// Throws std::out_of_range for an index outside the point.
[[nodiscard]] double partialDerivative(const MultivariateFunction& f, std::span<const double> point, std::size_t index, double h);

enum class RombergStatus : std::uint8_t
{
    Converged,
    /// All six Romberg columns were filled without two estimates within the tolerance (OSP error code 1).
    NotConverged,
    /// The halved step fell below sqrt(Double.MIN_VALUE) (OSP error code 2), including a negative step.
    StepBelowPrecision,
};

struct RombergResult
{
    /// The converged estimate, or the last centred difference that OSP reports in its exception.
    double value;
    RombergStatus status;
};

/// Derivative.romberg: Richardson extrapolation of centred differences with halving steps, returning
/// once successive extrapolations differ by less than `tolerance`. OSP throws on failure; this reports it.
[[nodiscard]] RombergResult rombergDerivative(const RealFunction& f, double x, double h, double tolerance);

} // namespace motrack::science
