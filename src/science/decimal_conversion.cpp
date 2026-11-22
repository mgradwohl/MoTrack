#include "science/decimal_conversion.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace motrack::science
{
namespace
{

// Digits beyond this count can only break a tie, so a nonzero tail becomes one sticky digit. A midpoint
// between adjacent doubles has at most 767 significant digits and therefore lies on the kept-digit grid.
constexpr std::size_t MAX_SIGNIFICANT_DIGITS = 800;
// A significand with n digits and decimal exponent e lies in [10^(n+e-1), 10^(n+e)).
constexpr long long OVERFLOW_MAGNITUDE = 310;   // 10^309 exceeds the largest double.
constexpr long long UNDERFLOW_MAGNITUDE = -325; // 10^-326 is below half the smallest subnormal.
constexpr long long EXPONENT_SATURATION = 1'000'000'000;
// 10^(801 + 325) needs 3741 bits; scaling for a 54-bit quotient adds at most 56 more.
constexpr std::size_t LIMB_COUNT = 128;
constexpr int QUOTIENT_BITS = 54;            // 53 significand bits and one rounding bit.
constexpr int MIN_QUOTIENT_EXPONENT = -1075; // Units of half the smallest subnormal.
constexpr std::uint32_t BILLION = 1'000'000'000;

/// Fixed-capacity unsigned integer; the bounds above keep every intermediate value within LIMB_COUNT.
class BigUnsigned
{
  public:
    void multiplyAdd(std::uint32_t factor, std::uint32_t addend) noexcept
    {
        std::uint64_t carry = addend;
        for (std::size_t i = 0; i < m_Size; ++i)
        {
            const auto product = (static_cast<std::uint64_t>(m_Limbs[i]) * factor) + carry;
            m_Limbs[i] = static_cast<std::uint32_t>(product);
            carry = product >> 32U;
        }
        if (carry != 0)
        {
            m_Limbs[m_Size++] = static_cast<std::uint32_t>(carry);
        }
    }

    void multiplyByPowerOfTen(long long exponent) noexcept
    {
        for (; exponent >= 9; exponent -= 9)
        {
            multiplyAdd(BILLION, 0);
        }
        std::uint32_t factor = 1;
        for (; exponent > 0; --exponent)
        {
            factor *= 10;
        }
        multiplyAdd(factor, 0);
    }

    void shiftLeft(std::size_t bits) noexcept
    {
        const std::size_t limbs = bits / 32;
        const auto offset = static_cast<unsigned>(bits % 32);
        m_Limbs[m_Size + limbs] = 0;
        for (std::size_t i = m_Size; i-- > 0;)
        {
            const std::uint64_t wide = static_cast<std::uint64_t>(m_Limbs[i]) << offset;
            m_Limbs[i + limbs + 1] |= static_cast<std::uint32_t>(wide >> 32U);
            m_Limbs[i + limbs] = static_cast<std::uint32_t>(wide);
        }
        for (std::size_t i = 0; i < limbs; ++i)
        {
            m_Limbs[i] = 0;
        }
        m_Size += limbs + 1;
        trim();
    }

    void shiftRightOne() noexcept
    {
        for (std::size_t i = 0; i < m_Size; ++i)
        {
            const std::uint32_t high = i + 1 < m_Size ? m_Limbs[i + 1] : 0;
            m_Limbs[i] = (m_Limbs[i] >> 1U) | (high << 31U);
        }
        trim();
    }

    void subtract(const BigUnsigned& other) noexcept
    {
        std::uint64_t borrow = 0;
        for (std::size_t i = 0; i < m_Size; ++i)
        {
            const std::uint64_t subtrahend = (i < other.m_Size ? other.m_Limbs[i] : 0) + borrow;
            borrow = m_Limbs[i] < subtrahend ? 1 : 0;
            m_Limbs[i] = static_cast<std::uint32_t>((borrow << 32U) + m_Limbs[i] - subtrahend);
        }
        trim();
    }

    [[nodiscard]] int compare(const BigUnsigned& other) const noexcept
    {
        if (m_Size != other.m_Size)
        {
            return m_Size < other.m_Size ? -1 : 1;
        }
        for (std::size_t i = m_Size; i-- > 0;)
        {
            if (m_Limbs[i] != other.m_Limbs[i])
            {
                return m_Limbs[i] < other.m_Limbs[i] ? -1 : 1;
            }
        }
        return 0;
    }

    [[nodiscard]] long long bitLength() const noexcept
    {
        return m_Size == 0 ? 0 : static_cast<long long>(((m_Size - 1) * 32) + std::bit_width(m_Limbs[m_Size - 1]));
    }

    [[nodiscard]] bool isZero() const noexcept
    {
        return m_Size == 0;
    }

  private:
    void trim() noexcept
    {
        while (m_Size > 0 && m_Limbs[m_Size - 1] == 0)
        {
            --m_Size;
        }
    }

    std::array<std::uint32_t, LIMB_COUNT> m_Limbs{};
    std::size_t m_Size = 0;
};

bool isDigit(char c) noexcept
{
    return c >= '0' && c <= '9';
}

struct Decimal
{
    std::array<char, MAX_SIGNIFICANT_DIGITS + 1> digits{};
    std::size_t count = 0;
    long long exponent = 0; ///< value = digits * 10^exponent
    bool sticky = false;

    void append(char digit, bool fraction) noexcept
    {
        if (count == 0 && digit == '0')
        {
            exponent -= fraction ? 1 : 0;
        }
        else if (count < MAX_SIGNIFICANT_DIGITS)
        {
            digits[count++] = digit;
            exponent -= fraction ? 1 : 0;
        }
        else
        {
            sticky = sticky || digit != '0';
            exponent += fraction ? 0 : 1;
        }
    }
};

/// Rounds digits * 10^exponent to the nearest double, ties to even.
double nearest(const Decimal& decimal) noexcept
{
    BigUnsigned numerator;
    for (std::size_t i = 0; i < decimal.count; ++i)
    {
        numerator.multiplyAdd(10, static_cast<std::uint32_t>(decimal.digits[i] - '0'));
    }
    BigUnsigned denominator;
    denominator.multiplyAdd(1, 1);
    if (decimal.exponent > 0)
    {
        numerator.multiplyByPowerOfTen(decimal.exponent);
    }
    else
    {
        denominator.multiplyByPowerOfTen(-decimal.exponent);
    }
    // Scale so the quotient has 54 or 55 bits, then take it bit by bit with the remainder as sticky.
    long long binaryExponent = numerator.bitLength() - denominator.bitLength() - QUOTIENT_BITS;
    if (binaryExponent >= 0)
    {
        denominator.shiftLeft(static_cast<std::size_t>(binaryExponent));
    }
    else
    {
        numerator.shiftLeft(static_cast<std::size_t>(-binaryExponent));
    }
    BigUnsigned divisor = denominator;
    divisor.shiftLeft(QUOTIENT_BITS + 1);
    std::uint64_t quotient = 0;
    for (int bit = QUOTIENT_BITS + 1; bit >= 0; --bit)
    {
        if (numerator.compare(divisor) >= 0)
        {
            numerator.subtract(divisor);
            quotient |= std::uint64_t{1} << static_cast<unsigned>(bit);
        }
        divisor.shiftRightOne();
    }
    bool sticky = !numerator.isZero();
    if (quotient >> static_cast<unsigned>(QUOTIENT_BITS) != 0)
    {
        sticky = sticky || (quotient & 1U) != 0;
        quotient >>= 1U;
        ++binaryExponent;
    }
    if (binaryExponent < MIN_QUOTIENT_EXPONENT)
    {
        // Subnormal: drop the bits below half the smallest subnormal. Values below 10^-326 never get
        // here, so the shift stays below 64.
        const auto shift = static_cast<unsigned>(MIN_QUOTIENT_EXPONENT - binaryExponent);
        sticky = sticky || (quotient & ((std::uint64_t{1} << shift) - 1)) != 0;
        quotient >>= shift;
        binaryExponent = MIN_QUOTIENT_EXPONENT;
    }
    std::uint64_t significand = quotient >> 1U;
    if ((quotient & 1U) != 0 && (sticky || (significand & 1U) != 0))
    {
        ++significand;
    }
    // Exact: the significand has at most 53 bits and the scale is no finer than the smallest subnormal.
    return std::ldexp(static_cast<double>(significand), static_cast<int>(binaryExponent + 1));
}

} // namespace

std::optional<double> parseJavaDecimal(std::string_view text) noexcept
{
    std::size_t i = 0;
    bool negative = false;
    if (i < text.size() && (text[i] == '+' || text[i] == '-'))
    {
        negative = text[i] == '-';
        ++i;
    }
    Decimal decimal;
    bool anyDigit = false;
    for (; i < text.size() && isDigit(text[i]); ++i)
    {
        decimal.append(text[i], false);
        anyDigit = true;
    }
    if (i < text.size() && text[i] == '.')
    {
        for (++i; i < text.size() && isDigit(text[i]); ++i)
        {
            decimal.append(text[i], true);
            anyDigit = true;
        }
    }
    if (!anyDigit)
    {
        return std::nullopt;
    }
    long long exponent = 0;
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E'))
    {
        ++i;
        bool negativeExponent = false;
        if (i < text.size() && (text[i] == '+' || text[i] == '-'))
        {
            negativeExponent = text[i] == '-';
            ++i;
        }
        if (i == text.size() || !isDigit(text[i]))
        {
            return std::nullopt;
        }
        for (; i < text.size() && isDigit(text[i]); ++i)
        {
            if (exponent < EXPONENT_SATURATION)
            {
                exponent = (exponent * 10) + (text[i] - '0');
            }
        }
        exponent = negativeExponent ? -exponent : exponent;
    }
    if (i != text.size())
    {
        return std::nullopt;
    }
    if (decimal.sticky)
    {
        decimal.digits[decimal.count++] = '1';
        --decimal.exponent;
    }
    while (decimal.count > 0 && decimal.digits[decimal.count - 1] == '0')
    {
        --decimal.count;
        ++decimal.exponent;
    }
    const double zero = negative ? -0.0 : 0.0;
    if (decimal.count == 0)
    {
        return zero;
    }
    decimal.exponent += exponent;
    const auto magnitude = static_cast<long long>(decimal.count) + decimal.exponent;
    if (magnitude > OVERFLOW_MAGNITUDE)
    {
        return negative ? -HUGE_VAL : HUGE_VAL;
    }
    if (magnitude < UNDERFLOW_MAGNITUDE)
    {
        return zero;
    }
    const double value = nearest(decimal);
    return negative ? -value : value;
}

} // namespace motrack::science
