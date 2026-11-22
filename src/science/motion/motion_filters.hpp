#pragma once

#include "science/motion/motion_types.hpp"

#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace motrack::science
{

// Ports of Tracker 6.3.5's MotionFilter implementations, which a filtered point-mass clone applies to the
// world x and y positions of its source, one sample per clip step, before differentiating them. Each
// filter works separately on every run of consecutive present samples; missing samples stay missing.
// Parameters are normalized exactly as Tracker's setters normalize them. See docs/motion-kernels.md.

/// Centred moving average ("Moving Average"). Near a run's ends the window is truncated to the run, so
/// an edge sample averages itself and the neighbours on one side only.
class MovingAverageFilter
{
  public:
    static constexpr int DEFAULT_WINDOW = 5;

    /// The window in samples: below 1 becomes 1, an even value is rounded up (setWindow).
    explicit MovingAverageFilter(int window = DEFAULT_WINDOW) noexcept;

    [[nodiscard]] int window() const noexcept;

    friend bool operator==(const MovingAverageFilter&, const MovingAverageFilter&) = default;

  private:
    int m_Window;
};

/// Low-pass Butterworth filter applied forward and backward ("Butterworth Lowpass", zero phase): a
/// cascade of bilinear-transform biquads with prewarping, steady-state initial conditions and odd
/// reflection padding. The forward-backward pass doubles the order and squares the magnitude response,
/// so the cutoff is the -6 dB point of the result. A cutoff at or above Nyquist leaves samples unchanged,
/// as do runs shorter than 3 samples.
class ButterworthFilter
{
  public:
    static constexpr int DEFAULT_ORDER = 4;
    static constexpr double DEFAULT_CUTOFF_HZ = 6.0;
    static constexpr double DEFAULT_SAMPLE_RATE_HZ = 30.0;

    /// The order is clamped to 1..8; a cutoff or sample rate at or below zero becomes 1 Hz.
    explicit ButterworthFilter(int order = DEFAULT_ORDER,
                               double cutoffHz = DEFAULT_CUTOFF_HZ,
                               double sampleRateHz = DEFAULT_SAMPLE_RATE_HZ) noexcept;

    [[nodiscard]] int order() const noexcept;
    [[nodiscard]] double cutoffHz() const noexcept;
    [[nodiscard]] double sampleRateHz() const noexcept;

    friend bool operator==(const ButterworthFilter&, const ButterworthFilter&) = default;

  private:
    int m_Order;
    double m_CutoffHz;
    double m_SampleRateHz;
};

/// Savitzky-Golay smoothing: the least-squares polynomial over a centred window, evaluated at its centre,
/// with one-sided windows of the same length at a run's ends. A run shorter than the window uses the
/// longest odd window that fits; a run with fewer samples than order + 1 is left unchanged.
class SavitzkyGolayFilter
{
  public:
    static constexpr int DEFAULT_WINDOW = 7;
    static constexpr int DEFAULT_POLYNOMIAL_ORDER = 2;

    /// The window is at least 3 and odd (rounded up); the order is at least 1 and below the window.
    explicit SavitzkyGolayFilter(int window = DEFAULT_WINDOW, int polynomialOrder = DEFAULT_POLYNOMIAL_ORDER) noexcept;

    [[nodiscard]] int window() const noexcept;
    [[nodiscard]] int polynomialOrder() const noexcept;

    friend bool operator==(const SavitzkyGolayFilter&, const SavitzkyGolayFilter&) = default;

  private:
    int m_Window;
    int m_PolynomialOrder;
};

using MotionFilter = std::variant<MovingAverageFilter, ButterworthFilter, SavitzkyGolayFilter>;

/// The sample rate Tracker's filter dialog gives a new Butterworth filter: 1000 / the mean clip-step
/// duration in milliseconds, or 30 Hz when that duration is not positive. Tracker saves the rate with the
/// filter and does not update it when the clip timing or stride later changes.
[[nodiscard]] double butterworthSampleRateHz(double meanStepDurationMs) noexcept;

/// Filters one coordinate, one sample per clip step. A missing or non-finite sample is a gap and stays
/// missing; a non-finite filtered value (possible only after overflow) is also missing.
[[nodiscard]] std::vector<std::optional<double>> applyMotionFilter(const MotionFilter& filter,
                                                                   std::span<const std::optional<double>> samples);

/// Filters x and y separately with the same gaps, as FilteredPointMass.refreshPositions does.
[[nodiscard]] std::vector<std::optional<PlanarValue>> filterPositions(const MotionFilter& filter,
                                                                      std::span<const std::optional<PlanarValue>> positions);

/// Root-mean-square distance of filtered positions from their source (the dialog's "RMS Dev").
struct FilterDeviation
{
    double x;
    double y;
    double combined; ///< sqrt(x^2 + y^2)
};

/// The RMS deviation over steps where both the source and the filtered position exist, or std::nullopt
/// when there are none (Tracker shows 0 then). Throws std::invalid_argument if the sizes differ.
[[nodiscard]] std::optional<FilterDeviation> filterRmsDeviation(std::span<const std::optional<PlanarValue>> source,
                                                                std::span<const std::optional<PlanarValue>> filtered);

} // namespace motrack::science
