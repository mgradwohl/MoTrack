#include "science/decimal_conversion.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>

namespace
{

using motrack::science::parseJavaDecimal;

std::uint64_t bits(double value)
{
    return std::bit_cast<std::uint64_t>(value);
}

std::uint64_t parsedBits(const std::string& text)
{
    const auto value = parseJavaDecimal(text);
    EXPECT_TRUE(value.has_value()) << text;
    return value ? bits(*value) : 0;
}

TEST(DecimalConversion, AcceptsJavaDecimalForms)
{
    EXPECT_EQ(parsedBits("0"), bits(0.0));
    EXPECT_EQ(parsedBits("-0"), bits(-0.0));
    EXPECT_EQ(parsedBits("-0.000e5"), bits(-0.0));
    EXPECT_EQ(parsedBits("+1.5"), bits(1.5));
    EXPECT_EQ(parsedBits("1."), bits(1.0));
    EXPECT_EQ(parsedBits(".5"), bits(0.5));
    EXPECT_EQ(parsedBits("0.e5"), bits(0.0));
    EXPECT_EQ(parsedBits("1e3"), bits(1000.0));
    EXPECT_EQ(parsedBits("1E-3"), bits(0.001));
    EXPECT_EQ(parsedBits("1.5e+2"), bits(150.0));
    EXPECT_EQ(parsedBits("000000.0000001"), bits(1e-7));
    EXPECT_EQ(parsedBits("0.1"), 0x3FB999999999999AULL);
    EXPECT_EQ(parsedBits("1e23"), 0x44B52D02C7E14AF6ULL);
    EXPECT_EQ(parsedBits("123456789012345678901234567890"), bits(1.2345678901234568e29));
}

TEST(DecimalConversion, RejectsTextOutsideTheDecimalSubset)
{
    for (const char* text : {"",   "+",  "-",    ".",        "-.",  "e5", ".e5", "1e",  "1e+", "1E-",   "1.2.3", "1x",
                             " 1", "1 ", "0x10", "Infinity", "NaN", "1d", "1f",  "--1", "+-1", "1e5.0", "1,5"})
    {
        EXPECT_FALSE(parseJavaDecimal(text).has_value()) << text;
    }
}

TEST(DecimalConversion, RoundsBoundariesToNearestEven)
{
    EXPECT_EQ(parsedBits("4.9e-324"), 1U);
    EXPECT_EQ(parsedBits("2.4703282292062327e-324"), 0U);
    EXPECT_EQ(parsedBits("2.4703282292062328e-324"), 1U);
    EXPECT_EQ(parsedBits("2.2250738585072011e-308"), 0x000FFFFFFFFFFFFFULL);
    EXPECT_EQ(parsedBits("2.2250738585072012e-308"), 0x0010000000000000ULL);
    EXPECT_EQ(parsedBits("1.7976931348623157e308"), 0x7FEFFFFFFFFFFFFFULL);
    EXPECT_EQ(parsedBits("1.7976931348623158e308"), 0x7FEFFFFFFFFFFFFFULL);
    EXPECT_EQ(parsedBits("1.7976931348623159e308"), bits(std::numeric_limits<double>::infinity()));
    EXPECT_EQ(parsedBits("9007199254740993"), bits(9007199254740992.0));
    EXPECT_EQ(parsedBits("9007199254740995"), bits(9007199254740996.0));
    EXPECT_EQ(parsedBits("9007199254740993.0000000000000000000000000001"), bits(9007199254740994.0));
}

TEST(DecimalConversion, HonorsDigitsBeyondTheKeptPrecision)
{
    const std::string tie = "9007199254740993";
    EXPECT_EQ(parsedBits(tie + std::string(900, '0') + "e-900"), bits(9007199254740992.0));
    EXPECT_EQ(parsedBits(tie + std::string(900, '0') + "1e-901"), bits(9007199254740994.0));
    EXPECT_EQ(parsedBits(tie + "." + std::string(900, '0') + "1"), bits(9007199254740994.0));
    EXPECT_EQ(parsedBits("1" + std::string(1000, '0') + "e-1000"), bits(1.0));
    EXPECT_EQ(parsedBits("0." + std::string(400, '0') + "1e400"), bits(0.1));
    EXPECT_EQ(parsedBits(std::string(330, '9') + "e-330"), bits(1.0));
}

TEST(DecimalConversion, SaturatesHugeExponents)
{
    EXPECT_EQ(parsedBits("1e400"), bits(std::numeric_limits<double>::infinity()));
    EXPECT_EQ(parsedBits("-1e400"), bits(-std::numeric_limits<double>::infinity()));
    EXPECT_EQ(parsedBits("1e-400"), bits(0.0));
    EXPECT_EQ(parsedBits("-1e-400"), bits(-0.0));
    EXPECT_EQ(parsedBits("1e99999999999999999999"), bits(std::numeric_limits<double>::infinity()));
    EXPECT_EQ(parsedBits("1e-99999999999999999999"), bits(0.0));
    EXPECT_EQ(parsedBits("0e99999999999999999999"), bits(0.0));
    EXPECT_EQ(parsedBits("0.000001e310"), bits(1e304));
}

TEST(DecimalConversion, RoundTripsSeventeenDigitText)
{
    std::uint64_t state = 20261121;
    for (int i = 0; i < 20000; ++i)
    {
        // SplitMix64: a fixed, platform-independent sample of significands and signs.
        state += 0x9E3779B97F4A7C15ULL;
        std::uint64_t mixed = (state ^ (state >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        mixed = (mixed ^ (mixed >> 27U)) * 0x94D049BB133111EBULL;
        const auto pattern = (mixed ^ (mixed >> 31U)) & ~(std::uint64_t{0x7FF} << 52U);
        // Cover subnormal, ordinary and extreme exponents without generating NaN or infinity.
        const auto exponent = static_cast<std::uint64_t>(i % 2047);
        const auto value = std::bit_cast<double>(pattern | (exponent << 52U));
        std::array<char, 64> text{};
        std::snprintf(text.data(), text.size(), "%.17g", value);
        std::string decimal(text.data());
        std::ranges::replace(decimal, ',', '.'); // Locale-independent: the converter only accepts '.'.
        ASSERT_EQ(parsedBits(decimal), bits(value)) << decimal;
    }
}

} // namespace
