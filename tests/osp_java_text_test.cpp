#include "osp/java_text.hpp"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

using motrack::osp::DecodeError;

std::uint64_t bits(double value)
{
    return std::isnan(value) ? std::uint64_t{0x7ff8000000000000} : std::bit_cast<std::uint64_t>(value);
}

/// SplitMix64: deterministic on every platform, unlike std::uniform_*_distribution.
class Random
{
  public:
    explicit Random(std::uint64_t seed) : m_State(seed)
    {}

    std::uint64_t next()
    {
        auto value = (m_State += 0x9E3779B97F4A7C15U);
        value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9U;
        value = (value ^ (value >> 27)) * 0x94D049BB133111EBU;
        return value ^ (value >> 31);
    }

    std::uint64_t below(std::uint64_t bound)
    {
        return next() % bound;
    }

  private:
    std::uint64_t m_State;
};

double finiteDouble(Random& random)
{
    for (;;)
    {
        const auto value = std::bit_cast<double>(random.next());
        if (std::isfinite(value))
        {
            return value;
        }
    }
}

TEST(OspJavaText, ParsesIntegersLikeIntegerParseInt)
{
    using Case = std::pair<std::string_view, std::int32_t>;
    for (const auto& [text, value] : {Case{"+5", 5},
                                      Case{"-0", 0},
                                      Case{"007", 7},
                                      Case{"2147483647", 2147483647},
                                      Case{"-2147483648", std::numeric_limits<std::int32_t>::min()}})
    {
        EXPECT_EQ(motrack::osp::parseJavaInt(text), value) << text;
    }
    for (const auto text : {"", "-", "+", " 5", "5 ", "1.0", "1e3", "0x10", "--1", "1_000"})
    {
        EXPECT_EQ(motrack::osp::parseJavaInt(text).error_or(DecodeError::Missing), DecodeError::Malformed) << text;
    }
    for (const auto text : {"2147483648", "-2147483649", "99999999999999999999"})
    {
        EXPECT_EQ(motrack::osp::parseJavaInt(text).error_or(DecodeError::Missing), DecodeError::OutOfRange) << text;
    }
    // Java accepts other Unicode decimal digits (U+0663 ARABIC-INDIC DIGIT THREE); MoTrack reports them.
    EXPECT_EQ(motrack::osp::parseJavaInt("\xD9\xA3").error_or(DecodeError::Missing), DecodeError::Unsupported);
}

TEST(OspJavaText, ParsesDoublesLikeDoubleParseDouble)
{
    // Expected bits recorded from Tracker 6.3.5's Java 17 runtime (tests/fixtures/osp/numbers.xml through
    // tools/reference/XmlControlDump.java).
    using Case = std::pair<std::string_view, std::uint64_t>;
    for (const auto& [text, expected] : {
             Case{" 1.5 ", 0x3ff8000000000000},
             Case{"1.5f", 0x3ff8000000000000},
             Case{"2d", 0x4000000000000000},
             Case{"1e3D", 0x408f400000000000},
             Case{"0x1.8p1", 0x4008000000000000},
             Case{"-0X.8P-1074", 0x8000000000000000},
             Case{"0x1.00000000000008000000001p0", 0x3ff0000000000001},
             Case{"0x1.0000000000000800p0", 0x3ff0000000000000},
             Case{"0x1.0000000000001800p0", 0x3ff0000000000002},
             Case{"0x0p99", 0x0000000000000000},
             Case{"0x1p-1080", 0x0000000000000000},
             Case{"0x1p2000", 0x7ff0000000000000},
             Case{"0x1p99999999999999999999", 0x7ff0000000000000},
             Case{"0x1p-1074", 0x0000000000000001},
             Case{"0x1.fffffffffffffp1023", 0x7fefffffffffffff},
             Case{"0x1.fffffffffffff8p1023", 0x7ff0000000000000},
             Case{"0x0.0000000000001p-1022", 0x0000000000000001},
             Case{"1e-400", 0x0000000000000000},
             Case{"-1e400", 0xfff0000000000000},
             Case{"1e99999999999999999999", 0x7ff0000000000000},
             Case{".5", 0x3fe0000000000000},
             Case{"5.", 0x4014000000000000},
             Case{"NaN", 0x7ff8000000000000},
             Case{"-NaN", 0x7ff8000000000000},
             Case{"-Infinity", 0xfff0000000000000},
             Case{"+Infinity", 0x7ff0000000000000},
             Case{"0.1000000000000000055511151231257827021181583404541015625", 0x3fb999999999999a},
             Case{"9007199254740993", 0x4340000000000000},
             Case{"9007199254740993.000000000000000000000000000001", 0x4340000000000001},
             Case{"9007199254740995", 0x4340000000000002},
             Case{"4.9e-324", 0x0000000000000001},
             Case{"2.4703282292062327e-324", 0x0000000000000000},
             Case{"2.4703282292062328e-324", 0x0000000000000001},
             Case{"1.7976931348623157e308", 0x7fefffffffffffff},
             Case{"1.7976931348623159e308", 0x7ff0000000000000},
             Case{"-0.0", 0x8000000000000000},
             Case{"000", 0x0000000000000000},
             Case{"33.333333333333336", 0x4040aaaaaaaaaaab},
             Case{"1E+2", 0x4059000000000000},
             Case{"\t\n 42\x01", 0x4045000000000000},
         })
    {
        const auto value = motrack::osp::parseJavaDouble(text);
        ASSERT_TRUE(value) << text;
        EXPECT_EQ(bits(*value), expected) << text;
    }
    for (const auto text : {"",   " ",   "-",  "+",   "1fd", "0x1.8",     "0x",   "0xp1", "0x1p",  "0x1p+", "0x1g", "0x1p1x",   ".",
                            "1e", "1e+", "e5", "nan", "inf", "Infinityf", "NaNd", "1,5",  "1.5.5", "1e5.5", "--1",  "0x1.8p1ff"})
    {
        EXPECT_EQ(motrack::osp::parseJavaDouble(text).error_or(DecodeError::Missing), DecodeError::Malformed) << text;
    }
}

