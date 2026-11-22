#include "science/expression.hpp"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{

using motrack::science::BuiltinFunction;
using motrack::science::Expression;
using motrack::science::ExpressionDialect;
using motrack::science::ExpressionError;
using motrack::science::ExpressionErrorCode;
using motrack::science::ExpressionOptions;
using motrack::science::parseExpression;
using motrack::science::RandomSource;
using motrack::science::TrackerDifferences;

constexpr double NAN_VALUE = std::numeric_limits<double>::quiet_NaN();
constexpr std::array<std::string_view, 0> NO_VARIABLES{};
constexpr std::array<std::string_view, 2> XY{"x", "y"};
constexpr ExpressionOptions TRACKER{.dialect = ExpressionDialect::Tracker};

Expression
compiled(std::string_view text, std::span<const std::string_view> variables = NO_VARIABLES, const ExpressionOptions& options = {})
{
    auto parsed = parseExpression(text, variables, options);
    if (!parsed)
    {
        ADD_FAILURE() << text << ": error " << static_cast<int>(parsed.error().code) << " at " << parsed.error().position;
        return *parseExpression("0", NO_VARIABLES);
    }
    return *parsed;
}

double value(std::string_view text, const ExpressionOptions& options = {})
{
    return compiled(text, NO_VARIABLES, options).evaluate({});
}

ExpressionError
failure(std::string_view text, std::span<const std::string_view> variables = NO_VARIABLES, const ExpressionOptions& options = {})
{
    const auto parsed = parseExpression(text, variables, options);
    if (parsed)
    {
        ADD_FAILURE() << text << " unexpectedly parsed";
        return {};
    }
    return parsed.error();
}

class SequenceRandom final : public RandomSource
{
  public:
    explicit SequenceRandom(std::vector<double> draws) : m_Draws(std::move(draws))
    {}

    double next() override
    {
        return m_Draws[m_Next++ % m_Draws.size()];
    }

    [[nodiscard]] std::size_t draws() const
    {
        return m_Next;
    }

  private:
    std::vector<double> m_Draws;
    std::size_t m_Next = 0;
};

TEST(Expression, EvaluatesVariablesInDeclarationOrder)
{
    const auto expression = compiled("x*10+y", XY);
    EXPECT_EQ(expression.variableCount(), 2U);
    EXPECT_EQ(expression.text(), "x*10+y");
    EXPECT_EQ(expression.dialect(), ExpressionDialect::Corrected);
    EXPECT_EQ(expression.evaluate(std::array{1.0, 2.0}), 12.0);
    EXPECT_EQ(expression.evaluate(std::array{-3.0, 0.5}), -29.5);
    EXPECT_TRUE(std::isnan(expression.evaluate(std::array{1.0, NAN_VALUE})));
}

TEST(Expression, RejectsWrongValueCounts)
{
    const auto expression = compiled("x+y", XY);
    EXPECT_TRUE(std::isnan(expression.evaluate(std::array{1.0})));
    EXPECT_TRUE(std::isnan(expression.evaluate(std::array{1.0, 2.0, 3.0})));
    EXPECT_TRUE(std::isnan(compiled("1").evaluate(std::array{1.0})));
}

TEST(Expression, ReportsReferencedVariablesAndFunctions)
{
    const std::array<std::string_view, 4> names{"a", "b", "c", "unused"};
    const auto expression = compiled("if(c<0, sin(b)+b, max(a, b))*cos(c)", names);
    EXPECT_EQ(std::vector(expression.referencedVariables().begin(), expression.referencedVariables().end()),
              (std::vector<std::size_t>{0, 1, 2}));
    EXPECT_TRUE(expression.referencesVariable(0));
    EXPECT_FALSE(expression.referencesVariable(3));
    EXPECT_FALSE(expression.referencesVariable(99));
    EXPECT_EQ(std::vector(expression.functions().begin(), expression.functions().end()),
              (std::vector{BuiltinFunction::Sin, BuiltinFunction::Cos, BuiltinFunction::Max}));
    EXPECT_FALSE(expression.usesRandom());
    EXPECT_TRUE(compiled("random(2)").usesRandom());
}

