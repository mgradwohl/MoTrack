#include "osp/java_text.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <limits>

namespace motrack::osp
{
namespace
{

constexpr std::array<std::uint32_t, 10> POWERS_OF_TEN{
    1, 10, 100, 1'000, 10'000, 100'000, 1'000'000, 10'000'000, 100'000'000, 1'000'000'000};

/// Unsigned integer with little-endian 32-bit limbs. Exact double conversion needs at most a few thousand bits.
class BigUint
{
  public:
    BigUint() = default;

    explicit BigUint(std::uint64_t value)
    {
        for (; value != 0; value >>= 32)
        {
            m_Limbs.push_back(static_cast<std::uint32_t>(value));
        }
    }

    [[nodiscard]] bool isZero() const noexcept
    {
        return m_Limbs.empty();
    }

    void multiplyAdd(std::uint32_t factor, std::uint32_t addend)
    {
        std::uint64_t carry = addend;
        for (auto& limb : m_Limbs)
        {
            const auto product = (std::uint64_t{limb} * factor) + carry;
            limb = static_cast<std::uint32_t>(product);
            carry = product >> 32;
        }
        if (carry != 0)
        {
            m_Limbs.push_back(static_cast<std::uint32_t>(carry));
        }
        trim();
    }

    void multiplyPowerOfTen(std::uint64_t exponent)
    {
        for (; exponent >= 9; exponent -= 9)
        {
            multiplyAdd(POWERS_OF_TEN[9], 0);
        }
        multiplyAdd(POWERS_OF_TEN[exponent], 0);
    }

    void add(const BigUint& other)
    {
        if (m_Limbs.size() < other.m_Limbs.size())
        {
            m_Limbs.resize(other.m_Limbs.size(), 0);
        }
        std::uint64_t carry = 0;
        for (std::size_t index = 0; index < m_Limbs.size(); ++index)
        {
            const auto sum = std::uint64_t{m_Limbs[index]} + (index < other.m_Limbs.size() ? other.m_Limbs[index] : 0U) + carry;
            m_Limbs[index] = static_cast<std::uint32_t>(sum);
            carry = sum >> 32;
        }
        if (carry != 0)
        {
            m_Limbs.push_back(static_cast<std::uint32_t>(carry));
        }
    }

    void shiftLeft(std::size_t bits)
    {
        if (isZero())
        {
            return;
        }
        if (const auto partial = bits % 32; partial != 0)
        {
            std::uint32_t carry = 0;
            for (auto& limb : m_Limbs)
            {
                const auto next = limb >> (32 - partial);
                limb = (limb << partial) | carry;
                carry = next;
            }
            if (carry != 0)
            {
                m_Limbs.push_back(carry);
            }
        }
        m_Limbs.insert(m_Limbs.begin(), bits / 32, 0);
    }

    /// floor(numerator / divisor) when that quotient is below 2^64 (Knuth's algorithm D, after Hacker's Delight
    /// divmnu). Both operands are first scaled by the same power of two; `numerator` becomes the scaled remainder.
    static std::uint64_t divide(BigUint& numerator, BigUint& divisor)
    {
        if (divisor.m_Limbs.size() == 1)
        {
            numerator.shiftLeft(32);
            divisor.shiftLeft(32);
        }
        const auto normalization = static_cast<std::size_t>(std::countl_zero(divisor.m_Limbs.back()));
        numerator.shiftLeft(normalization);
        divisor.shiftLeft(normalization);
        const auto& v = divisor.m_Limbs;
        auto& u = numerator.m_Limbs;
        const auto n = v.size();
        u.resize(n + 2, 0);
        constexpr std::uint64_t BASE = std::uint64_t{1} << 32;
        std::uint64_t quotient = 0;
        for (std::size_t j = 2; j-- > 0;)
        {
            const auto top = (std::uint64_t{u[j + n]} << 32) | u[j + n - 1];
            auto estimate = top / v[n - 1];
            auto rest = top % v[n - 1];
            while (estimate >= BASE || estimate * v[n - 2] > ((rest << 32) | u[j + n - 2]))
            {
                --estimate;
                rest += v[n - 1];
                if (rest >= BASE)
                {
                    break;
                }
            }
            std::int64_t borrow = 0;
            for (std::size_t index = 0; index < n; ++index)
            {
                const auto product = estimate * v[index];
                const auto difference = static_cast<std::int64_t>(u[index + j]) - borrow - static_cast<std::int64_t>(product & 0xFFFFFFFFU);
                u[index + j] = static_cast<std::uint32_t>(difference);
                borrow = static_cast<std::int64_t>(product >> 32) - (difference >> 32);
            }
            const auto difference = static_cast<std::int64_t>(u[j + n]) - borrow;
            u[j + n] = static_cast<std::uint32_t>(difference);
            if (difference < 0)
            {
                // The estimate was one too large: add the divisor back.
                --estimate;
                std::uint64_t carry = 0;
                for (std::size_t index = 0; index < n; ++index)
                {
                    const auto sum = std::uint64_t{u[index + j]} + v[index] + carry;
                    u[index + j] = static_cast<std::uint32_t>(sum);
                    carry = sum >> 32;
                }
                u[j + n] += static_cast<std::uint32_t>(carry);
            }
            quotient = (quotient << 32) | estimate;
        }
        numerator.trim();
        return quotient;
    }

