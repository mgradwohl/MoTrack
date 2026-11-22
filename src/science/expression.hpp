#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace motrack::science
{

/// Why an expression was rejected. OSP SuryonoParser errors keep their Java codes; MoTrack's bounds follow.
enum class ExpressionErrorCode : std::uint8_t
{
    SyntaxError = 1,
    ParenthesisExpected = 2,
    ExpressionExpected = 4,
    UnknownIdentifier = 5,
    OperatorExpected = 6,
    ParenthesesMismatch = 7,
    CommaExpected = 11,
    InvalidOperand = 12,
    InvalidOperator = 13,
    ReferenceDefinitionExpected = 14,
    ReferenceNameExpected = 15,
    TrackerCodeLimit = 100, ///< Tracker dialect: more than 99 postfix codes (Java throws an index exception).
    TooLong,                ///< Longer than ExpressionOptions::maxLength.
    TooDeep,                ///< Nested deeper than ExpressionOptions::maxNesting.
    InvalidEncoding,        ///< The expression or a variable name is not valid UTF-8.
};

struct ExpressionError
{
    ExpressionErrorCode code = ExpressionErrorCode::SyntaxError;
    /// OSP's one-based error position in UTF-16 code units of the expression after backslashes are
    /// removed; 0 for errors before parsing. OSP often reports the character after the offending token.
    std::size_t position = 0;

    bool operator==(const ExpressionError&) const = default;
};

/// OSP's English diagnostic (`SuryonoParser.toErrorString`) or MoTrack's wording for its own bounds.
/// User interfaces translate by code; these strings are for logs and tests.
[[nodiscard]] std::string_view errorMessage(ExpressionErrorCode code) noexcept;

/// Built-in functions in OSP's `getFunctionNames()` order. All angles are radians.
enum class BuiltinFunction : std::uint8_t
{
    Sin,
    Cos,
    Tan,
    Ln,
    Log,
    Abs,
    Int,
    Frac,
    Asin,
    Acos,
    Atan,
    Sinh,
    Cosh,
    Tanh,
    Asinh,
    Acosh,
    Atanh,
    Ceil,
    Floor,
    Round,
    Exp,
    Sqr,
    Sqrt,
    Sign,
    Step,
    Random,
    Min,
    Max,
    Mod,
    Atan2,
};

[[nodiscard]] std::string_view functionName(BuiltinFunction function) noexcept;

/// True for names the parser resolves before variables (`pi`, `e`, `if` and every function name), as in
/// OSP's FunctionTool.parserNames. A variable declared with such a name can never be referenced.
[[nodiscard]] bool isReservedName(std::string_view name) noexcept;

enum class ExpressionDialect : std::uint8_t
{
    /// Tracker's grammar and values, with the OSP defects described in TrackerDifferences fixed.
    Corrected,
    /// OSP SuryonoParser's values and errors as shipped in Tracker 6.3.5 (unchanged on upstream swingJS),
    /// bit for bit except: NaN is returned instead of 0 plus a flag, a wrong value count returns NaN, the
    /// 99-code overflow is TrackerCodeLimit rather than an exception, and ExpressionOptions bounds apply.
    Tracker,
};

/// Constructs whose Tracker 6.3.5 value differs from the corrected dialect. Detected for either dialect.
struct TrackerDifferences
{
    bool negatedExponent = false;   ///< `a^-b`: Tracker computes -(a^b).
    bool powerProductInSum = false; ///< `a+b^c*d`: Tracker computes (a+b^c)*d.
    bool droppedNegation = false;   ///< `a+-b`, `a--b`: Tracker ignores the sign unless ^, * or / follows b.
    bool inverseHyperbolic = false; ///< asinh/acosh/atanh: Tracker's formulas lose precision or overflow.
    bool commonLogarithm = false;   ///< log(x): Tracker divides ln(x) by ln(10), so log(1000) < 3.

    [[nodiscard]] bool any() const noexcept;

    bool operator==(const TrackerDifferences&) const = default;
};

struct ExpressionOptions
{
    ExpressionDialect dialect = ExpressionDialect::Corrected;
    std::size_t maxLength = 4096; ///< UTF-16 code units after backslash removal.
    std::size_t maxNesting = 128; ///< Parentheses, calls, relation and boolean operands, and power chains.
};

/// Uniform random numbers in [0, 1) for `random(x)`, which evaluates to x times one draw.
class RandomSource
{
  public:
    virtual ~RandomSource();

    [[nodiscard]] virtual double next() = 0;
};

namespace detail
{
struct ExpressionBuilder;
} // namespace detail

/// An immutable compiled expression. Copies share the compiled program; evaluation is const, allocates
/// only for unusually deep programs, and is safe to call concurrently from any thread.
class Expression
{
  public:
    /// Evaluates with values in declaration order. Returns NaN, never OSP's 0-for-NaN substitution, so
    /// callers emulating a Tracker context (parameters, UserFunction) must substitute explicitly. Returns
    /// NaN if the value count differs from variableCount(). `random` uses a per-thread generator.
    [[nodiscard]] double evaluate(std::span<const double> values) const;
    /// Evaluates with a caller-supplied source for `random`, e.g. a seeded or recorded sequence.
    [[nodiscard]] double evaluate(std::span<const double> values, RandomSource& random) const;

    [[nodiscard]] std::size_t variableCount() const noexcept;
    /// Indices of declared variables the text references, ascending and unique, including those that
    /// appear only in an untaken if() branch. Use them for dependency graphs.
    [[nodiscard]] std::span<const std::size_t> referencedVariables() const noexcept;
    [[nodiscard]] bool referencesVariable(std::size_t index) const noexcept;
    /// Built-in functions the text calls, in enumeration order and unique.
    [[nodiscard]] std::span<const BuiltinFunction> functions() const noexcept;
    /// True if `random` is called, so results must not be cached as a pure function of the inputs.
    [[nodiscard]] bool usesRandom() const noexcept;
    [[nodiscard]] TrackerDifferences trackerDifferences() const noexcept;
    /// Number of postfix codes SuryonoParser compiles for this text in either dialect; Tracker cannot parse
    /// more than 99.
    [[nodiscard]] std::size_t trackerCodeSize() const noexcept;
    [[nodiscard]] bool fitsTrackerLimits() const noexcept;
    /// Fully parenthesized text that Tracker 6.3.5 parses into this expression's operations (for the
    /// corrected dialect, its corrected meaning), for writing an expression to a Tracker file: no precedence
    /// defect applies to it. Literals and names are kept; spaces are not. Tracker still computes its own
    /// asinh, acosh, atanh and log (trackerDifferences()), and the explicit negations the corrected dialect
    /// adds can push it past Tracker's code limit; parse the result with the Tracker dialect to check.
    [[nodiscard]] std::string trackerText() const;
    [[nodiscard]] ExpressionDialect dialect() const noexcept;
    /// The UTF-8 text as given to parseExpression.
    [[nodiscard]] const std::string& text() const noexcept;

  private:
    struct Program;
    friend struct detail::ExpressionBuilder; // The only code that creates programs.
    explicit Expression(std::shared_ptr<const Program> program) noexcept;

    std::shared_ptr<const Program> m_Program;
};

/// An Expression or the reason it was rejected; the subset of std::expected that callers need. CI's Linux
/// toolchain (Clang 18 with libstdc++) does not provide std::expected because Clang 18 reports
/// __cpp_concepts as 201907L. Accessing the absent alternative throws std::bad_variant_access.
class ExpressionResult
{
  public:
    // Implicit, like std::expected, so parseExpression can return either alternative directly.
    ExpressionResult(Expression expression) noexcept;
    ExpressionResult(ExpressionError error) noexcept;

    [[nodiscard]] bool hasValue() const noexcept;
    explicit operator bool() const noexcept;
    [[nodiscard]] const Expression& operator*() const;
    [[nodiscard]] const Expression* operator->() const;
    [[nodiscard]] const ExpressionError& error() const;

  private:
    std::variant<Expression, ExpressionError> m_Value;
};

/// Compiles an OSP expression over the declared variables (UTF-8, matched exactly and case-sensitively;
/// the first of duplicate names wins and names containing operator characters never match).
[[nodiscard]] ExpressionResult
parseExpression(std::string_view text, std::span<const std::string_view> variables, const ExpressionOptions& options = {});
[[nodiscard]] ExpressionResult
parseExpression(std::string_view text, std::span<const std::string> variables, const ExpressionOptions& options = {});

} // namespace motrack::science
