#pragma once

#include <cstdint>
#include <optional>
#include <span>

namespace motrack::science
{

/// An exact duration of numerator / denominator seconds, such as a stream time base (FFmpeg's AVStream::time_base,
/// seconds per timestamp tick). It is valid when both parts are positive.
struct SecondsRatio
{
    std::int32_t numerator = 0;
    std::int32_t denominator = 0;

    [[nodiscard]] constexpr bool valid() const noexcept
    {
        return numerator > 0 && denominator > 0;
    }

    friend constexpr bool operator==(const SecondsRatio&, const SecondsRatio&) = default;
};

inline constexpr SecondsRatio ONE_MILLISECOND{.numerator = 1, .denominator = 1000};

/// Infers the precision that a movie's timestamps actually have, which can be coarser than the time base they are
/// counted in (#335; docs/motion-kernels.md). FFmpeg's MP4 muxer, for example, raises the 1/1000 s time base of
/// Matroska or WebM to 1/16000 s when it remuxes them, so whole-millisecond timestamps arrive as multiples of 16 ticks.
///
/// The rule is max(time base, 1 ms) when every timestamp is a whole millisecond: when the tick is finer than 1 ms and
/// the offset of every timestamp from the first is a whole number of milliseconds, the precision is 1 ms; otherwise
/// it is the time base. So:
/// - One timestamp off the millisecond lattice keeps the time base, and variable timing that uses the finer ticks is
///   classified exactly as with the time base alone.
/// - The precision is never coarser than max(time base, 1 ms). The greatest common divisor of the offsets could grow
///   to a whole frame interval (a constant 1001/30000 s rate gives 1001 ticks) or to the common factor of a few
///   genuine intervals (3000 and 3060 ticks of 1/90000 s give 60), and so hide real variable timing; this rule cannot.
/// - Offsets count from the first timestamp, so a start offset (an edit list, a decoder delay) does not matter.
///
/// Integer arithmetic only: no overflow for any 64-bit timestamps.
class TimestampPrecision
{
  public:
    /// For timestamps in ticks of `timeBase`.
    explicit TimestampPrecision(SecondsRatio timeBase) noexcept;

    /// Adds a timestamp, in ticks of the time base.
    void add(std::int64_t ticks) noexcept;

    /// The precision of the timestamps added so far. It is the time base until two different timestamps have been
    /// added; an invalid time base is returned as it is.
    [[nodiscard]] SecondsRatio precision() const noexcept;

  private:
    SecondsRatio m_TimeBase;
    /// The least positive number of ticks that is a whole number of milliseconds; 0 when the tick is not finer than
    /// 1 ms or the time base is invalid.
    std::int64_t m_MillisecondTicks = 0;
    std::optional<std::int64_t> m_First;
    bool m_Advanced = false;
    bool m_WholeMilliseconds = true;
};

/// TimestampPrecision over a sequence of timestamps in ticks of `timeBase`.
[[nodiscard]] SecondsRatio timestampPrecision(SecondsRatio timeBase, std::span<const std::int64_t> ticks) noexcept;

} // namespace motrack::science