TEST(OspJavaText, ParsesLongAndExtremeDecimalSignificands)
{
    // More than 800 digits keep a sticky digit; the value is just above the midpoint 2^53 + 1.
    std::string text = "9007199254740993";
    text += std::string(900, '0');
    text += "1e-901";
    EXPECT_EQ(bits(*motrack::osp::parseJavaDouble(text)), 0x4340000000000001U);
    text = "9007199254740993" + std::string(900, '0') + "e-900";
    EXPECT_EQ(bits(*motrack::osp::parseJavaDouble(text)), 0x4340000000000000U);
    EXPECT_EQ(bits(*motrack::osp::parseJavaDouble("0." + std::string(400, '0') + "1e400")), bits(0.1));
    EXPECT_EQ(bits(*motrack::osp::parseJavaDouble("1" + std::string(400, '0') + "e-400")), bits(1.0));
    EXPECT_EQ(bits(*motrack::osp::parseJavaDouble("17976931348623158e292")), 0x7fefffffffffffffU);
    EXPECT_EQ(bits(*motrack::osp::parseJavaDouble("17976931348623159e292")), 0x7ff0000000000000U);
    EXPECT_EQ(bits(*motrack::osp::parseJavaDouble("17976931348623157.9e292")), 0x7fefffffffffffffU);
    EXPECT_EQ(bits(*motrack::osp::parseJavaDouble("2.2250738585072011e-308")), 0x000fffffffffffffU);
    EXPECT_EQ(bits(*motrack::osp::parseJavaDouble("2.2250738585072012e-308")), 0x0010000000000000U);
}

TEST(OspJavaText, RoundsIntegerMidpointsToEven)
{
    Random random(20261121);
    for (int exponent = 53; exponent < 63; ++exponent)
    {
        const auto spacing = std::uint64_t{1} << (exponent - 52);
        for (int sample = 0; sample < 50; ++sample)
        {
            const auto low = (std::uint64_t{1} << exponent) + (spacing * random.below(std::uint64_t{1} << 40));
            const auto midpoint = low + (spacing / 2);
            const auto lowEven = ((low / spacing) % 2) == 0;
            const auto even = static_cast<double>(lowEven ? low : low + spacing);
            const auto text = std::to_string(midpoint);
            EXPECT_EQ(*motrack::osp::parseJavaDouble(text), even) << text;
            EXPECT_EQ(*motrack::osp::parseJavaDouble(text + ".0000000000000000000000001"), static_cast<double>(low + spacing)) << text;
            EXPECT_EQ(*motrack::osp::parseJavaDouble(std::to_string(midpoint - 1) + ".9999999999999999999999999"), static_cast<double>(low))
                << text;
        }
    }
}

#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
TEST(OspJavaText, ParsingMatchesTheStandardLibraryOnRandomDecimals)
{
    Random random(42);
    for (int sample = 0; sample < 4000; ++sample)
    {
        std::string digits;
        const auto length = 1 + random.below(sample % 10 == 0 ? 120 : 25);
        for (std::uint64_t digit = 0; digit < length; ++digit)
        {
            digits.push_back(static_cast<char>('0' + random.below(10)));
        }
        const auto exponent = static_cast<int>(random.below(700)) - 360;
        const auto text = digits + "e" + std::to_string(exponent);
        double expected = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), expected);
        if (parsed.ec == std::errc::result_out_of_range)
        {
            continue;
        }
        ASSERT_EQ(parsed.ec, std::errc{});
        EXPECT_EQ(bits(*motrack::osp::parseJavaDouble(text)), bits(expected)) << text;
    }
}

