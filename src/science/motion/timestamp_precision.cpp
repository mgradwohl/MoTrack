#include "science/motion/timestamp_precision.hpp"

#include <numeric>

namespace motrack::science
{
namespace
{

constexpr std::int64_t MILLISECONDS_PER_SECOND = 1000;

/// ticks mod modulus in [0, modulus), for a positive modulus; never overflows.
std::int64_t residue(std::int64_t ticks, std::int64_t modulus) noexcept
{
    const auto remainder = ticks % modulus;
    return remainder < 0 ? remainder + modulus : remainder;
}

} // namespace

TimestampPrecision::TimestampPrecision(SecondsRatio timeBase) noexcept : m_TimeBase(timeBase)
{
    if (!timeBase.valid())
    {
        return;
    }
    // A tick is numerator / denominator s, so t ticks are a whole number of milliseconds when t × numerator × 1000 is
    // a multiple of the denominator. Dividing both by g = gcd(denominator, numerator × 1000) leaves coprime factors,
    // so that holds exactly when t is a multiple of denominator / g: 16 ticks of 1/16000 s, 90 of 1/90000 s, and
    // 384 (25 ms) of 1/15360 s, where 1 ms is not a whole number of ticks.
    const auto scaled = std::int64_t{timeBase.numerator} * MILLISECONDS_PER_SECOND;
    if (scaled < timeBase.denominator) // The tick is finer than 1 ms.
    {
        m_MillisecondTicks = timeBase.denominator / std::gcd(std::int64_t{timeBase.denominator}, scaled);
    }
}

void TimestampPrecision::add(std::int64_t ticks) noexcept
{
    if (!m_First)
    {
        m_First = ticks;
        return;
    }
    m_Advanced = m_Advanced || ticks != *m_First;
    // The offset from the first timestamp is whole milliseconds when both lie in the same residue class; comparing
    // residues avoids forming a difference that could overflow.
    if (m_MillisecondTicks > 0 && residue(ticks, m_MillisecondTicks) != residue(*m_First, m_MillisecondTicks))
    {
        m_WholeMilliseconds = false;
    }
}

SecondsRatio TimestampPrecision::precision() const noexcept
{
    return m_MillisecondTicks > 0 && m_Advanced && m_WholeMilliseconds ? ONE_MILLISECOND : m_TimeBase;
}

SecondsRatio timestampPrecision(SecondsRatio timeBase, std::span<const std::int64_t> ticks) noexcept
{
    TimestampPrecision precision(timeBase);
    for (const auto timestamp : ticks)
    {
        precision.add(timestamp);
    }
    return precision.precision();
}

} // namespace motrack::science