TEST(Expression, AcceptsOwnedVariableNames)
{
    const std::vector<std::string> names{"θ", "ω"};
    const auto parsed = parseExpression("θ*ω", names);
    ASSERT_TRUE(parsed.hasValue());
    EXPECT_EQ(parsed->evaluate(std::array{2.0, 4.0}), 8.0);
}

TEST(Expression, CopiesShareTheImmutableProgram)
{
    const auto original = compiled("x^2", XY);
    auto copy = compiled("1");
    copy = original;
    EXPECT_EQ(copy.evaluate(std::array{3.0, 0.0}), 9.0);
    EXPECT_EQ(&copy.text(), &original.text());
}

TEST(Expression, EvaluatesConcurrently)
{
    const auto expression = compiled("sqrt(x)*y+random(0)", XY);
    std::array<bool, 4> exact{};
    std::vector<std::thread> workers;
    workers.reserve(exact.size());
    for (auto& result : exact)
    {
        workers.emplace_back(
            [&expression, &result]
            {
                bool same = true;
                for (int i = 0; i < 2000; ++i)
                {
                    const auto x = static_cast<double>(i);
                    same = same && expression.evaluate(std::array{x, 2.0}) == std::sqrt(x) * 2.0;
                }
                result = same;
            });
    }
    for (auto& worker : workers)
    {
        worker.join();
    }
    EXPECT_EQ(exact, (std::array{true, true, true, true}));
}

TEST(Expression, RandomScalesOneDrawPerCall)
{
    const auto expression = compiled("random(x)+random(10)", XY);
    SequenceRandom random({0.25, 0.5});
    EXPECT_EQ(expression.evaluate(std::array{4.0, 0.0}, random), 1.0 + 5.0);
    EXPECT_EQ(random.draws(), 2U);
    const auto branch = compiled("if(x<0, random(1), 7)", XY);
    EXPECT_EQ(branch.evaluate(std::array{1.0, 0.0}, random), 7.0);
    EXPECT_EQ(random.draws(), 2U); // The untaken branch does not draw.
    for (int i = 0; i < 1000; ++i)
    {
        const double draw = compiled("random(1)").evaluate({});
        ASSERT_GE(draw, 0.0);
        ASSERT_LT(draw, 1.0);
    }
}

TEST(Expression, CorrectsTrackerPrecedenceDefects)
{
    EXPECT_EQ(value("2^-3"), 0.125);
    EXPECT_EQ(value("2^-3", TRACKER), -8.0);
    EXPECT_EQ(value("1+2^2*3"), 13.0);
    EXPECT_EQ(value("1+2^2*3", TRACKER), 15.0);
    EXPECT_EQ(value("2--3"), 5.0);
    EXPECT_EQ(value("2--3", TRACKER), -1.0);
    EXPECT_EQ(value("-2^-2"), -0.25);
    EXPECT_EQ(value("-2^-2", TRACKER), 4.0);
    EXPECT_EQ(compiled("2^-3").trackerDifferences(), (TrackerDifferences{.negatedExponent = true}));
    EXPECT_EQ(compiled("1+2^2/3", NO_VARIABLES, TRACKER).trackerDifferences(), (TrackerDifferences{.powerProductInSum = true}));
    EXPECT_EQ(compiled("1+-2").trackerDifferences(), (TrackerDifferences{.droppedNegation = true}));
    EXPECT_EQ(compiled("asinh(1)").trackerDifferences(), (TrackerDifferences{.inverseHyperbolic = true}));
    EXPECT_FALSE(compiled("1-(-2)+-3*4+2^2").trackerDifferences().any());
}