    [[nodiscard]] std::size_t bitLength() const noexcept
    {
        return m_Limbs.empty() ? 0 : ((m_Limbs.size() - 1) * 32) + static_cast<std::size_t>(std::bit_width(m_Limbs.back()));
    }

    [[nodiscard]] int compare(const BigUint& other) const noexcept
    {
        if (m_Limbs.size() != other.m_Limbs.size())
        {
            return m_Limbs.size() < other.m_Limbs.size() ? -1 : 1;
        }
        for (auto index = m_Limbs.size(); index-- > 0;)
        {
            if (m_Limbs[index] != other.m_Limbs[index])
            {
                return m_Limbs[index] < other.m_Limbs[index] ? -1 : 1;
            }
        }
        return 0;
    }

    /// Requires `*this >= other`.
    void subtract(const BigUint& other)
    {
        std::uint64_t borrow = 0;
        for (std::size_t index = 0; index < m_Limbs.size(); ++index)
        {
            const std::uint64_t subtrahend = (index < other.m_Limbs.size() ? other.m_Limbs[index] : 0U) + borrow;
            borrow = subtrahend > m_Limbs[index] ? 1 : 0;
            m_Limbs[index] = static_cast<std::uint32_t>((std::uint64_t{m_Limbs[index]} + (borrow << 32)) - subtrahend);
        }
        trim();
    }

  private:
    void trim()
    {
        while (!m_Limbs.empty() && m_Limbs.back() == 0)
        {
            m_Limbs.pop_back();
        }
    }

