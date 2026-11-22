#pragma once

#include "science/motion/motion_types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace motrack::science
{

// Ports of the point-mass derivative algorithms of Tracker 6.3.5 (dc064f92): FirstDerivative,
// SecondDerivative and BounceDerivatives as driven by PointMass. See docs/motion-kernels.md for the
// formulas, the settings Tracker exposes and the deliberate differences. Every function is pure and
// thread-safe. A missing input or a non-finite result is std::nullopt; nothing is replaced by zero.

/// Half-width of a finite-difference stencil in clip steps (Tracker's "spill").
enum class DerivativeSpill : std::uint8_t
{
    One = 1,
    Two = 2,
};

/// First derivative per clip step (FirstDerivative), present where steps k - spill .. k + spill all exist:
/// spill 1: (x[k+1] - x[k-1]) / 2; spill 2: the least-squares slope (-2 x[k-2] - x[k-1] + x[k+1] + 2 x[k+2]) / 10.
[[nodiscard]] std::vector<std::optional<double>> firstStepDifference(std::span<const std::optional<double>> samples, DerivativeSpill spill);

/// Second derivative per clip step squared (SecondDerivative), with the same validity window:
/// spill 1: x[k-1] - 2 x[k] + x[k+1]; spill 2: the least-squares parabola (2 x[k-2] - x[k-1] - 2 x[k] - x[k+1] + 2 x[k+2]) / 7.
[[nodiscard]] std::vector<std::optional<double>> secondStepDifference(std::span<const std::optional<double>> samples,
                                                                      DerivativeSpill spill);

/// The velocity/acceleration algorithms of Tracker's Algorithms dialog (plot popup > Algorithm...).
/// The choice is per point mass, is not saved in .trk files, and defaults to FiniteDifference.
enum class DerivativeAlgorithm : std::uint8_t
{
    /// "Finite Difference" (PointMass.FINITE_DIFF = 0): velocity spill 1, acceleration spill 2.
    FiniteDifference,
    /// "Smoother Finite Difference" (FINITE_DIFF_VSPILL2 = 2): velocity and acceleration spill 2.
    SmoothFiniteDifference,
    /// "Bounce Detection" (BOUNCE_DETECT = 1): Karplus' seven-step quadratic fits that also model a
    /// sudden velocity change ("bounce") inside the window.
    BounceDetection,
};

inline constexpr DerivativeAlgorithm TRACKER_DEFAULT_DERIVATIVE_ALGORITHM = DerivativeAlgorithm::FiniteDifference;

/// The algorithms in the order Tracker's dialog lists them.
inline constexpr std::array ALL_DERIVATIVE_ALGORITHMS = {
    DerivativeAlgorithm::FiniteDifference,
    DerivativeAlgorithm::SmoothFiniteDifference,
    DerivativeAlgorithm::BounceDetection,
};

/// Tracker's integer code for an algorithm (PointMass.setAlgorithm).
[[nodiscard]] int trackerAlgorithmCode(DerivativeAlgorithm algorithm) noexcept;

/// The algorithm for a Tracker code, or std::nullopt for an undefined code (which setAlgorithm ignores).
[[nodiscard]] std::optional<DerivativeAlgorithm> derivativeAlgorithmFromTrackerCode(int code) noexcept;

/// Derivatives before time scaling: `first` in length units per clip step, `second` per step squared.
struct StepDerivatives
{
    std::vector<std::optional<PlanarValue>> first;
    std::vector<std::optional<PlanarValue>> second;
};

/// Evaluates an algorithm on positions indexed by clip step (positions.size() == clip.stepCount).
///
/// A derivative is present only at a step with a position, as PointMass creates velocity and acceleration
/// vectors only there. The finite differences need every step of their window. Bounce Detection fits a
/// seven-step window, shifting it away from missing steps, so it can also estimate a step at a clip edge
/// or next to a gap. The clip lattice matters only to Bounce Detection, whose step search Tracker writes
/// in frame units; for a stride above 1 (where Tracker 6.3.5 throws) MoTrack gives Tracker's stride-1 result.
/// Throws std::invalid_argument for an invalid lattice or a size mismatch.
[[nodiscard]] StepDerivatives
stepDerivatives(DerivativeAlgorithm algorithm, const ClipLattice& clip, std::span<const std::optional<PlanarValue>> positions);

/// Velocity (length units per second) and acceleration (per second squared) at each clip step.
struct MotionDerivatives
{
    std::vector<std::optional<PlanarValue>> velocity;
    std::vector<std::optional<PlanarValue>> acceleration;
};

/// Point-mass velocity and acceleration as PointMass.refreshData scales the step derivatives.
///
/// `stepTimesSeconds[k]` is the time of clip step k in seconds: Tracker's VideoPlayer.getStepTime(k) / 1000.
/// For every algorithm the velocity divides by (t[k+1] - t[k-1]) / 2 and the acceleration by
/// ((t[k+2] - t[k-2]) / 4)^2, Tracker's mean step interval. A step whose neighbors lie outside the clip, or
/// whose times are not finite, has no value. Tracker's uniform stencils do not adapt to unequal step
/// intervals (variable frame-rate media); see docs/motion-kernels.md. Throws std::invalid_argument for an
/// invalid lattice or a size mismatch.
[[nodiscard]] MotionDerivatives pointMassDerivatives(DerivativeAlgorithm algorithm,
                                                     const ClipLattice& clip,
                                                     std::span<const std::optional<PlanarValue>> positions,
                                                     std::span<const double> stepTimesSeconds);

// MoTrack's extension of the default Finite Difference algorithm to measured step times, such as decoded
// movie timestamps, whose spacing Tracker's stencils assume to be even (#87, #331; docs/motion-kernels.md).

/// True when times[step - spill .. step + spill] increase and every interior time lies within the timestamp
/// resolution of the evenly spaced grid between the window's end times, which is what Tracker's stencils
/// assume. The tolerance is max(timeResolutionSeconds, 1 ns) for the rounding of each timestamp to its time
/// base, plus 1 ns for MoTrack's rescaling to whole nanoseconds and 8 ulps of the largest time. For three
/// steps this says the two intervals agree within 2 max(resolution, 1 ns) + 2 ns. A non-positive or
/// non-finite resolution counts as 1 ns.
[[nodiscard]] bool evenlySpacedWindow(std::span<const double> times, std::size_t step, std::size_t spill, double timeResolutionSeconds);

/// The derivative at times[1] of the quadratic through three samples, exact for constant acceleration at
/// any spacing; for equal intervals it is Tracker's (x[2] - x[0]) / (t[2] - t[0]). Missing unless the times
/// increase and every input and the result are finite.
[[nodiscard]] std::optional<double> quadraticSlope(std::span<const double, 3> times, std::span<const double, 3> values);

/// 2c of the least-squares parabola x ≈ a + b (t - times[2]) + c (t - times[2])² through five samples,
/// exact for constant acceleration at any spacing. At equal spacing Δ it is Tracker's five-point
/// (2x[0] - x[1] - 2x[2] - x[3] + 2x[4]) / (7 Δ²) (see the derivation in the source). Missing unless the
/// times increase and every input and the result are finite.
[[nodiscard]] std::optional<double> leastSquaresCurvature(std::span<const double, 5> times, std::span<const double, 5> values);

/// Velocity and acceleration for measured step times (seconds, one per clip step). Wherever a window's
/// times are evenly spaced within `timeResolutionSeconds` (evenlySpacedWindow), the values are Tracker's
/// Finite Difference ones (pointMassDerivatives, bit for bit). Elsewhere velocity is quadraticSlope over
/// steps k-1..k+1 and acceleration leastSquaresCurvature over k-2..k+2, so the acceleration estimator is a
/// five-point fit everywhere. Windows and missing values follow Tracker: every step of a window must have a
/// position. Throws std::invalid_argument for an invalid lattice or a size mismatch.
[[nodiscard]] MotionDerivatives measuredTimeDerivatives(const ClipLattice& clip,
                                                        std::span<const std::optional<PlanarValue>> positions,
                                                        std::span<const double> stepTimesSeconds,
                                                        double timeResolutionSeconds);

/// Polar-angle motion of the position vector about the world origin.
struct AngularMotion
{
    /// θ in radians: the cumulative atan2(y, x), unwrapped across ±π and across missing steps.
    std::vector<std::optional<double>> angle;
    /// ω = dθ/dt in radians per second (always the spill-1 difference, whatever the algorithm).
    std::vector<std::optional<double>> angularVelocity;
    /// α = d²θ/dt² in radians per second squared (always the spill-2 parabola).
    std::vector<std::optional<double>> angularAcceleration;
};

/// PointMass.getRotationData with refreshData's time scaling (stepTimesSeconds as for pointMassDerivatives).
/// Throws std::invalid_argument if the sizes differ.
[[nodiscard]] AngularMotion pointMassAngularMotion(std::span<const std::optional<PlanarValue>> positions,
                                                   std::span<const double> stepTimesSeconds);

} // namespace motrack::science