TEST(Expression, KeepsTrackerConventions)
{
    EXPECT_EQ(value("1/0"), 1.0 / 1.0e-128); // OSP's divide-by-zero trap is kept in both dialects.
    EXPECT_EQ(value("0/0"), 0.0);
    EXPECT_EQ(value("int(2.5)"), 2.0);
    EXPECT_EQ(value("mod(5,3)"), -1.0);
    EXPECT_EQ(value("log(1000)"), std::log(1000.0) / std::numbers::ln10);
    EXPECT_EQ(value("(1<0)&(1<0)|(0<1)"), 0.0); // & and | share one precedence and group to the right.
    EXPECT_EQ(value("if(0/0*sqrt(-1), 1, 2)"), 1.0);
    EXPECT_EQ(value("step(-0)"), 1.0);
    EXPECT_EQ(std::bit_cast<std::uint64_t>(value("-0")), std::bit_cast<std::uint64_t>(-0.0));
}

TEST(Expression, InverseHyperbolicFunctionsKeepPrecision)
{
    EXPECT_EQ(value("asinh(1e-20)"), 1e-20);
    EXPECT_EQ(value("asinh(1e-20)", TRACKER), 0.0);
    EXPECT_NEAR(value("asinh(-1e8)"), -19.113827924512311, 1e-14);
    EXPECT_EQ(value("asinh(-1e8)", TRACKER), -std::numeric_limits<double>::infinity());
    EXPECT_NEAR(value("acosh(1e200)"), 461.21016577936913, 1e-12);
    EXPECT_EQ(value("acosh(1e200)", TRACKER), std::numeric_limits<double>::infinity());
    EXPECT_EQ(value("atanh(1e-20)"), 1e-20);
    EXPECT_EQ(value("atanh(1e-20)", TRACKER), 0.0);
}

TEST(Expression, TrackerDialectEnforcesTheCodeLimit)
{
    std::string sum = "1";
    for (int i = 0; i < 49; ++i)
    {
        sum += "+1";
    }
    EXPECT_EQ(compiled(sum, NO_VARIABLES, TRACKER).trackerCodeSize(), 99U);
    EXPECT_TRUE(compiled(sum, NO_VARIABLES, TRACKER).fitsTrackerLimits());
    sum += "+1";
    EXPECT_EQ(failure(sum, NO_VARIABLES, TRACKER).code, ExpressionErrorCode::TrackerCodeLimit);
    const auto corrected = compiled(sum);
    EXPECT_EQ(corrected.trackerCodeSize(), 101U);
    EXPECT_FALSE(corrected.fitsTrackerLimits());
    EXPECT_EQ(corrected.evaluate({}), 51.0);
    EXPECT_EQ(compiled("if(1<2,3,4)").trackerCodeSize(), 9U); // 1 2 < IF d 3 JUMP d 4
}

TEST(Expression, EvaluatesDeepProgramsOnTheHeap)
{
    std::string text;
    for (int i = 0; i < 80; ++i)
    {
        text += "1+(";
    }
    text += "1" + std::string(80, ')');
    EXPECT_EQ(value(text), 81.0);
    EXPECT_EQ(failure(text, NO_VARIABLES, TRACKER).code, ExpressionErrorCode::TrackerCodeLimit);
}

TEST(Expression, BoundsLengthAndNesting)
{
    const ExpressionOptions shortText{.maxLength = 5};
    EXPECT_EQ(value("1+2+3", shortText), 6.0);
    EXPECT_EQ(value("1\\+2\\+3", shortText), 6.0); // Backslashes are removed before the bound applies.
    EXPECT_EQ(failure("1+2+34", NO_VARIABLES, shortText), (ExpressionError{.code = ExpressionErrorCode::TooLong, .position = 6}));
    const ExpressionOptions shallow{.maxNesting = 3};
    EXPECT_EQ(value("((1))", shallow), 1.0);
    EXPECT_EQ(failure("(((1)))", NO_VARIABLES, shallow).code, ExpressionErrorCode::TooDeep);
    EXPECT_EQ(failure("2^2^2^2", NO_VARIABLES, shallow).code, ExpressionErrorCode::TooDeep);
    EXPECT_EQ(value(std::string(127, '(') + "1" + std::string(127, ')')), 1.0);
    EXPECT_EQ(failure(std::string(128, '(') + "1" + std::string(128, ')')).code, ExpressionErrorCode::TooDeep);
    EXPECT_EQ(failure(std::string(10000, '(')).code, ExpressionErrorCode::TooLong);
    EXPECT_EQ(failure(std::string(1000, '(')).code, ExpressionErrorCode::TooDeep);
}