    std::vector<std::uint32_t> m_Limbs;
};

constexpr bool isDigit(char value) noexcept
{
    return value >= '0' && value <= '9';
}

constexpr int hexValue(char value) noexcept
{
    if (isDigit(value))
    {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f')
    {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F')
    {
        return value - 'A' + 10;
    }
    return -1;
}

BigUint decimalInteger(std::string_view digits)
{
    BigUint value;
    for (std::size_t index = 0; index < digits.size(); index += 9)
    {
        const auto chunk = digits.substr(index, 9);
        std::uint32_t part = 0;
        for (const auto digit : chunk)
        {
            part = (part * 10) + static_cast<std::uint32_t>(digit - '0');
        }
        value.multiplyAdd(POWERS_OF_TEN[chunk.size()], part);
    }
    return value;
}

double withSign(bool negative, double magnitude) noexcept
{
    return negative ? -magnitude : magnitude;
}

/// Rounds `q * 2^(exponent - 63)` (bit 63 of `q` set; `sticky` marks discarded nonzero bits) to the nearest
/// double, ties to even, with IEEE overflow to infinity and gradual underflow.
double assemble(bool negative, std::uint64_t q, std::int64_t exponent, bool sticky) noexcept
{
    constexpr std::int64_t MAX_EXPONENT = 1023;
    constexpr std::int64_t MIN_NORMAL_EXPONENT = -1022;
    if (exponent > MAX_EXPONENT)
    {
        return withSign(negative, std::numeric_limits<double>::infinity());
    }
    if (exponent >= MIN_NORMAL_EXPONENT)
    {
        auto mantissa = q >> 11;
        const auto rest = q & 0x7FFU;
        if (rest > 0x400U || (rest == 0x400U && (sticky || (mantissa & 1U) != 0)))
        {
            ++mantissa;
        }
        if (mantissa == (std::uint64_t{1} << 53))
        {
            mantissa >>= 1;
            if (++exponent > MAX_EXPONENT)
            {
                return withSign(negative, std::numeric_limits<double>::infinity());
            }
        }
        return withSign(negative, std::ldexp(static_cast<double>(mantissa), static_cast<int>(exponent - 52)));
    }
    // Subnormal: count units of 2^-1074. The value is q * 2^(exponent - 63), so drop this many bits of q.
    const auto drop = 63 - 1074 - exponent;
    std::uint64_t units = 0;
    if (drop == 64)
    {
        units = (q > (std::uint64_t{1} << 63) || sticky) ? 1 : 0;
    }
    else if (drop < 64)
    {
        units = q >> drop;
        const auto low = q & ((std::uint64_t{1} << drop) - 1);
        const auto half = std::uint64_t{1} << (drop - 1);
        if (low > half || (low == half && (sticky || (units & 1U) != 0)))
        {
            ++units;
        }
    }
    return withSign(negative, std::ldexp(static_cast<double>(units), -1074));
}

/// Correctly rounded value of `digits * 10^exponent`. `digits` is a nonempty ASCII string without leading zeros.
double decimalToDouble(std::string_view digits, std::int64_t exponent, bool negative)
{
    // Keep 800 significant digits plus a sticky digit. A midpoint between adjacent doubles has at most 767
    // significant digits, so this cannot move the value across a rounding boundary.
    constexpr std::size_t MAX_DIGITS = 800;
    std::string kept;
    if (digits.size() > MAX_DIGITS)
    {
        kept = digits.substr(0, MAX_DIGITS);
        const auto nonzero = digits.substr(MAX_DIGITS).find_first_not_of('0') != std::string_view::npos;
        exponent += static_cast<std::int64_t>(digits.size() - MAX_DIGITS);
        if (nonzero)
        {
            kept.push_back('1');
            --exponent;
        }
        digits = kept;
    }
    const auto magnitude = static_cast<std::int64_t>(digits.size()) + exponent; // value < 10^magnitude
    if (magnitude > 310)
    {
        return withSign(negative, std::numeric_limits<double>::infinity());
    }
    if (magnitude <= -324)
    {
        return withSign(negative, 0.0);
    }
    if (digits.size() <= 15 && exponent >= -22 && exponent <= 22)
    {
        // Clinger's fast path: both operands are exact, so one IEEE operation rounds correctly.
        static constexpr std::array<double, 23> EXACT_POWERS{1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
                                                             1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
        std::uint64_t integer = 0;
        for (const auto digit : digits)
        {
            integer = (integer * 10) + static_cast<std::uint64_t>(digit - '0');
        }
        const auto value = static_cast<double>(integer);
        const auto power = EXACT_POWERS[static_cast<std::size_t>(exponent < 0 ? -exponent : exponent)];
        return withSign(negative, exponent < 0 ? value / power : value * power);
    }
    auto numerator = decimalInteger(digits);
    BigUint denominator(1);
    if (exponent >= 0)
    {
        numerator.multiplyPowerOfTen(static_cast<std::uint64_t>(exponent));
    }
    else
    {
        denominator.multiplyPowerOfTen(static_cast<std::uint64_t>(-exponent));
    }
    // numerator/denominator lies in (2^(d-1), 2^(d+1)); scale it into (2^62, 2^64).
    const auto difference = static_cast<std::int64_t>(numerator.bitLength()) - static_cast<std::int64_t>(denominator.bitLength());
    const auto shift = 63 - difference;
    if (shift >= 0)
    {
        numerator.shiftLeft(static_cast<std::size_t>(shift));
    }
    else
    {
        denominator.shiftLeft(static_cast<std::size_t>(-shift));
    }
    auto q = BigUint::divide(numerator, denominator);
    auto leading = 63 - shift;
    if ((q >> 63) == 0)
    {
        numerator.shiftLeft(1);
        const bool next = numerator.compare(denominator) >= 0;
        if (next)
        {
            numerator.subtract(denominator);
        }
        q = (q << 1) | (next ? 1U : 0U);
        --leading;
    }
    return assemble(negative, q, leading, !numerator.isZero());
}

/// Java's hexadecimal floating-point grammar after "0x": hex digits with an optional point, a mandatory binary
/// exponent and an optional type suffix (FloatingDecimal.parseHexString).
Expected<double, DecodeError> parseHexadecimal(std::string_view body, bool negative)
{
    std::string hexDigits;
    std::size_t index = 0;
    std::int64_t pointPosition = 0;
    for (; index < body.size() && hexValue(body[index]) >= 0; ++index)
    {
        hexDigits.push_back(body[index]);
        ++pointPosition;
    }
    if (index < body.size() && body[index] == '.')
    {
        for (++index; index < body.size() && hexValue(body[index]) >= 0; ++index)
        {
            hexDigits.push_back(body[index]);
        }
    }
    if (hexDigits.empty() || index == body.size() || (body[index] != 'p' && body[index] != 'P'))
    {
        return makeUnexpected(DecodeError::Malformed);
    }
    ++index;
    bool negativeExponent = false;
    if (index < body.size() && (body[index] == '+' || body[index] == '-'))
    {
        negativeExponent = body[index] == '-';
        ++index;
    }
    if (index == body.size() || !isDigit(body[index]))
    {
        return makeUnexpected(DecodeError::Malformed);
    }
    std::int64_t binaryExponent = 0;
    for (; index < body.size() && isDigit(body[index]); ++index)
    {
        binaryExponent = std::min<std::int64_t>((binaryExponent * 10) + (body[index] - '0'), 1'000'000'000);
    }
    if (index < body.size() &&
        (index + 1 != body.size() || (body[index] != 'f' && body[index] != 'F' && body[index] != 'd' && body[index] != 'D')))
    {
        return makeUnexpected(DecodeError::Malformed);
    }
    const auto first = hexDigits.find_first_not_of('0');
    if (first == std::string::npos)
    {
        return withSign(negative, 0.0);
    }
    pointPosition -= static_cast<std::int64_t>(first);
    const std::string_view significant = std::string_view(hexDigits).substr(first);
    const auto used = std::min<std::size_t>(significant.size(), 16);
    std::uint64_t accumulator = 0;
    for (std::size_t digit = 0; digit < used; ++digit)
    {
        accumulator = (accumulator << 4) | static_cast<std::uint64_t>(hexValue(significant[digit]));
    }
    const bool sticky = significant.substr(used).find_first_not_of('0') != std::string_view::npos;
    const auto leadingZeros = std::countl_zero(accumulator);
    const auto exponent =
        63 - leadingZeros + (4 * (pointPosition - static_cast<std::int64_t>(used))) + (negativeExponent ? -binaryExponent : binaryExponent);
    if (exponent > 2000)
    {
        return withSign(negative, std::numeric_limits<double>::infinity());
    }
    if (exponent < -2000)
    {
        return withSign(negative, 0.0);
    }
    return assemble(negative, accumulator << leadingZeros, exponent, sticky);
}

struct Decimal
{
    std::string digits{};      ///< Significant digits without leading or trailing zeros.
    std::int64_t exponent = 0; ///< The value is digits * 10^exponent.
};

/// A finite positive double as Burger and Dybvig's scaled integers: the value is r/s and its rounding interval
/// extends m-/s below and m+/s above it.
struct Interval
{
    BigUint r{};
    BigUint s{};
    BigUint upper{};
    BigUint lower{};
    bool inclusive = false; ///< Round-half-even maps both boundaries of an even significand to it.
};

Interval interval(double value)
{
    const auto bits = std::bit_cast<std::uint64_t>(value);
    const auto biased = static_cast<int>((bits >> 52) & 0x7FFU);
    auto significand = bits & ((std::uint64_t{1} << 52) - 1);
    int exponent = -1074;
    if (biased != 0)
    {
        significand |= std::uint64_t{1} << 52;
        exponent = biased - 1075;
    }
    // Above a power of two the gap is twice the gap below, except at the smallest normal exponent.
    const bool unequal = significand == (std::uint64_t{1} << 52) && biased > 1;
    const std::size_t extra = unequal ? 2 : 1;
    Interval result{.r = BigUint(significand),
                    .s = BigUint(1),
                    .upper = BigUint(unequal ? 2 : 1),
                    .lower = BigUint(1),
                    .inclusive = (significand & 1U) == 0};
    result.r.shiftLeft(extra);
    result.s.shiftLeft(extra);
    if (exponent >= 0)
    {
        result.r.shiftLeft(static_cast<std::size_t>(exponent));
        result.upper.shiftLeft(static_cast<std::size_t>(exponent));
        result.lower.shiftLeft(static_cast<std::size_t>(exponent));
    }
    else
    {
        result.s.shiftLeft(static_cast<std::size_t>(-exponent));
    }
    return result;
}

Decimal decimal(std::vector<int> digits, std::int64_t exponent)
{
    for (auto position = digits.size() - 1; position > 0 && digits[position] == 10; --position)
    {
        digits[position] = 0;
        ++digits[position - 1];
    }
    if (digits.front() == 10)
    {
        digits.front() = 1;
        digits.insert(digits.begin() + 1, 0);
    }
    Decimal result{.exponent = exponent};
    for (const auto digit : digits)
    {
        result.digits.push_back(static_cast<char>('0' + digit));
    }
    result.digits.erase(0, std::min(result.digits.find_first_not_of('0'), result.digits.size() - 1));
    while (result.digits.size() > 1 && result.digits.back() == '0')
    {
        result.digits.pop_back();
        ++result.exponent;
    }
    return result;
}

/// The shortest decimal in the rounding interval of a finite positive double, the closest one when several
/// have that length and the even one on an exact tie (Steele and White's free-format algorithm as refined by
/// Burger and Dybvig). This is the digit selection of Java's Double.toString since JDK 19.
Decimal shortestDecimal(double value)
{
    auto [r, s, upper, lower, inclusive] = interval(value);
    // ceil(log10(value)) can only be one too small after the offset; the loop below corrects that.
    auto k = static_cast<std::int64_t>(std::ceil(std::log10(value) - 1e-10));
    if (k >= 0)
    {
        s.multiplyPowerOfTen(static_cast<std::uint64_t>(k));
    }
    else
    {
        r.multiplyPowerOfTen(static_cast<std::uint64_t>(-k));
        upper.multiplyPowerOfTen(static_cast<std::uint64_t>(-k));
        lower.multiplyPowerOfTen(static_cast<std::uint64_t>(-k));
    }
    const auto reachesHigh = [&]
    {
        auto sum = r;
        sum.add(upper);
        const auto order = sum.compare(s);
        return inclusive ? order >= 0 : order > 0;
    };
    while (reachesHigh())
    {
        s.multiplyAdd(10, 0);
        ++k;
    }
    std::vector<int> digits;
    for (;;)
    {
        r.multiplyAdd(10, 0);
        upper.multiplyAdd(10, 0);
        lower.multiplyAdd(10, 0);
        int digit = 0;
        while (r.compare(s) >= 0)
        {
            r.subtract(s);
            ++digit;
        }
        const auto lowOrder = r.compare(lower);
        const bool low = inclusive ? lowOrder <= 0 : lowOrder < 0;
        const bool high = reachesHigh();
        if (!low && !high)
        {
            digits.push_back(digit);
            continue;
        }
        if (low && high)
        {
            auto twice = r;
            twice.shiftLeft(1);
            const auto order = twice.compare(s);
            if (order > 0 || (order == 0 && digit % 2 != 0))
            {
                ++digit;
            }
        }
        else if (high)
        {
            ++digit;
        }
        digits.push_back(digit);
        break;
    }
    const auto exponent = k - static_cast<std::int64_t>(digits.size());
    return decimal(std::move(digits), exponent);
}

/// A finite positive double correctly rounded (ties to even) to `precision` significant digits.
Decimal closestDecimal(double value, std::int64_t precision)
{
    auto [r, s, upper, lower, inclusive] = interval(value);
    auto k = static_cast<std::int64_t>(std::floor(std::log10(value))) + 1;
    const auto shift = precision - k;
    if (shift >= 0)
    {
        r.multiplyPowerOfTen(static_cast<std::uint64_t>(shift));
    }
    else
    {
        s.multiplyPowerOfTen(static_cast<std::uint64_t>(-shift));
    }
    // Correct an estimate that is one off: 10^(precision - 1) <= r/s < 10^precision.
    auto limit = s;
    limit.multiplyPowerOfTen(static_cast<std::uint64_t>(precision));
    if (r.compare(limit) >= 0)
    {
        s.multiplyAdd(10, 0);
        ++k;
    }
    auto floor = s;
    floor.multiplyPowerOfTen(static_cast<std::uint64_t>(precision - 1));
    if (r.compare(floor) < 0)
    {
        r.multiplyAdd(10, 0);
        --k;
    }
    std::int64_t quotient = 0;
    while (r.compare(s) >= 0)
    {
        r.subtract(s);
        ++quotient;
    }
    r.shiftLeft(1);
    const auto order = r.compare(s);
    if (order > 0 || (order == 0 && quotient % 2 != 0))
    {
        ++quotient;
    }
    std::vector<int> digits;
    for (const auto character : std::to_string(quotient))
    {
        digits.push_back(character - '0');
    }
    return decimal(std::move(digits), k - precision);
}

} // namespace

Expected<std::int32_t, DecodeError> parseJavaInt(std::string_view text)
{
    if (text.empty())
    {
        return makeUnexpected(DecodeError::Malformed);
    }
    std::size_t index = 0;
    const bool negative = text.front() == '-';
    if (negative || text.front() == '+')
    {
        if (text.size() == 1)
        {
            return makeUnexpected(DecodeError::Malformed);
        }
        index = 1;
    }
    constexpr std::int64_t LIMIT = std::int64_t{1} << 31;
    std::int64_t value = 0;
    for (; index < text.size(); ++index)
    {
        const auto digit = text[index];
        if (!isDigit(digit))
        {
            // Java also accepts other Unicode decimal digits; MoTrack only decodes ASCII digits.
            return makeUnexpected(static_cast<unsigned char>(digit) >= 0x80 ? DecodeError::Unsupported : DecodeError::Malformed);
        }
        value = (value * 10) + (digit - '0');
        if (value > LIMIT)
        {
            return makeUnexpected(DecodeError::OutOfRange);
        }
    }
    if (!negative && value == LIMIT)
    {
        return makeUnexpected(DecodeError::OutOfRange);
    }
    return static_cast<std::int32_t>(negative ? -value : value);
}

Expected<double, DecodeError> parseJavaDouble(std::string_view text)
{
    // FloatingDecimal.readJavaFormatString trims, then reads an optional sign.
    const auto trimmed = javaTrim(text);
    if (trimmed.empty())
    {
        return makeUnexpected(DecodeError::Malformed);
    }
    const bool negative = trimmed.front() == '-';
    const auto body = (negative || trimmed.front() == '+') ? trimmed.substr(1) : trimmed;
    if (body.empty())
    {
        return makeUnexpected(DecodeError::Malformed);
    }
    if (body.front() == 'N')
    {
        if (body == "NaN")
        {
            return std::numeric_limits<double>::quiet_NaN();
        }
        return makeUnexpected(DecodeError::Malformed);
    }
    if (body.front() == 'I')
    {
        if (body == "Infinity")
        {
            return withSign(negative, std::numeric_limits<double>::infinity());
        }
        return makeUnexpected(DecodeError::Malformed);
    }
    if (body.size() > 1 && body[0] == '0' && (body[1] == 'x' || body[1] == 'X'))
    {
        return parseHexadecimal(body.substr(2), negative);
    }
    std::string digits;
    std::int64_t exponent = 0;
    std::size_t index = 0;
    for (; index < body.size() && isDigit(body[index]); ++index)
    {
        digits.push_back(body[index]);
    }
    if (index < body.size() && body[index] == '.')
    {
        for (++index; index < body.size() && isDigit(body[index]); ++index)
        {
            digits.push_back(body[index]);
            --exponent;
        }
    }
    if (digits.empty())
    {
        return makeUnexpected(DecodeError::Malformed);
    }
    if (index < body.size() && (body[index] == 'e' || body[index] == 'E'))
    {
        ++index;
        bool negativeExponent = false;
        if (index < body.size() && (body[index] == '+' || body[index] == '-'))
        {
            negativeExponent = body[index] == '-';
            ++index;
        }
        if (index == body.size() || !isDigit(body[index]))
        {
            return makeUnexpected(DecodeError::Malformed);
        }
        std::int64_t written = 0;
        for (; index < body.size() && isDigit(body[index]); ++index)
        {
            written = std::min<std::int64_t>((written * 10) + (body[index] - '0'), 1'000'000'000);
        }
        exponent += negativeExponent ? -written : written;
    }
    if (index < body.size() &&
        (index + 1 != body.size() || (body[index] != 'f' && body[index] != 'F' && body[index] != 'd' && body[index] != 'D')))
    {
        return makeUnexpected(DecodeError::Malformed);
    }
    const auto first = digits.find_first_not_of('0');
    if (first == std::string::npos)
    {
        return withSign(negative, 0.0);
    }
    return decimalToDouble(std::string_view(digits).substr(first), exponent, negative);
}

bool parseJavaBoolean(std::string_view text) noexcept
{
    constexpr std::string_view TRUE_TEXT = "true";
    return text.size() == TRUE_TEXT.size() &&
           std::ranges::equal(
               text, TRUE_TEXT, [](char left, char right) { return (left >= 'A' && left <= 'Z' ? left + ('a' - 'A') : left) == right; });
}

std::string formatJavaInt(std::int32_t value)
{
    return std::to_string(value);
}

std::string formatJavaDouble(double value)
{
    if (std::isnan(value))
    {
        return "NaN";
    }
    if (std::isinf(value))
    {
        return value > 0 ? "Infinity" : "-Infinity";
    }
    const bool negative = std::signbit(value);
    if (value == 0)
    {
        return negative ? "-0.0" : "0.0";
    }
    const auto magnitude = std::fabs(value);
    auto chosen = shortestDecimal(magnitude);
    if (chosen.digits.size() == 1)
    {
        // JDK 19+ chooses the closest two-digit decimal when one digit would suffice (4.9E-324, not 5.0E-324).
        chosen = closestDecimal(magnitude, 2);
    }
    const auto& digits = chosen.digits;
    const auto leading = chosen.exponent + static_cast<std::int64_t>(digits.size()) - 1;
    std::string output = negative ? "-" : "";
    if (leading >= -3 && leading < 7)
    {
        if (leading >= 0)
        {
            const auto integerLength = static_cast<std::size_t>(leading) + 1;
            if (digits.size() <= integerLength)
            {
                output += digits + std::string(integerLength - digits.size(), '0') + ".0";
            }
            else
            {
                output += digits.substr(0, integerLength) + "." + digits.substr(integerLength);
            }
        }
        else
        {
            output += "0." + std::string(static_cast<std::size_t>(-leading - 1), '0') + digits;
        }
        return output;
    }
    output += digits.substr(0, 1) + "." + (digits.size() > 1 ? digits.substr(1) : "0") + "E" + std::to_string(leading);
    return output;
}

std::string_view javaTrim(std::string_view text) noexcept
{
    const auto blank = [](char value)
    {
        return static_cast<unsigned char>(value) <= 0x20;
    };
    while (!text.empty() && blank(text.front()))
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && blank(text.back()))
    {
        text.remove_suffix(1);
    }
    return text;
}

std::vector<std::string_view> javaSplitCommas(std::string_view text)
{
    std::vector<std::string_view> fields;
    if (text.find(',') == std::string_view::npos)
    {
        fields.push_back(text);
        return fields;
    }
    for (std::size_t start = 0;;)
    {
        const auto comma = text.find(',', start);
        fields.push_back(text.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start));
        if (comma == std::string_view::npos)
        {
            break;
        }
        start = comma + 1;
    }
    while (!fields.empty() && fields.back().empty())
    {
        fields.pop_back();
    }
    return fields;
}

} // namespace motrack::osp
