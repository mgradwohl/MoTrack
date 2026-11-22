#include "science/java_math.hpp"

#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace
{

namespace java = motrack::science::java;

constexpr double INF = std::numeric_limits<double>::infinity();
constexpr double NAN_VALUE = std::numeric_limits<double>::quiet_NaN();

std::uint64_t bits(double value)
{
    return std::bit_cast<std::uint64_t>(value);
}

TEST(JavaMath, PowFollowsJavaSpecialCases)
{
    EXPECT_TRUE(std::isnan(java::pow(1.0, NAN_VALUE)));
    EXPECT_TRUE(std::isnan(java::pow(1.0, INF)));
    EXPECT_TRUE(std::isnan(java::pow(-1.0, -INF)));
    EXPECT_TRUE(std::isnan(java::pow(2.0, NAN_VALUE)));
    EXPECT_EQ(java::pow(NAN_VALUE, 0.0), 1.0);
    EXPECT_EQ(java::pow(2.0, 10.0), 1024.0);
    EXPECT_EQ(java::pow(0.5, INF), 0.0);
    EXPECT_EQ(java::pow(2.0, INF), INF);
    EXPECT_EQ(bits(java::pow(-0.0, -1.0)), bits(-INF));
    EXPECT_TRUE(std::isnan(java::pow(-8.0, 1.0 / 3.0)));
}

TEST(JavaMath, RoundTiesTowardPositiveInfinityAndSaturates)
{
    EXPECT_EQ(bits(java::round(NAN_VALUE)), bits(0.0));
    EXPECT_EQ(bits(java::round(-0.0)), bits(0.0));
    EXPECT_EQ(bits(java::round(-0.4)), bits(0.0));
    EXPECT_EQ(java::round(0.5), 1.0);
    EXPECT_EQ(bits(java::round(-0.5)), bits(0.0));
    EXPECT_EQ(java::round(-2.5), -2.0);
    EXPECT_EQ(java::round(2.5), 3.0);
    EXPECT_EQ(java::round(0.49999999999999994), 0.0);
    EXPECT_EQ(java::round(4503599627370495.5), 4503599627370496.0);
    EXPECT_EQ(java::round(1e19), 9223372036854775808.0);
    EXPECT_EQ(java::round(INF), 9223372036854775808.0);
    EXPECT_EQ(java::round(-INF), -9223372036854775808.0);
    EXPECT_EQ(java::round(-9223372036854775808.0), -9223372036854775808.0);
}

TEST(JavaMath, RintSignumMinAndMax)
{
    EXPECT_EQ(java::rint(2.5), 2.0);
    EXPECT_EQ(java::rint(3.5), 4.0);
    EXPECT_EQ(bits(java::rint(-0.4)), bits(-0.0));
    EXPECT_EQ(bits(java::signum(-0.0)), bits(-0.0));
    EXPECT_EQ(java::signum(-3.0), -1.0);
    EXPECT_EQ(java::signum(INF), 1.0);
    EXPECT_TRUE(std::isnan(java::signum(NAN_VALUE)));
    EXPECT_TRUE(std::isnan(java::min(NAN_VALUE, 1.0)));
    EXPECT_TRUE(std::isnan(java::min(1.0, NAN_VALUE)));
    EXPECT_TRUE(std::isnan(java::max(NAN_VALUE, 1.0)));
    EXPECT_TRUE(std::isnan(java::max(1.0, NAN_VALUE)));
    EXPECT_EQ(bits(java::min(0.0, -0.0)), bits(-0.0));
    EXPECT_EQ(bits(java::min(-0.0, 0.0)), bits(-0.0));
    EXPECT_EQ(bits(java::max(-0.0, 0.0)), bits(0.0));
    EXPECT_EQ(bits(java::max(0.0, -0.0)), bits(0.0));
    EXPECT_EQ(java::min(1.0, 2.0), 1.0);
    EXPECT_EQ(java::max(1.0, 2.0), 2.0);
}

TEST(JavaMath, IeeeRemainderRoundsTheQuotientToEven)
{
    EXPECT_EQ(java::ieeeRemainder(5.0, 3.0), -1.0);
    EXPECT_EQ(java::ieeeRemainder(7.0, 2.0), -1.0);
    EXPECT_EQ(java::ieeeRemainder(5.0, 2.0), 1.0);
    EXPECT_EQ(bits(java::ieeeRemainder(-4.0, 2.0)), bits(-0.0));
    EXPECT_EQ(java::ieeeRemainder(1.0, INF), 1.0);
    EXPECT_TRUE(std::isnan(java::ieeeRemainder(1.0, 0.0)));
    EXPECT_TRUE(std::isnan(java::ieeeRemainder(INF, 1.0)));
}

} // namespace