TEST(Expression, ReportsOspErrorPositions)
{
    EXPECT_EQ(failure(""), (ExpressionError{.code = ExpressionErrorCode::ExpressionExpected, .position = 0}));
    EXPECT_EQ(failure("1+"), (ExpressionError{.code = ExpressionErrorCode::SyntaxError, .position = 3}));
    EXPECT_EQ(failure("sin(x", XY), (ExpressionError{.code = ExpressionErrorCode::ParenthesesMismatch, .position = 7}));
    EXPECT_EQ(failure("x+z", XY), (ExpressionError{.code = ExpressionErrorCode::UnknownIdentifier, .position = 3}));
    EXPECT_EQ(failure("1<2<3"), (ExpressionError{.code = ExpressionErrorCode::InvalidOperator, .position = 4}));
    EXPECT_EQ(failure("x;a:1", XY), (ExpressionError{.code = ExpressionErrorCode::ReferenceDefinitionExpected, .position = 3}));
    EXPECT_EQ(failure("x; :1", XY), (ExpressionError{.code = ExpressionErrorCode::ReferenceNameExpected, .position = 3}));
    EXPECT_EQ(failure("θ+ω+😀"), (ExpressionError{.code = ExpressionErrorCode::UnknownIdentifier, .position = 1}));
    const std::array<std::string_view, 2> greek{"θ", "ω"};
    EXPECT_EQ(failure("θ+ω+😀", greek), (ExpressionError{.code = ExpressionErrorCode::UnknownIdentifier, .position = 5}));
    EXPECT_EQ(failure("😀+x", XY), (ExpressionError{.code = ExpressionErrorCode::UnknownIdentifier, .position = 1}));
    EXPECT_EQ(failure("x+😀*", XY), (ExpressionError{.code = ExpressionErrorCode::UnknownIdentifier, .position = 3}));
}

TEST(Expression, RejectsInvalidUtf8)
{
    for (const std::string_view text :
         {"\xC3\x28", "\xC0\xAF", "\xED\xA0\x80", "\xE2\x82", "\xF4\x90\x80\x80", "\xFF", "\x80", "\xF0\x9F\x98"})
    {
        EXPECT_EQ(failure(text), (ExpressionError{.code = ExpressionErrorCode::InvalidEncoding, .position = 0}));
        const std::array<std::string_view, 1> names{text};
        EXPECT_EQ(failure("1", names).code, ExpressionErrorCode::InvalidEncoding);
    }
    const std::array<std::string_view, 3> names{"\xC2\xB5", "\xE2\x82\xAC", "\xF0\x9F\x98\x80"};
    EXPECT_EQ(compiled("\xC2\xB5+\xE2\x82\xAC*\xF0\x9F\x98\x80", names).evaluate(std::array{1.0, 2.0, 3.0}), 7.0);
}

TEST(Expression, NamesAndMessages)
{
    EXPECT_EQ(motrack::science::functionName(BuiltinFunction::Atan2), "atan2");
    EXPECT_EQ(motrack::science::functionName(BuiltinFunction::Sin), "sin");
    for (const std::string_view name : {"pi", "e", "if", "sqrt", "random", "atan2"})
    {
        EXPECT_TRUE(motrack::science::isReservedName(name)) << name;
    }
    for (const std::string_view name : {"x", "Sin", "PI", "E", "", "pow", "t"})
    {
        EXPECT_FALSE(motrack::science::isReservedName(name)) << name;
    }
    for (const auto code : {ExpressionErrorCode::TrackerCodeLimit,
                            ExpressionErrorCode::TooLong,
                            ExpressionErrorCode::TooDeep,
                            ExpressionErrorCode::InvalidEncoding})
    {
        EXPECT_FALSE(motrack::science::errorMessage(code).empty());
    }
}

} // namespace
