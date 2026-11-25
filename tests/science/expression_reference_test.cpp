#include "science/expression.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

// Compares both dialects with SuryonoParser results recorded from the pinned Tracker 6.3.5 JAR by
// tools/reference/ExpressionOracle.java (regenerate and diff with `tools/corpus.py expression-oracle`).

namespace
{

using motrack::science::Expression;
using motrack::science::ExpressionDialect;
using motrack::science::ExpressionErrorCode;

constexpr int RANDOM_SAMPLES = 200;

QJsonObject loadReference()
{
    QFile file(QStringLiteral(MOTRACK_EXPRESSION_REFERENCE));
    if (!file.open(QIODevice::ReadOnly))
    {
        return {};
    }
    return QJsonDocument::fromJson(file.readAll()).object();
}

const QJsonObject& reference()
{
    static const QJsonObject document = loadReference();
    return document;
}

/// Parses Java's Double.toString text without using the engine's own decimal conversion.
double javaText(const QJsonValue& value)
{
    const auto text = value.toString();
    if (text == QStringLiteral("NaN"))
    {
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (text.endsWith(QStringLiteral("Infinity")))
    {
        return text.startsWith(u'-') ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
    }
    bool ok = false;
    const double result = text.toDouble(&ok);
    EXPECT_TRUE(ok) << text.toStdString();
    return result;
}

double javaBits(const QJsonValue& value)
{
    bool ok = false;
    const auto bits = value.toString().toULongLong(&ok, 16);
    EXPECT_TRUE(ok);
    return std::bit_cast<double>(static_cast<std::uint64_t>(bits));
}

std::int64_t ordered(double value)
{
    const auto bits = std::bit_cast<std::int64_t>(value);
    return bits < 0 ? std::numeric_limits<std::int64_t>::min() - bits : bits;
}

/// Exact cases compare bits (signed zeros included). Transcendental cases accept the declared ulp bound:
/// Java's Math.sin, exp, log and pow, and each C++ library, are only within about 1 ulp of exact.
testing::AssertionResult sameValue(double expected, double actual, bool ulp, int maxUlps)
{
    if (std::isnan(expected) || std::isnan(actual))
    {
        if (std::isnan(expected) && std::isnan(actual))
        {
            return testing::AssertionSuccess();
        }
        return testing::AssertionFailure() << "expected " << expected << ", got " << actual;
    }
    if (!ulp || !std::isfinite(expected) || !std::isfinite(actual))
    {
        if (std::bit_cast<std::uint64_t>(expected) == std::bit_cast<std::uint64_t>(actual))
        {
            return testing::AssertionSuccess();
        }
        return testing::AssertionFailure() << "expected bits of " << expected << ", got " << actual;
    }
    const auto distance = ordered(expected) - ordered(actual);
    if (distance >= -maxUlps && distance <= maxUlps)
    {
        return testing::AssertionSuccess();
    }
    return testing::AssertionFailure() << "expected " << expected << ", got " << actual << " (" << distance << " ulp)";
}

std::vector<double> values(const QJsonObject& evaluation)
{
    std::vector<double> result;
    for (const auto& value : evaluation[QStringLiteral("values")].toArray())
    {
        result.push_back(javaText(value));
    }
    return result;
}

void checkDefects(const QJsonObject& item, const Expression& expression)
{
    const auto defect = item[QStringLiteral("defect")].toString();
    auto differences = expression.trackerDifferences();
    // SuryonoParser evaluates a `+` exponent correctly; the UserFunction cases below show where Tracker does
    // not. A literal case is one number, so its `+` after an exponent marker is the literal's sign; no
    // other case has such a literal.
    const auto group = item[QStringLiteral("group")].toString();
    const auto text = item[QStringLiteral("expression")].toString();
    const bool literal = group == QStringLiteral("literal") || group == QStringLiteral("generated-literal");
    EXPECT_EQ(differences.plusExponentSign, literal && (text.contains(QStringLiteral("e+")) || text.contains(QStringLiteral("E+"))));
    differences.plusExponentSign = false;
    if (defect.isEmpty() || defect == QStringLiteral("tracker-code-limit"))
    {
        // Annotated cases are the only curated ones with constructs Tracker evaluates differently.
        if (!group.startsWith(QStringLiteral("generated")))
        {
            EXPECT_FALSE(differences.any());
        }
        return;
    }
    EXPECT_EQ(differences.negatedExponent, defect.contains(QStringLiteral("negated-exponent")));
    EXPECT_EQ(differences.powerProductInSum, defect.contains(QStringLiteral("power-product-in-sum")));
    EXPECT_EQ(differences.droppedNegation, defect.contains(QStringLiteral("dropped-negation")));
    EXPECT_EQ(differences.inverseHyperbolic, defect.contains(QStringLiteral("inverse-hyperbolic-precision")));
    EXPECT_EQ(differences.commonLogarithm, defect.contains(QStringLiteral("common-logarithm")));
}

void checkRandom(const Expression& expression, const QJsonObject& evaluation)
{
    const auto input = values(evaluation);
    const auto range = evaluation[QStringLiteral("range")].toArray();
    const double lower = javaText(range[0]);
    const double upper = javaText(range[1]);
    const bool nan = evaluation[QStringLiteral("nan")].toBool();
    for (int sample = 0; sample < RANDOM_SAMPLES; ++sample)
    {
        const double result = expression.evaluate(input);
        ASSERT_EQ(std::isnan(result), nan);
        if (!nan)
        {
            ASSERT_GE(result, lower);
            ASSERT_LE(result, upper);
        }
    }
}

void checkCase(const QJsonObject& item, ExpressionDialect dialect, int maxUlps)
{
    SCOPED_TRACE(dialect == ExpressionDialect::Tracker ? "Tracker dialect" : "corrected dialect");
    std::vector<std::string> variables;
    for (const auto& name : item[QStringLiteral("variables")].toArray())
    {
        variables.push_back(name.toString().toStdString());
    }
    const auto parsed =
        motrack::science::parseExpression(item[QStringLiteral("expression")].toString().toStdString(), variables, {.dialect = dialect});
    const auto error = item[QStringLiteral("error")].toObject();
    if (!error.isEmpty())
    {
        ASSERT_FALSE(parsed.hasValue());
        EXPECT_EQ(static_cast<int>(parsed.error().code), error[QStringLiteral("code")].toInt());
        EXPECT_EQ(parsed.error().position, static_cast<std::size_t>(error[QStringLiteral("position")].toInt()));
        EXPECT_EQ(QString::fromUtf8(motrack::science::errorMessage(parsed.error().code)), error[QStringLiteral("message")].toString());
        return;
    }
    const bool overflow = item[QStringLiteral("exception")].isString();
    if (overflow)
    {
        EXPECT_EQ(item[QStringLiteral("exception")].toString(), QStringLiteral("java.lang.ArrayIndexOutOfBoundsException"));
        if (dialect == ExpressionDialect::Tracker)
        {
            ASSERT_FALSE(parsed.hasValue());
            EXPECT_EQ(parsed.error().code, ExpressionErrorCode::TrackerCodeLimit);
            return;
        }
        // Without Tracker's limit, a case without an intended value must fail at its later syntax error.
        const auto first = item[QStringLiteral("evaluations")].toArray().first().toObject();
        if (!first.contains(QStringLiteral("intended_bits")))
        {
            ASSERT_FALSE(parsed.hasValue());
            EXPECT_EQ(parsed.error().code, ExpressionErrorCode::SyntaxError);
            return;
        }
    }
    ASSERT_TRUE(parsed.hasValue()) << static_cast<int>(parsed.error().code) << " at " << parsed.error().position;
    EXPECT_EQ(parsed->fitsTrackerLimits(), !overflow);
    checkDefects(item, *parsed);
    const auto compare = item[QStringLiteral("compare")].toString();
    for (const auto& entry : item[QStringLiteral("evaluations")].toArray())
    {
        const auto evaluation = entry.toObject();
        if (compare == QStringLiteral("random"))
        {
            checkRandom(*parsed, evaluation);
            continue;
        }
        const bool intended = dialect == ExpressionDialect::Corrected && evaluation.contains(QStringLiteral("intended_bits"));
        if (intended && !overflow)
        {
            EXPECT_TRUE(parsed->trackerDifferences().any());
        }
        const auto key = intended ? QStringLiteral("intended_bits") : QStringLiteral("result_bits");
        if (!evaluation.contains(key))
        {
            EXPECT_TRUE(overflow); // Java threw, and no independent value was recorded.
            continue;
        }
        const double expected = javaBits(evaluation[key]);
        const auto input = values(evaluation);
        EXPECT_TRUE(sameValue(expected, parsed->evaluate(input), compare == QStringLiteral("ulp"), maxUlps))
            << "at " << QJsonDocument(evaluation[QStringLiteral("values")].toArray()).toJson(QJsonDocument::Compact).toStdString();
    }
}

TEST(ExpressionReference, FixtureRecordsThePinnedParser)
{
    const auto& root = reference();
    ASSERT_EQ(root[QStringLiteral("schema_version")].toInt(), 1);
    const auto pinned = root[QStringLiteral("reference")].toObject();
    EXPECT_EQ(pinned[QStringLiteral("tracker_version")].toString(), QStringLiteral("6.3.5"));
    EXPECT_GT(root[QStringLiteral("cases")].toArray().size(), 600);
    const auto names = root[QStringLiteral("function_names")].toArray();
    ASSERT_EQ(names.size(), 30);
    for (qsizetype index = 0; index < names.size(); ++index)
    {
        const auto function = static_cast<motrack::science::BuiltinFunction>(index);
        EXPECT_EQ(QString::fromUtf8(motrack::science::functionName(function)), names[index].toString());
        EXPECT_TRUE(motrack::science::isReservedName(motrack::science::functionName(function)));
    }
    const auto messages = root[QStringLiteral("error_messages")].toObject();
    for (const int code : {1, 2, 4, 5, 6, 7, 11, 12, 13, 14, 15})
    {
        EXPECT_EQ(QString::fromUtf8(motrack::science::errorMessage(static_cast<ExpressionErrorCode>(code))),
                  messages[QString::number(code)].toString());
    }
}

TEST(ExpressionReference, BothDialectsMatchJavaOrTheIndependentIntendedValue)
{
    const auto& root = reference();
    const int maxUlps = root[QStringLiteral("tolerance")].toObject()[QStringLiteral("ulp")].toInt();
    ASSERT_GT(maxUlps, 0);
    for (const auto& entry : root[QStringLiteral("cases")].toArray())
    {
        const auto item = entry.toObject();
        SCOPED_TRACE(item[QStringLiteral("id")].toString().toStdString() + ": " +
                     item[QStringLiteral("expression")].toString().toStdString());
        checkCase(item, ExpressionDialect::Tracker, maxUlps);
        checkCase(item, ExpressionDialect::Corrected, maxUlps);
    }
}

/// trackerText() must make Tracker compute the corrected values. The Tracker dialect is checked against
/// Java above, so parsing the rendering with it shows what Tracker 6.3.5 computes from the exported text.
TEST(ExpressionReference, TrackerTextKeepsCorrectedValuesInTracker)
{
    int rendered = 0;
    for (const auto& entry : reference()[QStringLiteral("cases")].toArray())
    {
        const auto item = entry.toObject();
        SCOPED_TRACE(item[QStringLiteral("id")].toString().toStdString());
        std::vector<std::string> variables;
        for (const auto& name : item[QStringLiteral("variables")].toArray())
        {
            variables.push_back(name.toString().toStdString());
        }
        const auto text = item[QStringLiteral("expression")].toString().toStdString();
        const auto corrected = motrack::science::parseExpression(text, variables);
        const auto tracker = motrack::science::parseExpression(text, variables, {.dialect = ExpressionDialect::Tracker});
        if (!corrected)
        {
            continue;
        }
        if (tracker)
        {
            EXPECT_EQ(corrected->trackerCodeSize(), tracker->trackerCodeSize());
        }
        const auto differences = corrected->trackerDifferences();
        const auto exported =
            motrack::science::parseExpression(corrected->trackerText(), variables, {.dialect = ExpressionDialect::Tracker});
        if (!exported)
        {
            // Only the explicit negations of a near-limit expression can exceed Tracker's code limit.
            EXPECT_EQ(exported.error().code, ExpressionErrorCode::TrackerCodeLimit) << corrected->trackerText();
            EXPECT_TRUE(!corrected->fitsTrackerLimits() || differences.droppedNegation);
            continue;
        }
        EXPECT_FALSE(exported->trackerDifferences().negatedExponent || exported->trackerDifferences().powerProductInSum ||
                     exported->trackerDifferences().droppedNegation || exported->trackerDifferences().plusExponentSign)
            << corrected->trackerText();
        EXPECT_EQ(std::vector(exported->functions().begin(), exported->functions().end()),
                  std::vector(corrected->functions().begin(), corrected->functions().end()));
        if (differences.inverseHyperbolic || differences.commonLogarithm || corrected->usesRandom())
        {
            continue;
        }
        ++rendered;
        for (const auto& evaluation : item[QStringLiteral("evaluations")].toArray())
        {
            const auto input = values(evaluation.toObject());
            EXPECT_TRUE(sameValue(corrected->evaluate(input), exported->evaluate(input), false, 0)) << corrected->trackerText();
        }
    }
    EXPECT_GT(rendered, 300);
}

/// Tracker's models and fit functions parse through OSP's UserFunction, whose name padding splits a `+`
/// exponent (`1e+5` becomes `1e +5`): the function is rejected, evaluates 0 and still saves the typed text
/// (#215). Both dialects evaluate the literal as written, matching the plus-free equivalent through
/// UserFunction, and plusExponentSign predicts every rejection.
TEST(ExpressionReference, UserFunctionRejectsPlusExponents)
{
    const auto functions = reference()[QStringLiteral("user_functions")].toArray();
    ASSERT_GE(functions.size(), 15);
    int rejected = 0;
    for (const auto& entry : functions)
    {
        const auto item = entry.toObject();
        const auto text = item[QStringLiteral("expression")].toString();
        SCOPED_TRACE(item[QStringLiteral("id")].toString().toStdString() + ": " + text.toStdString());
        // UserFunction declares the independent variables, then the parameters.
        std::vector<std::string> names;
        for (const auto& name : item[QStringLiteral("variables")].toArray())
        {
            names.push_back(name.toString().toStdString());
        }
        for (const auto& name : item[QStringLiteral("parameters")].toArray())
        {
            names.push_back(name.toString().toStdString());
        }
        std::vector<double> parameterValues;
        for (const auto& value : item[QStringLiteral("parameter_values")].toArray())
        {
            parameterValues.push_back(javaText(value));
        }
        const bool valid = item[QStringLiteral("valid")].toBool();
        EXPECT_EQ(item[QStringLiteral("saved")].toString(), text);
        EXPECT_EQ(item[QStringLiteral("stored")].toString(), valid ? text : QStringLiteral("0"));
        rejected += valid ? 0 : 1;
        for (const auto dialect : {ExpressionDialect::Tracker, ExpressionDialect::Corrected})
        {
            SCOPED_TRACE(dialect == ExpressionDialect::Tracker ? "Tracker dialect" : "corrected dialect");
            const auto parsed = motrack::science::parseExpression(text.toStdString(), names, {.dialect = dialect});
            ASSERT_TRUE(parsed.hasValue());
            EXPECT_EQ(parsed->trackerDifferences(), (motrack::science::TrackerDifferences{.plusExponentSign = !valid}));
            const auto exported = motrack::science::parseExpression(parsed->trackerText(), names, {.dialect = ExpressionDialect::Tracker});
            ASSERT_TRUE(exported.hasValue()) << parsed->trackerText();
            EXPECT_FALSE(exported->trackerDifferences().any()) << parsed->trackerText();
            for (const auto& evaluationEntry : item[QStringLiteral("evaluations")].toArray())
            {
                const auto evaluation = evaluationEntry.toObject();
                auto input = values(evaluation);
                input.insert(input.end(), parameterValues.begin(), parameterValues.end());
                const double result = javaBits(evaluation[QStringLiteral("result_bits")]);
                if (!valid)
                {
                    EXPECT_EQ(std::bit_cast<std::uint64_t>(result), 0U); // Tracker's silent +0.
                }
                const double expected =
                    evaluation.contains(QStringLiteral("intended_bits")) ? javaBits(evaluation[QStringLiteral("intended_bits")]) : result;
                EXPECT_TRUE(sameValue(expected, parsed->evaluate(input), false, 0));
                EXPECT_TRUE(sameValue(expected, exported->evaluate(input), false, 0));
            }
        }
    }
    EXPECT_GE(rejected, 10);
}

} // namespace
