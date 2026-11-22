#pragma once

#include <cstdint>

namespace motrack
{

/// The resolution of a movie's decoded frame timestamps: its stream time base, numerator / denominator
/// seconds per tick (FFmpeg's AVStream::time_base; for example 1/1000 in Matroska and WebM files, 1/600 in
/// many phone recordings, 1/90000 in MPEG transport streams). Zero is unknown. MoTrack rescales decoded
/// timestamps to whole nanoseconds, so the effective resolution is never finer than 1 ns.
struct FrameTimeBase
{
    std::int32_t numerator = 0;
    std::int32_t denominator = 0;

    [[nodiscard]] constexpr bool known() const noexcept
    {
        return numerator > 0 && denominator > 0;
    }

    /// Seconds per tick, at least 1 ns; 1 ns when unknown.
    [[nodiscard]] constexpr double resolutionSeconds() const noexcept
    {
        constexpr double NANOSECOND = 1e-9;
        const double tick = known() ? static_cast<double>(numerator) / static_cast<double>(denominator) : 0.0;
        return tick > NANOSECOND ? tick : NANOSECOND;
    }

    friend constexpr bool operator==(const FrameTimeBase&, const FrameTimeBase&) = default;
};

} // namespace motrack
