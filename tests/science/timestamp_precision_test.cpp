#include "science/motion/timestamp_precision.hpp"

#include "science/motion/motion_derivatives.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

// Independent expectations for the inferred timestamp precision (#335): timestamps built tick by tick from a known
// frame rate and clock, so the expected precision follows from how they were made.

namespace
{

namespace science = motrack::science;
using science::SecondsRatio;

constexpr SecondsRatio MP4_REMUX{.numerator = 1, .denominator = 16000};
constexpr SecondsRatio TRANSPORT_STREAM{.numerator = 1, .denominator = 90000};

/// The whole-millisecond timestamps of a constant frame rate (rateNumerator / rateDenominator seconds per frame) that
/// a Matroska or WebM muxer writes: the true time of each frame rounded to 1 ms.
std::vector<std::int64_t> wholeMilliseconds(int frames, std::int64_t rateNumerator, std::int64_t rateDenominator)
{
    std::vector<std::int64_t> result;
    result.reserve(static_cast<std::size_t>(frames));
    for (std::int64_t frame = 0; frame < frames; ++frame)
    {
        result.push_back(std::llround(static_cast<double>(frame * 1000 * rateNumerator) / static_cast<double>(rateDenominator)));
    }
    return result;
}

/// Millisecond timestamps counted in ticks: `ticksPerMillisecond` each (16 for FFmpeg's MP4 remux), from `start`.
std::vector<std::int64_t> remuxed(const std::vector<std::int64_t>& milliseconds, std::int64_t ticksPerMillisecond, std::int64_t start = 0)
{
    std::vector<std::int64_t> result;
    result.reserve(milliseconds.size());
    for (const auto value : milliseconds)
    {
        result.push_back(start + value * ticksPerMillisecond);
    }
    return result;
}

/// The timestamps a muxer writes for the true times of a constant frame rate in ticks of 1/timescale s, rounded
/// to the nearest tick.
std::vector<std::int64_t> nativeTicks(int frames, double framesPerSecond, double timescale)
{
    std::vector<std::int64_t> result;
    result.reserve(static_cast<std::size_t>(frames));
    for (int frame = 0; frame < frames; ++frame)
    {
        result.push_back(std::llround(frame * timescale / framesPerSecond));
    }
    return result;
}

TEST(TimestampPrecision, WholeMillisecondsCountedInFinerTicksHaveMillisecondPrecision)
{
    // MKV or WebM remuxed to MP4 (1/16000 s) or to an MPEG transport stream (1/90000 s) without re-encoding: every
    // timestamp is a whole millisecond, so the rounding of the original 1 ms clock is all the timing they carry.
    struct Rate
    {
        std::int64_t numerator;
        std::int64_t denominator;
    };
    for (const auto& [numerator, denominator] : {Rate{.numerator = 1, .denominator = 30},
                                                 Rate{.numerator = 1001, .denominator = 30000},
                                                 Rate{.numerator = 1, .denominator = 60},
                                                 Rate{.numerator = 1, .denominator = 25}})
    {
        const auto milliseconds = wholeMilliseconds(31, numerator, denominator);
        EXPECT_EQ(science::timestampPrecision(MP4_REMUX, remuxed(milliseconds, 16)), science::ONE_MILLISECOND) << denominator;
        EXPECT_EQ(science::timestampPrecision(TRANSPORT_STREAM, remuxed(milliseconds, 90)), science::ONE_MILLISECOND) << denominator;
        // Offsets count from the first timestamp: a start off the millisecond lattice, negative times (a decoder delay)
        // and timestamps at the ends of the 64-bit range change nothing, and neither does the order they arrive in.
        for (const auto start : {std::int64_t{7},
                                 std::int64_t{-1063},
                                 std::numeric_limits<std::int64_t>::min(),
                                 std::numeric_limits<std::int64_t>::max() - 16 * milliseconds.back()})
        {
            EXPECT_EQ(science::timestampPrecision(MP4_REMUX, remuxed(milliseconds, 16, start)), science::ONE_MILLISECOND) << start;
        }
        auto reversed = remuxed(milliseconds, 16, 5);
        std::ranges::reverse(reversed);
        EXPECT_EQ(science::timestampPrecision(MP4_REMUX, reversed), science::ONE_MILLISECOND);
    }
    // Where 1 ms is not a whole number of ticks (1/15360 s, 15.36 ticks), whole milliseconds are multiples of 25 ms
    // (384 ticks), as at 40 fps; the precision is still 1 ms. A tick numerator other than 1 works the same way.
    EXPECT_EQ(science::timestampPrecision({.numerator = 1, .denominator = 15360}, std::vector<std::int64_t>{0, 384, 768, 1152}),
              science::ONE_MILLISECOND);
    EXPECT_EQ(science::timestampPrecision({.numerator = 1, .denominator = 15360}, std::vector<std::int64_t>{0, 512, 1024, 1536}),
              (SecondsRatio{.numerator = 1, .denominator = 15360}));
    EXPECT_EQ(science::timestampPrecision({.numerator = 2, .denominator = 90000}, std::vector<std::int64_t>{0, 1485, 3015, 4500}),
              science::ONE_MILLISECOND);
}

TEST(TimestampPrecision, TimestampsThatUseTheFinerTicksKeepTheTimeBase)
{
    // Genuine timing on a fine time base: the declared tick stays the precision, so its variable frame rate is still
    // told from rounding exactly as with the time base alone.
    // A native 30 fps MP4 (533 or 534 ticks of 1/16000 s) and 29.97 fps in an MPEG transport stream (3003 ticks).
    EXPECT_EQ(science::timestampPrecision(MP4_REMUX, nativeTicks(31, 30.0, 16000.0)), MP4_REMUX);
    EXPECT_EQ(science::timestampPrecision(TRANSPORT_STREAM, nativeTicks(31, 30000.0 / 1001.0, 90000.0)), TRANSPORT_STREAM);
    // A camera clock in microseconds rounded to 1/90000 s, as MoTrack's capture writes it, with real jitter.
    std::vector<std::int64_t> camera;
    camera.reserve(31);
    for (std::int64_t frame = 0; frame < 31; ++frame)
    {
        const auto microseconds = 1'000'000 + frame * 33'333 + (frame * 7919) % 1500 - 750;
        camera.push_back(std::llround(static_cast<double>(microseconds) * 0.09));
    }
    EXPECT_EQ(science::timestampPrecision(TRANSPORT_STREAM, camera), TRANSPORT_STREAM);
    // One timestamp a single tick off the millisecond lattice, wherever it is, keeps the time base for the movie.
    const auto milliseconds = remuxed(wholeMilliseconds(31, 1, 30), 16);
    for (const std::size_t frame : {std::size_t{0}, std::size_t{15}, milliseconds.size() - 1})
    {
        auto moved = milliseconds;
        moved[frame] += 1;
        EXPECT_EQ(science::timestampPrecision(MP4_REMUX, moved), MP4_REMUX) << frame;
    }
    // Two cases where the greatest common divisor of the offsets would coarsen the precision and hide real timing:
    // a rate change from 3000 to 3060 ticks (33.3 to 34 ms; the divisor is 60 ticks), and a constant 1001/30000 s
    // rate whose divisor is the frame interval itself. Neither offset sequence is whole milliseconds.
    std::vector<std::int64_t> change;
    change.reserve(31);
    for (std::int64_t frame = 0; frame < 31; ++frame)
    {
        change.push_back(frame <= 15 ? 3000 * frame : std::int64_t{3000} * 15 + 3060 * (frame - 15));
    }
    EXPECT_EQ(science::timestampPrecision(TRANSPORT_STREAM, change), TRANSPORT_STREAM);
    constexpr SecondsRatio NTSC{.numerator = 1, .denominator = 30000};
    EXPECT_EQ(science::timestampPrecision(NTSC, nativeTicks(31, 30000.0 / 1001.0, 30000.0)), NTSC);
}

TEST(TimestampPrecision, IsNeverCoarserThanTheTimeBaseOrOneMillisecond)
{
    // 25 fps in an MPEG transport stream: 3600 ticks, exactly 40 ms per frame, with a dropped frame (80 ms). The
    // offsets share 40 ms, but the precision is 1 ms, so the dropped frame still makes its windows uneven.
    std::vector<std::int64_t> ticks;
    ticks.reserve(12);
    for (std::int64_t frame = 0; frame < 12; ++frame)
    {
        ticks.push_back(3600 * (frame < 6 ? frame : frame + 1));
    }
    const auto precision = science::timestampPrecision(TRANSPORT_STREAM, ticks);
    ASSERT_EQ(precision, science::ONE_MILLISECOND);
    std::vector<double> seconds;
    seconds.reserve(ticks.size());
    for (const auto tick : ticks)
    {
        seconds.push_back(static_cast<double>(tick) / 90000.0);
    }
    const double resolution = static_cast<double>(precision.numerator) / static_cast<double>(precision.denominator);
    for (std::size_t step = 2; step + 2 < seconds.size(); ++step)
    {
        const bool holdsTheDrop = step >= 4 && step <= 7; // Windows spanning the 80 ms interval between frames 5 and 6.
        EXPECT_EQ(science::evenlySpacedWindow(seconds, step, 2, resolution), !holdsTheDrop) << step;
    }
    // A tick of 1 ms or more is the precision whatever the timestamps (max(time base, 1 ms)): 1/1000 s, and 1/600 s
    // with offsets that happen to be whole milliseconds (multiples of 3 ticks, 5 ms).
    constexpr SecondsRatio MATROSKA{.numerator = 1, .denominator = 1000};
    constexpr SecondsRatio PHONE{.numerator = 1, .denominator = 600};
    EXPECT_EQ(science::timestampPrecision(MATROSKA, wholeMilliseconds(31, 1, 30)), MATROSKA);
    EXPECT_EQ(science::timestampPrecision(PHONE, std::vector<std::int64_t>{0, 21, 39, 60, 81}), PHONE);
    // #334's mixed clip: 30 fps on 1/600 s with one frame up to three ticks late keeps its time base.
    for (const std::int64_t late : {1, 2, 3})
    {
        std::vector<std::int64_t> mixed;
        mixed.reserve(21);
        for (std::int64_t frame = 0; frame < 21; ++frame)
        {
            mixed.push_back(20 * frame + (frame == 10 ? late : 0));
        }
        EXPECT_EQ(science::timestampPrecision(PHONE, mixed), PHONE) << late;
    }
}

TEST(TimestampPrecision, NeedsTwoTimestampsAndAValidTimeBase)
{
    // Without two different timestamps there is nothing to infer from, and an invalid time base is returned as it is.
    EXPECT_EQ(science::timestampPrecision(MP4_REMUX, {}), MP4_REMUX);
    EXPECT_EQ(science::timestampPrecision(MP4_REMUX, std::vector<std::int64_t>{160}), MP4_REMUX);
    EXPECT_EQ(science::timestampPrecision(MP4_REMUX, std::vector<std::int64_t>{160, 160}), MP4_REMUX);
    EXPECT_EQ(science::timestampPrecision(MP4_REMUX, std::vector<std::int64_t>{160, 160, 176}), science::ONE_MILLISECOND);
    for (const auto invalid : {SecondsRatio{},
                               SecondsRatio{.numerator = 1, .denominator = 0},
                               SecondsRatio{.numerator = -1, .denominator = 1000},
                               SecondsRatio{.numerator = 0, .denominator = 16000}})
    {
        EXPECT_FALSE(invalid.valid());
        EXPECT_EQ(science::timestampPrecision(invalid, std::vector<std::int64_t>{0, 16, 32}), invalid);
    }
    // The incremental form gives the same answer after each timestamp.
    science::TimestampPrecision precision(MP4_REMUX);
    EXPECT_EQ(precision.precision(), MP4_REMUX);
    precision.add(0);
    EXPECT_EQ(precision.precision(), MP4_REMUX);
    precision.add(528);
    EXPECT_EQ(precision.precision(), science::ONE_MILLISECOND);
    precision.add(1072);
    EXPECT_EQ(precision.precision(), science::ONE_MILLISECOND);
    precision.add(1601);
    EXPECT_EQ(precision.precision(), MP4_REMUX);
    precision.add(2128);
    EXPECT_EQ(precision.precision(), MP4_REMUX);
}

} // namespace
