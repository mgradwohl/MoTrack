#include "science/motion/motion_derivatives.hpp"

#include "science/motion/bounce_derivatives.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>
#include <utility>

namespace motrack::science
{

bool ClipLattice::valid() const noexcept
{
    if (firstFrame < 0 || stepSize < 1 || stepCount < 0)
    {
        return false;
    }
    const auto last = static_cast<std::int64_t>(firstFrame) + static_cast<std::int64_t>(stepSize) * std::max(0, stepCount - 1);
    return last <= std::numeric_limits<int>::max();
}

int ClipLattice::frame(int step) const noexcept
{
    return firstFrame + stepSize * step;
}

std::vector<std::optional<PlanarValue>> clipStepPositions(const ClipLattice& clip, std::span<const FramePosition> marks)
{
    if (!clip.valid())
    {
        throw std::invalid_argument("Invalid clip lattice");
    }
    std::vector<std::optional<PlanarValue>> positions(static_cast<std::size_t>(clip.stepCount));
    for (const auto& mark : marks)
    {
        const auto offset = static_cast<std::int64_t>(mark.frame) - clip.firstFrame;
        if (offset < 0 || offset % clip.stepSize != 0 || offset / clip.stepSize >= clip.stepCount)
        {
            continue;
        }
        auto& position = positions[static_cast<std::size_t>(offset / clip.stepSize)];
        if (std::isfinite(mark.position.x) && std::isfinite(mark.position.y))
        {
            position = mark.position;
        }
        else
        {
            position.reset();
        }
    }
    return positions;
}

namespace
{

constexpr double NOT_A_NUMBER = std::numeric_limits<double>::quiet_NaN();

/// A present, finite value or NaN, so that the formulas below can be written exactly as in Java.
double valueOrNan(const std::optional<double>& sample)
{
    return sample && std::isfinite(*sample) ? *sample : NOT_A_NUMBER;
}

/// Positions as NaN-marked coordinate arrays: every present position has finite coordinates.
struct Coordinates
{
    std::vector<double> x;
    std::vector<double> y;
};

Coordinates coordinates(std::span<const std::optional<PlanarValue>> positions)
{
    Coordinates result{.x = std::vector<double>(positions.size(), NOT_A_NUMBER), .y = std::vector<double>(positions.size(), NOT_A_NUMBER)};
    for (std::size_t step = 0; step < positions.size(); ++step)
    {
        if (const auto& position = positions[step]; position && std::isfinite(position->x) && std::isfinite(position->y))
        {
            result.x[step] = position->x;
            result.y[step] = position->y;
        }
    }
    return result;
}

/// FirstDerivative/SecondDerivative's validity rule: steps k - spill .. k + spill must all exist.
bool windowPresent(std::span<const double> values, std::size_t step, std::size_t spill)
{
    if (step < spill || step + spill >= values.size())
    {
        return false;
    }
    for (std::size_t index = step - spill; index <= step + spill; ++index)
    {
        if (std::isnan(values[index]))
        {
            return false;
        }
    }
    return true;
}

// The formulas of FirstDerivative and SecondDerivative, in Java's operand order.

double firstDifferenceAt(std::span<const double> x, std::size_t k, DerivativeSpill spill)
{
    if (spill == DerivativeSpill::One)
    {
        return (-x[k - 1] + x[k + 1]) / 2;
    }
    return (-2 * x[k - 2] - x[k - 1] + x[k + 1] + 2 * x[k + 2]) / 10;
}

double secondDifferenceAt(std::span<const double> x, std::size_t k, DerivativeSpill spill)
{
    if (spill == DerivativeSpill::One)
    {
        return (+x[k - 1] - 2 * x[k] + x[k + 1]);
    }
    return (+2 * x[k - 2] - x[k - 1] - 2 * x[k] - x[k + 1] + 2 * x[k + 2]) / 7;
}

using DifferenceFormula = double (*)(std::span<const double>, std::size_t, DerivativeSpill);

std::vector<std::optional<double>>
difference(std::span<const std::optional<double>> samples, DerivativeSpill spill, DifferenceFormula formula)
{
    std::vector<double> values(samples.size());
    for (std::size_t step = 0; step < samples.size(); ++step)
    {
        values[step] = valueOrNan(samples[step]);
    }
    std::vector<std::optional<double>> result(samples.size());
    for (std::size_t step = 0; step < samples.size(); ++step)
    {
        if (windowPresent(values, step, static_cast<std::size_t>(spill)))
        {
            if (const double value = formula(values, step, spill); std::isfinite(value))
            {
                result[step] = value;
            }
        }
    }
    return result;
}

std::vector<std::optional<PlanarValue>> planarDifference(const Coordinates& positions, DerivativeSpill spill, DifferenceFormula formula)
{
    std::vector<std::optional<PlanarValue>> result(positions.x.size());
    for (std::size_t step = 0; step < positions.x.size(); ++step)
    {
        if (!windowPresent(positions.x, step, static_cast<std::size_t>(spill)))
        {
            continue;
        }
        const double x = formula(positions.x, step, spill);
        const double y = formula(positions.y, step, spill);
        if (std::isfinite(x) && std::isfinite(y))
        {
            result[step] = PlanarValue{.x = x, .y = y};
        }
    }
    return result;
}

void requireLattice(const ClipLattice& clip, std::size_t positions)
{
    if (!clip.valid())
    {
        throw std::invalid_argument("Invalid clip lattice");
    }
    if (std::cmp_not_equal(positions, clip.stepCount))
    {
        throw std::invalid_argument("Expected one position per clip step, got " + std::to_string(positions));
    }
}

void requireTimes(std::size_t positions, std::size_t times)
{
    if (positions != times)
    {
        throw std::invalid_argument("Expected one step time per clip step, got " + std::to_string(times));
    }
}

/// VideoPlayer.getStepTime: NaN outside the clip.
double stepTime(std::span<const double> times, std::ptrdiff_t step)
{
    return step < 0 || step >= std::ssize(times) ? NOT_A_NUMBER : times[static_cast<std::size_t>(step)];
}

// PointMass.refreshData uses vDerivSpill = 1 and aDerivSpill = 2 for every algorithm.

double velocityInterval(std::span<const double> times, std::ptrdiff_t step)
{
    const double tf = stepTime(times, step + 1);
    const double to = stepTime(times, step - 1);
    return (tf - to) / (2 * 1);
}

double accelerationIntervalSquared(std::span<const double> times, std::ptrdiff_t step)
{
    const double tf = stepTime(times, step + 2);
    const double to = stepTime(times, step - 2);
    return (tf - to) * (tf - to) / (4 * 2 * 2);
}

std::optional<PlanarValue> scaled(const std::optional<PlanarValue>& value, double divisor)
{
    if (!value)
    {
        return std::nullopt;
    }
    const PlanarValue result{.x = value->x / divisor, .y = value->y / divisor};
    if (!std::isfinite(result.x) || !std::isfinite(result.y))
    {
        return std::nullopt;
    }
    return result;
}

std::optional<double> scaled(const std::optional<double>& value, double divisor)
{
    if (!value)
    {
        return std::nullopt;
    }
    const double result = *value / divisor;
    return std::isfinite(result) ? std::optional(result) : std::nullopt;
}

} // namespace

std::vector<std::optional<double>> firstStepDifference(std::span<const std::optional<double>> samples, DerivativeSpill spill)
{
    return difference(samples, spill, firstDifferenceAt);
}

std::vector<std::optional<double>> secondStepDifference(std::span<const std::optional<double>> samples, DerivativeSpill spill)
{
    return difference(samples, spill, secondDifferenceAt);
}

int trackerAlgorithmCode(DerivativeAlgorithm algorithm) noexcept
{
    // PointMass.FINITE_DIFF = 0, FINITE_DIFF_VSPILL2 = 2 and BOUNCE_DETECT = 1, in enumerator order.
    constexpr std::array CODES = {0, 2, 1};
    return CODES[static_cast<std::size_t>(algorithm)];
}

std::optional<DerivativeAlgorithm> derivativeAlgorithmFromTrackerCode(int code) noexcept
{
    for (const auto algorithm : ALL_DERIVATIVE_ALGORITHMS)
    {
        if (trackerAlgorithmCode(algorithm) == code)
        {
            return algorithm;
        }
    }
    return std::nullopt;
}

StepDerivatives
stepDerivatives(DerivativeAlgorithm algorithm, const ClipLattice& clip, std::span<const std::optional<PlanarValue>> positions)
{
    requireLattice(clip, positions.size());
    const Coordinates values = coordinates(positions);
    if (algorithm != DerivativeAlgorithm::BounceDetection)
    {
        const auto velocitySpill = algorithm == DerivativeAlgorithm::SmoothFiniteDifference ? DerivativeSpill::Two : DerivativeSpill::One;
        return {.first = planarDifference(values, velocitySpill, firstDifferenceAt),
                .second = planarDifference(values, DerivativeSpill::Two, secondDifferenceAt)};
    }
    std::vector<std::optional<PlanarValue>> finite(positions.size());
    for (std::size_t step = 0; step < positions.size(); ++step)
    {
        if (!std::isnan(values.x[step]))
        {
            finite[step] = PlanarValue{.x = values.x[step], .y = values.y[step]};
        }
    }
    auto result = motion_detail::bounceStepDerivatives(clip, finite);
    // PointMass keeps a vector only at a step that has a position.
    for (std::size_t step = 0; step < positions.size(); ++step)
    {
        for (auto* derivative : {&result.first[step], &result.second[step]})
        {
            if (std::isnan(values.x[step]) || (*derivative && (!std::isfinite((*derivative)->x) || !std::isfinite((*derivative)->y))))
            {
                derivative->reset();
            }
        }
    }
    return result;
}

MotionDerivatives pointMassDerivatives(DerivativeAlgorithm algorithm,
                                       const ClipLattice& clip,
                                       std::span<const std::optional<PlanarValue>> positions,
                                       std::span<const double> stepTimesSeconds)
{
    requireLattice(clip, positions.size());
    requireTimes(positions.size(), stepTimesSeconds.size());
    const auto steps = stepDerivatives(algorithm, clip, positions);
    MotionDerivatives result{.velocity = std::vector<std::optional<PlanarValue>>(positions.size()),
                             .acceleration = std::vector<std::optional<PlanarValue>>(positions.size())};
    for (std::size_t step = 0; step < positions.size(); ++step)
    {
        const auto index = static_cast<std::ptrdiff_t>(step);
        result.velocity[step] = scaled(steps.first[step], velocityInterval(stepTimesSeconds, index));
        result.acceleration[step] = scaled(steps.second[step], accelerationIntervalSquared(stepTimesSeconds, index));
    }
    return result;
}

namespace
{

constexpr double NANOSECOND = 1e-9;

std::optional<double> finiteOrMissing(double value)
{
    return std::isfinite(value) ? std::optional(value) : std::nullopt;
}

/// Whether every value is finite and the times strictly increase.
template <std::size_t N> bool increasingAndFinite(std::span<const double, N> times, std::span<const double, N> values)
{
    for (std::size_t i = 0; i < N; ++i)
    {
        if (!std::isfinite(times[i]) || !std::isfinite(values[i]) || (i > 0 && !(times[i] > times[i - 1])))
        {
            return false;
        }
    }
    return true;
}

/// One coordinate of the time-aware estimates at `step`, or std::nullopt when its window is incomplete.
std::optional<double>
unevenComponent(std::span<const double> coordinate, std::span<const double> times, std::size_t step, DerivativeSpill spill)
{
    const auto half = static_cast<std::size_t>(spill);
    if (!windowPresent(coordinate, step, half))
    {
        return std::nullopt;
    }
    if (spill == DerivativeSpill::One)
    {
        return quadraticSlope(times.subspan(step - 1).first<3>(), coordinate.subspan(step - 1).first<3>());
    }
    return leastSquaresCurvature(times.subspan(step - 2).first<5>(), coordinate.subspan(step - 2).first<5>());
}

std::optional<PlanarValue>
unevenDerivative(const Coordinates& positions, std::span<const double> times, std::size_t step, DerivativeSpill spill)
{
    const auto x = unevenComponent(positions.x, times, step, spill);
    const auto y = unevenComponent(positions.y, times, step, spill);
    if (!x || !y)
    {
        return std::nullopt;
    }
    return PlanarValue{.x = *x, .y = *y};
}

} // namespace

bool evenlySpacedWindow(std::span<const double> times, std::size_t step, std::size_t spill, double timeResolutionSeconds)
{
    if (spill == 0 || step < spill || step + spill >= times.size())
    {
        return false;
    }
    const auto first = step - spill;
    const auto last = step + spill;
    double magnitude = 0.0;
    for (auto index = first; index <= last; ++index)
    {
        if (!std::isfinite(times[index]) || (index > first && !(times[index] > times[index - 1])))
        {
            return false;
        }
        magnitude = std::max(magnitude, std::abs(times[index]));
    }
    const double resolution =
        std::isfinite(timeResolutionSeconds) && timeResolutionSeconds > NANOSECOND ? timeResolutionSeconds : NANOSECOND;
    const double tolerance = resolution + NANOSECOND + 8.0 * std::numeric_limits<double>::epsilon() * magnitude;
    const double interval = (times[last] - times[first]) / static_cast<double>(2 * spill);
    for (auto offset = std::size_t{1}; offset < 2 * spill; ++offset)
    {
        const double grid = times[first] + static_cast<double>(offset) * interval;
        if (std::abs(times[first + offset] - grid) > tolerance)
        {
            return false;
        }
    }
    return true;
}

std::optional<double> quadraticSlope(std::span<const double, 3> times, std::span<const double, 3> values)
{
    if (!increasingAndFinite(times, values))
    {
        return std::nullopt;
    }
    const double before = times[1] - times[0];
    const double after = times[2] - times[1];
    const double elapsed = before + after;
    const double firstSlope = (values[1] - values[0]) / before;
    const double secondSlope = (values[2] - values[1]) / after;
    // The quadratic's slope at the middle time weights each secant by the length of the other interval.
    return finiteOrMissing((after / elapsed) * firstSlope + (before / elapsed) * secondSlope);
}

std::optional<double> leastSquaresCurvature(std::span<const double, 5> times, std::span<const double, 5> values)
{
    if (!increasingAndFinite(times, values))
    {
        return std::nullopt;
    }
    // Fit y = a + b u + c u² with u = (t - t₂) / s and y = x - x₂, where s = (t₄ - t₀) / 4 is the mean interval
    // (Tracker's), so that u and the normal equations stay of order 1. With S_p = Σ u^p and T_p = Σ u^p y the
    // normal equations are
    //     [S0 S1 S2] [a]   [T0]
    //     [S1 S2 S3] [b] = [T1]
    //     [S2 S3 S4] [c]   [T2]
    // and Cramer's rule gives c = D_c / D. The acceleration is 2c / s².
    //
    // At equal spacing u = -2, -1, 0, 1, 2: S0 = 5, S1 = S3 = 0, S2 = 10, S4 = 34, so D = 5·10·34 - 10·10·10 = 700
    // and D_c = 5·10·T2 - 10·10·T0 = 50 (T2 - 2 T0). Then 2c = (T2 - 2 T0) / 7 = Σ (u² - 2) y / 7, whose weights
    // u² - 2 are (2, -1, -2, -1, 2). They sum to 0, so y = x - x₂ may be replaced by x, and the acceleration is
    // (2x₀ - x₁ - 2x₂ - x₃ + 2x₄) / (7 s²): Tracker's SecondDerivative (spill 2) over ((t₄ - t₀) / 4)².
    const double scale = (times[4] - times[0]) / 4;
    std::array<double, 5> power{};  // S0..S4
    std::array<double, 3> moment{}; // T0..T2
    for (std::size_t i = 0; i < 5; ++i)
    {
        const double u = (times[i] - times[2]) / scale;
        const double y = values[i] - values[2];
        double term = 1.0;
        for (std::size_t p = 0; p < 5; ++p)
        {
            power[p] += term;
            if (p < 3)
            {
                moment[p] += term * y;
            }
            term *= u;
        }
    }
    const auto [s0, s1, s2, s3, s4] = power;
    const auto [t0, t1, t2] = moment;
    const double determinant = s0 * (s2 * s4 - s3 * s3) - s1 * (s1 * s4 - s2 * s3) + s2 * (s1 * s3 - s2 * s2);
    const double curvature = s0 * (s2 * t2 - t1 * s3) - s1 * (s1 * t2 - t1 * s2) + t0 * (s1 * s3 - s2 * s2);
    if (!(determinant > 0.0))
    {
        return std::nullopt;
    }
    return finiteOrMissing(2.0 * (curvature / determinant) / (scale * scale));
}

MotionDerivatives measuredTimeDerivatives(const ClipLattice& clip,
                                          std::span<const std::optional<PlanarValue>> positions,
                                          std::span<const double> stepTimesSeconds,
                                          double timeResolutionSeconds)
{
    auto result = pointMassDerivatives(DerivativeAlgorithm::FiniteDifference, clip, positions, stepTimesSeconds);
    const Coordinates values = coordinates(positions);
    for (std::size_t step = 0; step < positions.size(); ++step)
    {
        if (!evenlySpacedWindow(stepTimesSeconds, step, 1, timeResolutionSeconds))
        {
            result.velocity[step] = unevenDerivative(values, stepTimesSeconds, step, DerivativeSpill::One);
        }
        if (!evenlySpacedWindow(stepTimesSeconds, step, 2, timeResolutionSeconds))
        {
            result.acceleration[step] = unevenDerivative(values, stepTimesSeconds, step, DerivativeSpill::Two);
        }
    }
    return result;
}

AngularMotion pointMassAngularMotion(std::span<const std::optional<PlanarValue>> positions, std::span<const double> stepTimesSeconds)
{
    requireTimes(positions.size(), stepTimesSeconds.size());
    const Coordinates values = coordinates(positions);
    AngularMotion result{.angle = std::vector<std::optional<double>>(positions.size()),
                         .angularVelocity = std::vector<std::optional<double>>(positions.size()),
                         .angularAcceleration = std::vector<std::optional<double>>(positions.size())};
    // getRotationData: accumulate the change of atan2(y, x), wrapped into [-pi, pi], across missing steps.
    double rotation = 0;
    double previousAngle = 0;
    for (std::size_t step = 0; step < positions.size(); ++step)
    {
        if (std::isnan(values.x[step]))
        {
            continue;
        }
        const double angle = std::atan2(values.y[step], values.x[step]);
        double delta = angle - previousAngle;
        if (delta < -std::numbers::pi)
        {
            delta += 2 * std::numbers::pi;
        }
        else if (delta > std::numbers::pi)
        {
            delta -= 2 * std::numbers::pi;
        }
        rotation += delta;
        previousAngle = angle;
        result.angle[step] = rotation;
    }
    const auto omega = firstStepDifference(result.angle, DerivativeSpill::One);
    const auto alpha = secondStepDifference(result.angle, DerivativeSpill::Two);
    for (std::size_t step = 0; step < positions.size(); ++step)
    {
        const auto index = static_cast<std::ptrdiff_t>(step);
        result.angularVelocity[step] = scaled(omega[step], velocityInterval(stepTimesSeconds, index));
        result.angularAcceleration[step] = scaled(alpha[step], accelerationIntervalSquared(stepTimesSeconds, index));
    }
    return result;
}

} // namespace motrack::science
