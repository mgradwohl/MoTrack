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
    // The corrected program adds the negation Tracker drops; Tracker's code count for the text does not.
    EXPECT_EQ(compiled("1+-2").trackerCodeSize(), 3U);
    EXPECT_EQ(compiled("1+-2", NO_VARIABLES, TRACKER).trackerCodeSize(), 3U);
    EXPECT_FALSE(compiled("1-(-2)+-3*4+2^2").trackerDifferences().any());
}

TEST(Expression, KeepsTrackerConventions)
{
    EXPECT_EQ(value("1/0"), 1.0 / 1.0e-128); // OSP's divide-by-zero trap is kept in both dialects.
    EXPECT_EQ(value("0/0"), 0.0);
    EXPECT_EQ(value("int(2.5)"), 2.0);
    EXPECT_EQ(value("mod(5,3)"), -1.0);
    EXPECT_EQ(value("log(1000)", TRACKER), std::log(1000.0) / std::numbers::ln10);
    EXPECT_EQ(value("(1<0)&(1<0)|(0<1)"), 0.0); // & and | share one precedence and group to the right.
    EXPECT_EQ(value("if(0/0*sqrt(-1), 1, 2)"), 1.0);
    EXPECT_EQ(value("step(-0)"), 1.0);
    EXPECT_EQ(std::bit_cast<std::uint64_t>(value("-0")), std::bit_cast<std::uint64_t>(-0.0));
}

