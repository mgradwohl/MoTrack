#pragma once

#include <cstdint>

namespace motrack
{

/// The resolution of a movie's decoded frame timestamps: its stream time base, numerator / denominator
/// seconds per tick (FFmpeg's AVStream::time_base; for example 1/1000 in Matroska and WebM files, 1/600 in
/// many phone recordings, 1/90000 in MPEG transport streams), and the precision the timestamps actually have.
/// Zero is unknown. MoTrack rescales decoded timestamps to whole nanoseconds, so the effective resolution is
/// never finer than 1 ns.
struct FrameTimeBase
{
    std::int32_t numerator = 0;
    std::int32_t denominator = 0;
    /// The precision of the timestamps themselves, precisionNumerator / precisionDenominator seconds, inferred
    /// from them when the movie is indexed (science::TimestampPrecision, #335): the tick, or 1 ms when every
    /// timestamp is a whole millisecond counted in finer ticks (MKV or WebM remuxed to MP4 declares 1/16000 s).
    /// Zero is not inferred, as in documents saved before #335; the tick then applies.
    std::int32_t precisionNumerator = 0;
    std::int32_t precisionDenominator = 0;

    [[nodiscard]] constexpr bool known() const noexcept
    {
        return numerator > 0 && denominator > 0;
    }

    [[nodiscard]] constexpr bool precisionKnown() const noexcept
    {
        return known() && precisionNumerator > 0 && precisionDenominator > 0;
    }

    /// Seconds per tick of the timestamps' resolution: the coarser of the tick and the inferred precision, at
    /// least 1 ns; 1 ns when unknown.
    [[nodiscard]] constexpr double resolutionSeconds() const noexcept
    {
        constexpr double NANOSECOND = 1e-9;
        const double tick = known() ? static_cast<double>(numerator) / static_cast<double>(denominator) : 0.0;
        const double precision =
            precisionKnown() ? static_cast<double>(precisionNumerator) / static_cast<double>(precisionDenominator) : 0.0;
        const double resolution = precision > tick ? precision : tick;
        return resolution > NANOSECOND ? resolution : NANOSECOND;
    }

    friend constexpr bool operator==(const FrameTimeBase&, const FrameTimeBase&) = default;
};

} // namespace motrack