TEST(OspJavaText, FormattingUsesShortestDigitsLikeTheStandardLibrary)
{
    Random random(7);
    for (int sample = 0; sample < 3000; ++sample)
    {
        const auto value = finiteDouble(random);
        const auto java = motrack::osp::formatJavaDouble(value);
        std::array<char, 64> buffer{};
        const auto written = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::scientific);
        const std::string_view shortest(buffer.data(), static_cast<std::size_t>(written.ptr - buffer.data()));
        auto digits = std::string(shortest.substr(0, shortest.find('e')));
        std::erase(digits, '.');
        std::erase(digits, '-');
        std::string javaDigits;
        for (const auto character : java.substr(0, java.find('E')))
        {
            if (character >= '0' && character <= '9')
            {
                javaDigits.push_back(character);
            }
        }
        javaDigits.erase(0, std::min(javaDigits.find_first_not_of('0'), javaDigits.size()));
        while (javaDigits.size() > 1 && javaDigits.back() == '0')
        {
            javaDigits.pop_back();
        }
        if (digits.size() > 1)
        {
            EXPECT_EQ(javaDigits, digits) << java << " vs " << shortest;
        }
    }
}
#endif

TEST(OspJavaText, FormatsDoublesLikeJdk19DoubleToString)
{
    using Case = std::pair<double, std::string_view>;
    for (const auto& [value, text] : {
             Case{1.0, "1.0"},
             Case{-1.0, "-1.0"},
             Case{0.0, "0.0"},
             Case{-0.0, "-0.0"},
             Case{100.0, "100.0"},
             Case{0.1, "0.1"},
             Case{0.001, "0.001"},
             Case{1.0e-4, "1.0E-4"},
             Case{1.5e-5, "1.5E-5"},
             Case{9999999.0, "9999999.0"},
             Case{1.0e7, "1.0E7"},
             Case{1234567.125, "1234567.125"},
             Case{123456789.0, "1.23456789E8"},
             Case{33.333333333333336, "33.333333333333336"},
             Case{1.0e23, "1.0E23"},
             Case{9.223372036854776e18, "9.223372036854776E18"},
             Case{std::numeric_limits<double>::denorm_min(), "4.9E-324"},
             Case{std::numeric_limits<double>::max(), "1.7976931348623157E308"},
             Case{std::numeric_limits<double>::min(), "2.2250738585072014E-308"},
             Case{std::numeric_limits<double>::infinity(), "Infinity"},
             Case{-std::numeric_limits<double>::infinity(), "-Infinity"},
             Case{std::numeric_limits<double>::quiet_NaN(), "NaN"},
             Case{0.3, "0.3"},
             Case{2.0 / 3.0, "0.6666666666666666"},
             Case{-123.456, "-123.456"},
         })
    {
        EXPECT_EQ(motrack::osp::formatJavaDouble(value), text);
    }
}

TEST(OspJavaText, FormattedDoublesParseBackExactly)
{
    Random random(99);
    for (int sample = 0; sample < 4000; ++sample)
    {
        const auto value = finiteDouble(random);
        const auto text = motrack::osp::formatJavaDouble(value);
        const auto parsed = motrack::osp::parseJavaDouble(text);
        ASSERT_TRUE(parsed) << text;
        EXPECT_EQ(bits(*parsed), bits(value)) << text;
    }
    for (int exponent = -1074; exponent <= 1023; exponent += 7)
    {
        const auto value = std::ldexp(1.0, exponent);
        EXPECT_EQ(*motrack::osp::parseJavaDouble(motrack::osp::formatJavaDouble(value)), value) << exponent;
    }
}

TEST(OspJavaText, FormatsIntegersAndParsesBooleans)
{
    EXPECT_EQ(motrack::osp::formatJavaInt(-2147483647 - 1), "-2147483648");
    EXPECT_EQ(motrack::osp::formatJavaInt(42), "42");
    EXPECT_TRUE(motrack::osp::parseJavaBoolean("true"));
    EXPECT_TRUE(motrack::osp::parseJavaBoolean("TRUE"));
    EXPECT_TRUE(motrack::osp::parseJavaBoolean("tRuE"));
    EXPECT_FALSE(motrack::osp::parseJavaBoolean(" true"));
    EXPECT_FALSE(motrack::osp::parseJavaBoolean("yes"));
    EXPECT_FALSE(motrack::osp::parseJavaBoolean(""));
}

TEST(OspJavaText, TrimsAndSplitsLikeJavaStrings)
{
    EXPECT_EQ(motrack::osp::javaTrim(std::string_view("\0 a b \x1f", 7)), "a b");
    EXPECT_EQ(motrack::osp::javaTrim("   "), "");
    using Fields = std::vector<std::string_view>;
    EXPECT_EQ(motrack::osp::javaSplitCommas(""), Fields{""});
    EXPECT_EQ(motrack::osp::javaSplitCommas(","), Fields{});
    EXPECT_EQ(motrack::osp::javaSplitCommas(",,a,,"), (Fields{"", "", "a"}));
    EXPECT_EQ(motrack::osp::javaSplitCommas("a,b"), (Fields{"a", "b"}));
    EXPECT_EQ(motrack::osp::javaSplitCommas("a"), Fields{"a"});
}

} // namespace