/// `1e+5` is the literal 1e5 in both dialects, as for Java's Double.parseDouble and SuryonoParser. Tracker's
/// models and fit functions reject such text (UserFunction pads it to `1e +5`, #215), so it is reported.
/// Expected values are C++ literals, converted by the compiler rather than the kernel.
TEST(Expression, AcceptsPlusExponentSigns)
{
    const std::array<std::string_view, 2> xa{"x", "A"};
    struct Case
    {
        std::string_view text;
        std::array<double, 2> values;
        double expected;
    };
    const std::array cases{
        Case{.text = "1e+5", .values = {}, .expected = 1e5},
        Case{.text = "1E+5", .values = {}, .expected = 1e5},
        Case{.text = "2.5e+3", .values = {}, .expected = 2.5e3},
        Case{.text = "1.e+2", .values = {}, .expected = 100.0},
        Case{.text = ".5e+1", .values = {}, .expected = 5.0},
        Case{.text = "1e+0", .values = {}, .expected = 1.0},
        Case{.text = "1e+308", .values = {}, .expected = 1e308},
        Case{.text = "1e+400", .values = {}, .expected = std::numeric_limits<double>::infinity()},
        Case{.text = "x*1e+2", .values = {3.0, 0.0}, .expected = 300.0},
        Case{.text = "x*1E+5", .values = {2.0, 0.0}, .expected = 2e5},
        Case{.text = "2.5e+3+x", .values = {2.0, 0.0}, .expected = 2502.0},
        Case{.text = "A*x*1e+5", .values = {2.0, 2.0}, .expected = 4e5},
        Case{.text = "x-1e+1", .values = {2.0, 0.0}, .expected = -8.0},
        Case{.text = "2^1e+1", .values = {}, .expected = 1024.0},
        Case{.text = "-1e+2*x", .values = {3.0, 0.0}, .expected = -300.0},
    };
    for (const Case& c : cases)
    {
        SCOPED_TRACE(c.text);
        for (const auto& options : {ExpressionOptions{}, TRACKER})
        {
            const auto expression = compiled(c.text, xa, options);
            EXPECT_EQ(expression.evaluate(c.values), c.expected);
            EXPECT_EQ(expression.trackerDifferences(), (TrackerDifferences{.plusExponentSign = true}));
            EXPECT_TRUE(expression.trackerDifferences().any());
            // The export has no sign for UserFunction to split, and the same value.
            const auto exported = parseExpression(expression.trackerText(), xa, TRACKER);
            ASSERT_TRUE(exported.hasValue()) << expression.trackerText();
            EXPECT_FALSE(exported->trackerDifferences().any());
            EXPECT_EQ(exported->evaluate(c.values), c.expected);
        }
    }
    EXPECT_EQ(compiled("x*1e+2", xa).trackerText(), "(x*1e2)");
    // Not literals with a plus exponent: the constant e, a negative exponent, and names ending in e.
    const std::array<std::string_view, 3> names{"x", "x1e", "E"};
    const std::array values{2.0, 10.0, 3.0};
    EXPECT_EQ(compiled("1e5", names).evaluate(values), 1e5);
    EXPECT_EQ(compiled("x*1e-5", names).evaluate(values), 2e-5);
    EXPECT_EQ(compiled("e+5", names).evaluate(values), std::numbers::e + 5.0);
    const double twoE = 2.0 * std::numbers::e; // A separate statement, so the sum below is never fused.
    EXPECT_EQ(compiled("x*e+5", names).evaluate(values), twoE + 5.0);
    EXPECT_EQ(compiled("e-5", names).evaluate(values), std::numbers::e - 5.0);
    EXPECT_EQ(compiled("x1e+5", names).evaluate(values), 15.0);
    EXPECT_EQ(compiled("x*E+5", names).evaluate(values), 11.0);
    for (const std::string_view text : {"1e5", "x*1e-5", "e+5", "x*e+5", "e-5", "x1e+5", "x*E+5"})
    {
        EXPECT_FALSE(compiled(text, names).trackerDifferences().any()) << text;
    }
    // `e` and `E` never join a preceding number: `2E+5` is a literal even beside a variable named E, and
    // the reserved constant e wins over a variable named e.
    EXPECT_EQ(compiled("2E+5*x", names).evaluate(values), 4e5);
    const std::array<std::string_view, 1> named{"e"};
    EXPECT_EQ(compiled("e+5", named).evaluate(std::array{7.0}), std::numbers::e + 5.0);
    EXPECT_EQ(compiled("1e+5", named).evaluate(std::array{7.0}), 1e5);
    // The text UserFunction makes of 1e+5 is rejected like any split literal.
    EXPECT_EQ(failure("1e +5"), (ExpressionError{.code = ExpressionErrorCode::ExpressionExpected, .position = 1}));
    EXPECT_EQ(failure("2*1e +5"), (ExpressionError{.code = ExpressionErrorCode::SyntaxError, .position = 3}));
    EXPECT_EQ(failure("1e+").code, ExpressionErrorCode::ExpressionExpected);
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

TEST(Expression, CommonLogarithmIsExactForPowersOfTen)
{
    EXPECT_EQ(value("log(1000)"), 3.0);
    EXPECT_EQ(value("log(1000)", TRACKER), 2.9999999999999996);
    EXPECT_EQ(value("floor(log(1e6))"), 6.0);
    EXPECT_EQ(value("floor(log(1e6))", TRACKER), 5.0);
    EXPECT_EQ(value("if(log(100)=2, 1, 0)"), 1.0);
    EXPECT_EQ(compiled("log(x)", XY).trackerDifferences(), (TrackerDifferences{.commonLogarithm = true}));
}

TEST(Expression, CompilesEachTextIndependently)
{
    // OSP's setFunction("0") and ("1") on a reused parser append to the previous code (osp#17); here every
    // parse owns a new program, so earlier expressions cannot leak into later ones.
    const auto sine = compiled("sin(x)", XY);
    for (const auto& [text, expected] : {std::pair{"0", 0.0}, std::pair{"0.0", 0.0}, std::pair{"1", 1.0}, std::pair{"1.0", 1.0}})
    {
        const auto constant = compiled(text, XY);
        EXPECT_EQ(constant.evaluate(std::array{1.0, 0.0}), expected) << text;
        EXPECT_EQ(sine.evaluate(std::array{1.0, 0.0}), std::sin(1.0));
    }
}

TEST(Expression, TrackerTextIsFullyParenthesized)
{
    constexpr std::array<std::string_view, 4> NAMES{"x", "y", "v_{x}", "\xCE\xB8"};
    const auto rendered = [&](std::string_view text)
    {
        return compiled(text, NAMES).trackerText();
    };
    EXPECT_EQ(rendered("2^-3"), "(2^(-3))");
    EXPECT_EQ(rendered("1+2^2*3"), "(1+((2^2)*3))");
    EXPECT_EQ(rendered("2--3"), "(2-(-3))");
    EXPECT_EQ(rendered("-x^2"), "(-(x^2))");
    EXPECT_EQ(rendered(".5e1 + pi*e"), "(0.5e1+(pi*e))");
    EXPECT_EQ(rendered("if(x<0, -x, max(x, y))"), "if((x<0),(-x),max(x,y))");
    EXPECT_EQ(rendered("!(x<1)&(y>=2)|(x<>y)"), "(!((x<1))&((y>=2)|(x<>y)))");
    EXPECT_EQ(rendered("if(x<=0,if(y=1,1,2),sqr(v_{x}))"), "if((x<=0),if((y=1),1,2),sqr(v_{x}))");
    EXPECT_EQ(rendered("\xCE\xB8/x"), "(\xCE\xB8/x)");
    EXPECT_EQ(rendered("-(!(1<2))"), "(-(!((1<2))))");
    EXPECT_EQ(rendered("if(-(!(x<1)),1,2)"), "if((-(!((x<1)))),1,2)");
    EXPECT_EQ(rendered("1e+5*2.5E+3-1e-2"), "((1e5*2.5E3)-1e-2)");
    for (const std::string_view text : {"-(!(1<2))", "if(-(!(x<1)),1,2)", "1e+5*2.5E+3-1e-2"})
    {
        const auto tracker = parseExpression(rendered(text), NAMES, TRACKER);
        ASSERT_TRUE(tracker.hasValue()) << text;
        EXPECT_EQ(tracker->evaluate(std::array{0.0, 0.0, 0.0, 0.0}), compiled(text, NAMES).evaluate(std::array{0.0, 0.0, 0.0, 0.0}));
    }
    // A Tracker-dialect expression renders its own (defective) operations, which Tracker then reproduces.
    EXPECT_EQ(compiled("2^-3", NO_VARIABLES, TRACKER).trackerText(), "(-(2^3))");
    const auto exported = parseExpression(rendered("x--y*2+2^-x"), NAMES, TRACKER);
    ASSERT_TRUE(exported.hasValue());
    EXPECT_FALSE(exported->trackerDifferences().any());
    EXPECT_EQ(exported->evaluate(std::array{3.0, 2.0, 0.0, 0.0}), compiled("x--y*2+2^-x", NAMES).evaluate(std::array{3.0, 2.0, 0.0, 0.0}));
}

/// Seeded random expressions over the whole grammar: trackerText() must make Tracker compute the corrected value.
class ExpressionGenerator
{
  public:
    explicit ExpressionGenerator(std::uint64_t seed) : m_State(seed)
    {}

    std::string value(int depth)
    {
        static constexpr std::array<std::string_view, 9> ATOMS{"1", "2", "0.5", "3", "1e+2", "2.5E-1", ".5", "x", "y"};
        static constexpr std::array<std::string_view, 9> UNARY{"abs", "sqr", "sin", "cos", "sqrt", "round", "frac", "sign", "step"};
        static constexpr std::array<std::string_view, 3> BINARY{"max", "min", "mod"};
        static constexpr std::array<std::string_view, 4> OPERATORS{"+", "-", "*", "/"};
        if (depth <= 0 || pick(5) == 0)
        {
            return std::string(ATOMS[pick(ATOMS.size())]);
        }
        switch (pick(9))
        {
        case 0:
            return "-" + value(depth - 1);
        case 1:
            return value(depth - 1) + std::string(OPERATORS[pick(OPERATORS.size())]) + value(depth - 1);
        case 2:
            return value(depth - 1) + "^" + (pick(2) == 0 ? "-" : "") + value(depth - 1);
        case 3:
            return "(" + value(depth - 1) + ")";
        case 4:
            return std::string(UNARY[pick(UNARY.size())]) + "(" + value(depth - 1) + ")";
        case 5:
            return std::string(BINARY[pick(BINARY.size())]) + "(" + value(depth - 1) + "," + value(depth - 1) + ")";
        case 6:
            return "if(" + condition(depth - 1) + "," + value(depth - 1) + "," + value(depth - 1) + ")";
        case 7:
            return "if(" + condition(depth - 1) + ",-(!(" + condition(depth - 1) + ")),--" + value(depth - 1) + ")";
        default:
            return value(depth - 1) + " " + std::string(OPERATORS[pick(OPERATORS.size())]) + " -" + value(depth - 1);
        }
    }

  private:
    std::string condition(int depth)
    {
        static constexpr std::array<std::string_view, 6> RELATIONS{"<", ">", "<=", ">=", "=", "<>"};
        switch (depth <= 0 ? 2 : pick(3))
        {
        case 0:
            return "(" + condition(depth - 1) + ")&(" + condition(depth - 1) + ")";
        case 1:
            return "!(" + condition(depth - 1) + ")";
        default:
            return value(depth - 1) + std::string(RELATIONS[pick(RELATIONS.size())]) + value(depth - 1);
        }
    }

    std::size_t pick(std::size_t count)
    {
        m_State += 0x9E3779B97F4A7C15ULL; // SplitMix64
        std::uint64_t mixed = (m_State ^ (m_State >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        mixed = (mixed ^ (mixed >> 27U)) * 0x94D049BB133111EBULL;
        return static_cast<std::size_t>((mixed ^ (mixed >> 31U)) % count);
    }

    std::uint64_t m_State;
};

TEST(Expression, TrackerTextRoundTripsRandomExpressions)
{
    ExpressionGenerator generator(20261122);
    const std::array points{std::array{0.5, -2.0}, std::array{3.0, 0.0}, std::array{-1.5, 4.0}};
    int compared = 0;
    for (int i = 0; i < 4000; ++i)
    {
        const auto text = generator.value(4);
        const auto corrected = parseExpression(text, XY);
        if (!corrected || !corrected->fitsTrackerLimits())
        {
            continue; // Rejected text, or too long for Tracker itself.
        }
        const auto exported = parseExpression(corrected->trackerText(), XY, TRACKER);
        if (!exported && exported.error().code == ExpressionErrorCode::TrackerCodeLimit && corrected->trackerDifferences().droppedNegation)
        {
            continue; // The explicit negations Tracker would drop can exceed its limit.
        }
        ASSERT_TRUE(exported.hasValue()) << text << " -> " << corrected->trackerText() << ": error "
                                         << static_cast<int>(exported.error().code) << " at " << exported.error().position;
        ASSERT_FALSE(exported->trackerDifferences().plusExponentSign) << corrected->trackerText(); // The atoms include 1e+2.
        for (const auto& point : points)
        {
            const double expected = corrected->evaluate(point);
            const double actual = exported->evaluate(point);
            ASSERT_TRUE((std::isnan(expected) && std::isnan(actual)) ||
                        std::bit_cast<std::uint64_t>(expected) == std::bit_cast<std::uint64_t>(actual))
                << text << " -> " << corrected->trackerText();
        }
        ++compared;
    }
    EXPECT_GT(compared, 1000);
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
    // Positions count UTF-16 code units, so the astral name occupies two.
    const std::array<std::string_view, 1> astral{"😀"};
    EXPECT_EQ(failure("😀+*", astral), (ExpressionError{.code = ExpressionErrorCode::SyntaxError, .position = 4}));
    EXPECT_EQ(compiled("😀*2", astral).evaluate(std::array{3.0}), 6.0);
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
