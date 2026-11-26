#include "science/expression.hpp"

#include "science/decimal_conversion.hpp"
#include "science/java_math.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <iterator>
#include <limits>
#include <numbers>
#include <optional>
#include <random>
#include <utility>
#include <variant>
#include <vector>

// Java never fuses a*b+c into a single rounding. Keep Tracker's formulas bit-identical on FMA targets.
#pragma STDC FP_CONTRACT OFF

namespace motrack::science
{
namespace detail
{

enum class OpCode : std::uint8_t
{
    Number,
    Variable,
    Negate,
    Add,
    Subtract,
    Multiply,
    Divide,
    Power,
    Less,
    Greater,
    LessEqual,
    GreaterEqual,
    Equal,
    NotEqual,
    And,
    Or,
    Not,
    Unary,
    Binary,
    JumpIfZero, ///< Pops the condition; skips `operand` instructions (the true branch) when it is zero.
    Jump,       ///< Skips `operand` instructions (the false branch).
};

struct Instruction
{
    OpCode op = OpCode::Number;
    BuiltinFunction function = BuiltinFunction::Sin;
    std::uint32_t operand = 0; ///< Variable index, literal index or instruction count to skip.
    double value = 0.0;
};

/// A run of compiled code. `width` counts SuryonoParser postfix codes, including jump displacements.
struct Segment
{
    std::vector<Instruction> code;
    std::size_t width = 0;
};

/// Everything a successful compile contributes to a program.
struct Compilation
{
    Segment segment;
    TrackerDifferences differences;
    std::size_t correctedOnlyCodes = 0; ///< Negations Tracker would drop; not part of its code size.
    std::vector<std::string> literals;  ///< Source text of each number and constant.
};

struct ExpressionBuilder
{
    static Expression
    build(std::string_view text, std::span<const std::string_view> variables, ExpressionDialect dialect, Compilation compilation);
};

} // namespace detail

struct Expression::Program
{
    std::string text;
    std::vector<detail::Instruction> code;
    std::vector<std::string> literals;
    std::vector<std::string> variableNames;
    std::vector<std::size_t> variables;
    std::vector<BuiltinFunction> functions;
    std::size_t variableCount = 0;
    std::size_t stackSize = 0;
    std::size_t trackerCodeSize = 0;
    TrackerDifferences differences;
    ExpressionDialect dialect = ExpressionDialect::Corrected;
    bool usesRandom = false;

    double evaluate(std::span<const double> values, RandomSource* random) const;
    double run(std::span<const double> values, RandomSource* random, std::span<double> stack) const;
    [[nodiscard]] std::string render(std::size_t begin, std::size_t end) const;
};

namespace
{

using detail::Instruction;
using detail::OpCode;
using detail::Segment;

/// SuryonoParser compiles into an int[100] whose first entry holds the code count.
constexpr std::size_t TRACKER_CODE_LIMIT = 99;
constexpr std::size_t INLINE_STACK = 64;
constexpr double QUIET_NAN = std::numeric_limits<double>::quiet_NaN();

constexpr std::array<std::string_view, 30> FUNCTION_NAMES = {
    "sin",   "cos",   "tan",  "ln",    "log",   "abs", "int", "frac", "asin", "acos", "atan",   "sinh", "cosh", "tanh", "asinh",
    "acosh", "atanh", "ceil", "floor", "round", "exp", "sqr", "sqrt", "sign", "step", "random", "min",  "max",  "mod",  "atan2"};

std::optional<std::u16string> toUtf16(std::string_view text)
{
    std::u16string result;
    result.reserve(text.size());
    for (std::size_t i = 0; i < text.size();)
    {
        const auto lead = static_cast<unsigned char>(text[i]);
        if (lead < 0x80U)
        {
            result.push_back(lead);
            ++i;
            continue;
        }
        std::size_t length = 0;
        char32_t point = 0;
        char32_t minimum = 0;
        if (lead >= 0xC2U && lead <= 0xDFU)
        {
            length = 2;
            point = lead & 0x1FU;
            minimum = 0x80;
        }
        else if (lead >= 0xE0U && lead <= 0xEFU)
        {
            length = 3;
            point = lead & 0x0FU;
            minimum = 0x800;
        }
        else if (lead >= 0xF0U && lead <= 0xF4U)
        {
            length = 4;
            point = lead & 0x07U;
            minimum = 0x10000;
        }
        else
        {
            return std::nullopt;
        }
        if (text.size() - i < length)
        {
            return std::nullopt;
        }
        for (std::size_t k = 1; k < length; ++k)
        {
            const auto continuation = static_cast<unsigned char>(text[i + k]);
            if ((continuation & 0xC0U) != 0x80U)
            {
                return std::nullopt;
            }
            point = (point << 6U) | (continuation & 0x3FU);
        }
        if (point < minimum || point > 0x10FFFFU || (point >= 0xD800U && point <= 0xDFFFU))
        {
            return std::nullopt;
        }
        if (point >= 0x10000U)
        {
            point -= 0x10000U;
            result.push_back(static_cast<char16_t>(0xD800U + (point >> 10U)));
            result.push_back(static_cast<char16_t>(0xDC00U + (point & 0x3FFU)));
        }
        else
        {
            result.push_back(static_cast<char16_t>(point));
        }
        i += length;
    }
    return result;
}

bool equalsAscii(std::u16string_view wide, std::string_view ascii) noexcept
{
    return std::ranges::equal(wide, ascii, [](char16_t a, char b) { return a == static_cast<unsigned char>(b); });
}

bool isDigit(char16_t c) noexcept
{
    return c >= u'0' && c <= u'9';
}

/// Characters that end an identifier (SuryonoParser.scanNonNumeric).
bool isDelimiter(char16_t c) noexcept
{
    return std::u16string_view(u" +-*/^(),<>=&|").find(c) != std::u16string_view::npos;
}

/// Java String.trim(): removes leading and trailing characters up to U+0020.
std::u16string_view javaTrim(std::u16string_view text) noexcept
{
    while (!text.empty() && text.front() <= u' ')
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && text.back() <= u' ')
    {
        text.remove_suffix(1);
    }
    return text;
}

/// Unwinds the recursive descent to parseSubFunction, like SuryonoParser's ParserException.
class ParseStop : public std::exception
{
  public:
    explicit ParseStop(ExpressionErrorCode code) noexcept : m_Code(code)
    {}

    [[nodiscard]] ExpressionErrorCode code() const noexcept
    {
        return m_Code;
    }

  private:
    ExpressionErrorCode m_Code;
};

/// A structural port of SuryonoParser's recursive descent, including its global boolean/relation flags,
/// error codes and positions. The corrected dialect differs only where noted in TrackerDifferences.
class Compiler
{
  public:
    Compiler(std::span<const std::u16string> variables, const ExpressionOptions& options) noexcept
        : m_Variables(variables), m_Options(options)
    {}

    std::variant<Segment, ExpressionError> compile(std::u16string_view text);

    [[nodiscard]] const TrackerDifferences& differences() const noexcept
    {
        return m_Differences;
    }

    [[nodiscard]] std::size_t correctedOnlyCodes() const noexcept
    {
        return m_CorrectedOnlyCodes;
    }

    [[nodiscard]] std::vector<std::string> takeLiterals() noexcept
    {
        return std::move(m_Literals);
    }

  private:
    class Nesting
    {
      public:
        explicit Nesting(Compiler& compiler) : m_Compiler(compiler)
        {
            if (++m_Compiler.m_Depth > m_Compiler.m_Options.maxNesting)
            {
                throw ParseStop(ExpressionErrorCode::TooDeep);
            }
        }
        Nesting(const Nesting&) = delete;
        Nesting& operator=(const Nesting&) = delete;
        Nesting(Nesting&&) = delete;
        Nesting& operator=(Nesting&&) = delete;
        ~Nesting()
        {
            --m_Compiler.m_Depth;
        }

      private:
        Compiler& m_Compiler;
    };

    [[nodiscard]] bool corrected() const noexcept
    {
        return m_Options.dialect == ExpressionDialect::Corrected;
    }

    std::optional<ExpressionError> parseSubFunction();
    void next();
    void skipSpaces();
    void reserve(Segment& segment, std::size_t width) const;
    void emit(Instruction instruction);
    void append(Segment& target, const Segment& source) const;
    bool getIdentifier();
    void scanAndParse();
    void arithmeticLevel1();
    void arithmeticLevel2();
    void arithmeticLevel3();
    void relationLevel();
    void booleanLevel();
    void scanNumber();
    void scanNonNumeric();
    void parseIf();
    void parseCall(BuiltinFunction function);

    std::span<const std::u16string> m_Variables;
    ExpressionOptions m_Options;
    std::u16string m_Function;
    std::ptrdiff_t m_Position = 0; ///< SuryonoParser.position: one past the current character.
    std::ptrdiff_t m_Start = 0;
    char16_t m_Ch = 0;
    bool m_IsBoolean = false;
    bool m_InRelation = false;
    std::size_t m_Depth = 0;
    Segment m_Segment;
    TrackerDifferences m_Differences;
    std::size_t m_CorrectedOnlyCodes = 0;
    std::vector<std::string> m_Literals;
};

std::variant<Segment, ExpressionError> Compiler::compile(std::u16string_view text)
{
    if (text.empty())
    {
        return ExpressionError{.code = ExpressionErrorCode::ExpressionExpected, .position = 0};
    }
    std::u16string all(text);
    // OSP first splits `;name:definition` segments off the end. Its loop keeps the ';' it split on, so the
    // following pass always sees an empty definition: every expression containing ';' is rejected.
    for (auto semicolon = all.rfind(u';'); semicolon != std::u16string::npos; semicolon = all.rfind(u';'))
    {
        auto offset = semicolon + 1;
        m_Function = all.substr(offset) + u')';
        all.resize(offset);
        ExpressionError error;
        const auto separator = m_Function.find(u':');
        if (separator == std::u16string::npos)
        {
            error = {.code = ExpressionErrorCode::ReferenceDefinitionExpected, .position = m_Function.find_first_not_of(u' ') + 1};
        }
        else if (javaTrim(std::u16string_view(m_Function).substr(0, separator)).empty())
        {
            error = {.code = ExpressionErrorCode::ReferenceNameExpected, .position = 1};
        }
        else
        {
            m_Function.erase(0, separator + 1);
            offset += separator + 1;
            const auto failure = parseSubFunction();
            if (!failure)
            {
                continue; // A definition that parses is still unusable; the next pass reports the error.
            }
            error = *failure;
        }
        error.position += offset;
        return error;
    }
    m_Function = all + u')';
    if (const auto failure = parseSubFunction())
    {
        return *failure;
    }
    return std::move(m_Segment);
}

std::optional<ExpressionError> Compiler::parseSubFunction()
{
    m_Position = 0;
    m_Segment = {};
    m_InRelation = false;
    m_IsBoolean = false;
    m_Depth = 0;
    try
    {
        scanAndParse();
    }
    catch (const ParseStop& stop)
    {
        auto code = stop.code();
        if (code == ExpressionErrorCode::SyntaxError && m_Segment.code.empty())
        {
            code = ExpressionErrorCode::ExpressionExpected;
        }
        return ExpressionError{.code = code, .position = static_cast<std::size_t>(m_Position)};
    }
    if (m_Position != std::ssize(m_Function))
    {
        return ExpressionError{.code = ExpressionErrorCode::ParenthesesMismatch, .position = static_cast<std::size_t>(m_Position)};
    }
    return std::nullopt;
}

void Compiler::next()
{
    // Java increments the position before charAt() reports the end of the text.
    const auto index = m_Position++;
    if (index >= std::ssize(m_Function))
    {
        throw ParseStop(ExpressionErrorCode::ParenthesesMismatch);
    }
    m_Ch = m_Function[static_cast<std::size_t>(index)];
}

void Compiler::skipSpaces()
{
    // Every caller has just read a character, and the appended ')' always ends the spaces, so OSP's
    // end-of-text error cannot occur here; at() still guards the invariant.
    const auto index = m_Function.find_first_not_of(u' ', static_cast<std::size_t>(m_Position - 1));
    m_Ch = m_Function.at(index);
    m_Position = static_cast<std::ptrdiff_t>(index) + 1;
}

void Compiler::reserve(Segment& segment, std::size_t width) const
{
    if (!corrected() && segment.width + width > TRACKER_CODE_LIMIT)
    {
        throw ParseStop(ExpressionErrorCode::TrackerCodeLimit);
    }
    segment.width += width;
}

void Compiler::emit(Instruction instruction)
{
    reserve(m_Segment, 1);
    m_Segment.code.push_back(instruction);
}

void Compiler::append(Segment& target, const Segment& source) const
{
    reserve(target, source.width);
    target.code.insert(target.code.end(), source.code.begin(), source.code.end());
}

bool Compiler::getIdentifier()
{
    bool negate = false;
    next();
    skipSpaces();
    if (m_Ch == u'!')
    {
        next();
        skipSpaces();
        if (m_Ch != u'(')
        {
            throw ParseStop(ExpressionErrorCode::ParenthesisExpected);
        }
        scanAndParse();
        if (m_Ch != u')')
        {
            throw ParseStop(ExpressionErrorCode::ParenthesisExpected);
        }
        if (!m_IsBoolean)
        {
            throw ParseStop(ExpressionErrorCode::InvalidOperand);
        }
        emit({.op = OpCode::Not});
        next(); // OSP does not skip spaces after !(...).
        return false;
    }
    m_IsBoolean = false;
    while (m_Ch == u'+' || m_Ch == u'-')
    {
        negate = m_Ch == u'-' ? !negate : negate;
        next();
        skipSpaces();
    }
    m_Start = m_Position;
    if (isDigit(m_Ch) || m_Ch == u'.')
    {
        scanNumber();
    }
    else if (m_Ch == u'(')
    {
        scanAndParse();
        next();
    }
    else
    {
        scanNonNumeric();
    }
    skipSpaces();
    return negate;
}

void Compiler::scanAndParse()
{
    const Nesting nesting(*this);
    const bool negate = getIdentifier();
    if (m_Ch != u'^' && negate)
    {
        emit({.op = OpCode::Negate});
    }
    while (true)
    {
        switch (m_Ch)
        {
        case u'+':
        case u'-':
            arithmeticLevel1();
            break;
        case u'*':
        case u'/':
            arithmeticLevel2();
            break;
        case u'^':
            arithmeticLevel3();
            if (negate)
            {
                emit({.op = OpCode::Negate});
            }
            break;
        case u',':
        case u')':
            return;
        case u'=':
        case u'<':
        case u'>':
            relationLevel();
            break;
        case u'&':
        case u'|':
            booleanLevel();
            break;
        default:
            throw ParseStop(ExpressionErrorCode::OperatorExpected);
        }
    }
}

void Compiler::arithmeticLevel1()
{
    if (m_IsBoolean)
    {
        throw ParseStop(ExpressionErrorCode::InvalidOperand);
    }
    do
    {
        const auto op = m_Ch == u'+' ? OpCode::Add : OpCode::Subtract;
        const bool negate = getIdentifier();
        if (m_IsBoolean)
        {
            throw ParseStop(ExpressionErrorCode::InvalidOperand);
        }
        if (m_Ch == u'^')
        {
            arithmeticLevel3();
            if (negate)
            {
                emit({.op = OpCode::Negate});
            }
            if (m_Ch == u'*' || m_Ch == u'/')
            {
                // OSP ends the term here, so the product applies to the whole preceding sum.
                m_Differences.powerProductInSum = true;
                if (corrected())
                {
                    arithmeticLevel2();
                }
            }
        }
        else if (m_Ch == u'*' || m_Ch == u'/')
        {
            if (negate)
            {
                emit({.op = OpCode::Negate});
            }
            arithmeticLevel2();
        }
        else if (negate)
        {
            // OSP only negates a +/- operand that is followed by ^, * or /.
            m_Differences.droppedNegation = true;
            if (corrected())
            {
                emit({.op = OpCode::Negate});
                ++m_CorrectedOnlyCodes;
            }
        }
        emit({.op = op});
    } while (m_Ch == u'+' || m_Ch == u'-');
}

void Compiler::arithmeticLevel2()
{
    if (m_IsBoolean)
    {
        throw ParseStop(ExpressionErrorCode::InvalidOperand);
    }
    do
    {
        const auto op = m_Ch == u'*' ? OpCode::Multiply : OpCode::Divide;
        const bool negate = getIdentifier();
        if (m_IsBoolean)
        {
            throw ParseStop(ExpressionErrorCode::InvalidOperand);
        }
        if (m_Ch == u'^')
        {
            arithmeticLevel3();
        }
        if (negate)
        {
            emit({.op = OpCode::Negate});
        }
        emit({.op = op});
    } while (m_Ch == u'*' || m_Ch == u'/');
}

void Compiler::arithmeticLevel3()
{
    const Nesting nesting(*this);
    if (m_IsBoolean)
    {
        throw ParseStop(ExpressionErrorCode::InvalidOperand);
    }
    const bool negate = getIdentifier();
    if (m_IsBoolean)
    {
        throw ParseStop(ExpressionErrorCode::InvalidOperand);
    }
    if (m_Ch == u'^')
    {
        arithmeticLevel3();
    }
    if (negate)
    {
        // OSP negates after the power, so a^-b becomes -(a^b).
        m_Differences.negatedExponent = true;
    }
    if (negate && corrected())
    {
        emit({.op = OpCode::Negate});
    }
    emit({.op = OpCode::Power});
    if (negate && !corrected())
    {
        emit({.op = OpCode::Negate});
    }
}

void Compiler::relationLevel()
{
    if (m_InRelation)
    {
        throw ParseStop(ExpressionErrorCode::InvalidOperator);
    }
    m_InRelation = true;
    if (m_IsBoolean)
    {
        throw ParseStop(ExpressionErrorCode::InvalidOperand);
    }
    auto op = OpCode::Equal;
    if (m_Ch == u'<')
    {
        op = OpCode::Less;
        next();
        if (m_Ch == u'>')
        {
            op = OpCode::NotEqual;
        }
        else if (m_Ch == u'=')
        {
            op = OpCode::LessEqual;
        }
        else
        {
            --m_Position;
        }
    }
    else if (m_Ch == u'>')
    {
        op = OpCode::Greater;
        next();
        if (m_Ch == u'=')
        {
            op = OpCode::GreaterEqual;
        }
        else
        {
            --m_Position;
        }
    }
    scanAndParse();
    m_InRelation = false;
    // OSP rejects a boolean right operand here too, but one would need a relation inside this relation,
    // which already failed with InvalidOperator.
    emit({.op = op});
    m_IsBoolean = true;
}

void Compiler::booleanLevel()
{
    if (!m_IsBoolean)
    {
        throw ParseStop(ExpressionErrorCode::InvalidOperand);
    }
    const auto op = m_Ch == u'&' ? OpCode::And : OpCode::Or;
    scanAndParse();
    if (!m_IsBoolean)
    {
        throw ParseStop(ExpressionErrorCode::InvalidOperand);
    }
    emit({.op = op});
}

void Compiler::scanNumber()
{
    // OSP's 200-constant limit is unreachable: a successful parse has at most 99 codes.
    std::string text;
    if (m_Ch != u'.')
    {
        do
        {
            text.push_back(static_cast<char>(m_Ch));
            next();
        } while (isDigit(m_Ch));
    }
    else
    {
        text.push_back('0');
    }
    if (m_Ch == u'.')
    {
        do
        {
            text.push_back(static_cast<char>(m_Ch));
            next();
        } while (isDigit(m_Ch));
    }
    bool plusSign = false;
    if (m_Ch == u'e' || m_Ch == u'E')
    {
        text.push_back(static_cast<char>(m_Ch));
        next();
        if (m_Ch == u'+' || m_Ch == u'-')
        {
            plusSign = m_Ch == u'+';
            text.push_back(static_cast<char>(m_Ch));
            next();
        }
        while (isDigit(m_Ch))
        {
            text.push_back(static_cast<char>(m_Ch));
            next();
        }
    }
    const auto value = parseJavaDecimal(text);
    if (!value)
    {
        m_Position = m_Start;
        throw ParseStop(ExpressionErrorCode::SyntaxError);
    }
    // UserFunction.padNames splits `1e+5` into `1e +5`, which fails to parse, so Tracker's models and fit
    // functions become 0. Report the construct, and export the literal without the sign it never needs.
    if (plusSign)
    {
        m_Differences.plusExponentSign = true;
        std::erase(text, '+');
    }
    m_Literals.push_back(std::move(text));
    emit({.op = OpCode::Number, .operand = static_cast<std::uint32_t>(m_Literals.size() - 1), .value = *value});
}

void Compiler::scanNonNumeric()
{
    if (std::u16string_view(u"*/^),<>=&|").find(m_Ch) != std::u16string_view::npos)
    {
        throw ParseStop(ExpressionErrorCode::SyntaxError);
    }
    std::u16string name;
    do
    {
        name.push_back(m_Ch);
        next();
    } while (!isDelimiter(m_Ch));
    if (name == u"pi")
    {
        m_Literals.emplace_back("pi");
        emit({.op = OpCode::Number, .operand = static_cast<std::uint32_t>(m_Literals.size() - 1), .value = std::numbers::pi});
        return;
    }
    if (name == u"e")
    {
        m_Literals.emplace_back("e");
        emit({.op = OpCode::Number, .operand = static_cast<std::uint32_t>(m_Literals.size() - 1), .value = std::numbers::e});
        return;
    }
    if (name == u"if")
    {
        parseIf();
        return;
    }
    if (const auto function = std::ranges::find_if(FUNCTION_NAMES, [&](std::string_view known) { return equalsAscii(name, known); });
        function != FUNCTION_NAMES.end())
    {
        parseCall(static_cast<BuiltinFunction>(function - FUNCTION_NAMES.begin()));
        return;
    }
    if (const auto variable = std::ranges::find(m_Variables, name); variable != m_Variables.end())
    {
        emit({.op = OpCode::Variable, .operand = static_cast<std::uint32_t>(variable - m_Variables.begin())});
        return;
    }
    m_Position = m_Start;
    throw ParseStop(ExpressionErrorCode::UnknownIdentifier);
}

void Compiler::parseIf()
{
    // Layout: condition, JumpIfZero, true branch, Jump, false branch. OSP adds each jump's displacement
    // code to the saved buffer only after parsing the branch, which decides when its code limit is hit.
    skipSpaces();
    if (m_Ch != u'(')
    {
        throw ParseStop(ExpressionErrorCode::ParenthesisExpected);
    }
    scanAndParse();
    if (m_Ch != u',')
    {
        throw ParseStop(ExpressionErrorCode::CommaExpected);
    }
    emit({.op = OpCode::JumpIfZero});
    Segment saved = std::exchange(m_Segment, {});
    const auto branch = saved.code.size() - 1;
    scanAndParse();
    if (m_Ch != u',')
    {
        throw ParseStop(ExpressionErrorCode::CommaExpected);
    }
    emit({.op = OpCode::Jump});
    reserve(saved, 1);
    saved.code[branch].operand = static_cast<std::uint32_t>(m_Segment.code.size());
    const auto jump = saved.code.size() + m_Segment.code.size() - 1;
    append(saved, m_Segment);
    m_Segment = {};
    scanAndParse();
    if (m_Ch != u')')
    {
        throw ParseStop(ExpressionErrorCode::ParenthesisExpected);
    }
    reserve(saved, 1);
    saved.code[jump].operand = static_cast<std::uint32_t>(m_Segment.code.size());
    append(saved, m_Segment);
    m_Segment = std::move(saved);
    next();
}

void Compiler::parseCall(BuiltinFunction function)
{
    skipSpaces();
    if (m_Ch != u'(')
    {
        throw ParseStop(ExpressionErrorCode::ParenthesisExpected);
    }
    scanAndParse();
    if (function < BuiltinFunction::Min)
    {
        if (m_Ch != u')')
        {
            throw ParseStop(ExpressionErrorCode::ParenthesisExpected);
        }
        next();
        emit({.op = OpCode::Unary, .function = function});
        return;
    }
    if (m_Ch != u',')
    {
        throw ParseStop(ExpressionErrorCode::CommaExpected);
    }
    Segment first = std::exchange(m_Segment, {});
    scanAndParse();
    if (m_Ch != u')')
    {
        throw ParseStop(ExpressionErrorCode::ParenthesisExpected);
    }
    next();
    append(first, m_Segment);
    m_Segment = std::move(first);
    emit({.op = OpCode::Binary, .function = function});
}

double call(BuiltinFunction function, double a, double b, ExpressionDialect dialect, RandomSource* random)
{
    const bool tracker = dialect == ExpressionDialect::Tracker;
    switch (function)
    {
    case BuiltinFunction::Sin:
        return std::sin(a);
    case BuiltinFunction::Cos:
        return std::cos(a);
    case BuiltinFunction::Tan:
        return std::tan(a);
    case BuiltinFunction::Ln:
        return std::log(a);
    case BuiltinFunction::Log:
        return tracker ? std::log(a) / std::numbers::ln10 : std::log10(a); // OSP divides by Math.log(10).
    case BuiltinFunction::Abs:
        return std::fabs(a);
    case BuiltinFunction::Int:
        return java::rint(a); // Nearest integer, ties to even.
    case BuiltinFunction::Frac:
        return a - java::rint(a);
    case BuiltinFunction::Asin:
        return std::asin(a);
    case BuiltinFunction::Acos:
        return std::acos(a);
    case BuiltinFunction::Atan:
        return std::atan(a);
    case BuiltinFunction::Sinh:
        return std::sinh(a);
    case BuiltinFunction::Cosh:
        return std::cosh(a);
    case BuiltinFunction::Tanh:
        return std::tanh(a);
    case BuiltinFunction::Asinh:
        return tracker ? std::log(a + std::sqrt((a * a) + 1)) : std::asinh(a);
    case BuiltinFunction::Acosh:
        return tracker ? std::log(a + std::sqrt((a * a) - 1)) : std::acosh(a);
    case BuiltinFunction::Atanh:
        return tracker ? std::log((1 + a) / (1 - a)) / 2 : std::atanh(a);
    case BuiltinFunction::Ceil:
        return std::ceil(a);
    case BuiltinFunction::Floor:
        return std::floor(a);
    case BuiltinFunction::Round:
        return java::round(a);
    case BuiltinFunction::Exp:
        return std::exp(a);
    case BuiltinFunction::Sqr:
        return a * a;
    case BuiltinFunction::Sqrt:
        return std::sqrt(a);
    case BuiltinFunction::Sign:
        return java::signum(a);
    case BuiltinFunction::Step:
        return a < 0 ? 0.0 : 1.0;
    case BuiltinFunction::Random:
        return a * random->next();
    case BuiltinFunction::Min:
        return java::min(a, b);
    case BuiltinFunction::Max:
        return java::max(a, b);
    case BuiltinFunction::Mod:
        return java::ieeeRemainder(a, b);
    case BuiltinFunction::Atan2:
        return std::atan2(a, b);
    }
    return QUIET_NAN;
}

double truth(bool value) noexcept
{
    return value ? 1.0 : 0.0;
}

class ThreadRandom final : public RandomSource
{
  public:
    ThreadRandom() : m_Engine(std::random_device{}())
    {}

    double next() override
    {
        return static_cast<double>(m_Engine() >> 11U) * 0x1.0p-53; // 53 random bits in [0, 1)
    }

  private:
    std::mt19937_64 m_Engine;
};

RandomSource& threadRandom()
{
    thread_local ThreadRandom source;
    return source;
}

} // namespace

double Expression::Program::run(std::span<const double> values, RandomSource* random, std::span<double> stack) const
{
    std::size_t top = 0; // number of stack entries
    const auto binary = [&](auto operation)
    {
        --top;
        stack[top - 1] = operation(stack[top - 1], stack[top]);
    };
    for (std::size_t pc = 0; pc < code.size(); ++pc)
    {
        const Instruction& instruction = code[pc];
        switch (instruction.op)
        {
        case OpCode::Number:
            stack[top++] = instruction.value;
            break;
        case OpCode::Variable:
            stack[top++] = values[instruction.operand];
            break;
        case OpCode::Negate:
            stack[top - 1] = -stack[top - 1];
            break;
        case OpCode::Add:
            binary([](double a, double b) { return a + b; });
            break;
        case OpCode::Subtract:
            binary([](double a, double b) { return a - b; });
            break;
        case OpCode::Multiply:
            binary([](double a, double b) { return a * b; });
            break;
        case OpCode::Divide:
            binary([](double a, double b) { return a / (b == 0 ? ZERO_DIVISOR_REPLACEMENT : b); });
            break;
        case OpCode::Power:
            binary([](double a, double b) { return java::pow(a, b); });
            break;
        case OpCode::Less:
            binary([](double a, double b) { return truth(a < b); });
            break;
        case OpCode::Greater:
            binary([](double a, double b) { return truth(a > b); });
            break;
        case OpCode::LessEqual:
            binary([](double a, double b) { return truth(a <= b); });
            break;
        case OpCode::GreaterEqual:
            binary([](double a, double b) { return truth(a >= b); });
            break;
        case OpCode::Equal:
            binary([](double a, double b) { return truth(a == b); });
            break;
        case OpCode::NotEqual:
            binary([](double a, double b) { return truth(a != b); });
            break;
        case OpCode::And:
            binary([](double a, double b) { return truth(a != 0 && b != 0); });
            break;
        case OpCode::Or:
            binary([](double a, double b) { return truth(a != 0 || b != 0); });
            break;
        case OpCode::Not:
            stack[top - 1] = truth(stack[top - 1] == 0);
            break;
        case OpCode::Unary:
            stack[top - 1] = call(instruction.function, stack[top - 1], 0.0, dialect, random);
            break;
        case OpCode::Binary:
            --top;
            stack[top - 1] = call(instruction.function, stack[top - 1], stack[top], dialect, random);
            break;
        case OpCode::JumpIfZero:
            --top;
            pc += stack[top] == 0 ? instruction.operand : 0; // NaN selects the true branch, as in OSP.
            break;
        case OpCode::Jump:
            pc += instruction.operand;
            break;
        }
    }
    return stack[0];
}

Expression::Expression(std::shared_ptr<const Program> program) noexcept : m_Program(std::move(program))
{}

RandomSource::~RandomSource() = default;

bool TrackerDifferences::any() const noexcept
{
    return negatedExponent || powerProductInSum || droppedNegation || inverseHyperbolic || commonLogarithm || plusExponentSign;
}

ExpressionResult::ExpressionResult(Expression expression) noexcept : m_Value(std::move(expression))
{}

ExpressionResult::ExpressionResult(ExpressionError error) noexcept : m_Value(error)
{}

bool ExpressionResult::hasValue() const noexcept
{
    return std::holds_alternative<Expression>(m_Value);
}

ExpressionResult::operator bool() const noexcept
{
    return hasValue();
}

const Expression& ExpressionResult::operator*() const
{
    return std::get<Expression>(m_Value);
}

const Expression* ExpressionResult::operator->() const
{
    return &std::get<Expression>(m_Value);
}

const ExpressionError& ExpressionResult::error() const
{
    return std::get<ExpressionError>(m_Value);
}

double Expression::Program::evaluate(std::span<const double> values, RandomSource* random) const
{
    if (values.size() != variableCount)
    {
        return QUIET_NAN;
    }
    if (stackSize <= INLINE_STACK)
    {
        std::array<double, INLINE_STACK> stack{};
        return run(values, random, stack);
    }
    std::vector<double> stack(stackSize);
    return run(values, random, stack);
}

double Expression::evaluate(std::span<const double> values) const
{
    // Only programs that call random() touch the per-thread generator.
    return m_Program->evaluate(values, m_Program->usesRandom ? &threadRandom() : nullptr);
}

double Expression::evaluate(std::span<const double> values, RandomSource& random) const
{
    return m_Program->evaluate(values, &random);
}

std::size_t Expression::variableCount() const noexcept
{
    return m_Program->variableCount;
}

std::span<const std::size_t> Expression::referencedVariables() const noexcept
{
    return m_Program->variables;
}

bool Expression::referencesVariable(std::size_t index) const noexcept
{
    return std::ranges::binary_search(m_Program->variables, index);
}

std::span<const BuiltinFunction> Expression::functions() const noexcept
{
    return m_Program->functions;
}

bool Expression::usesRandom() const noexcept
{
    return m_Program->usesRandom;
}

TrackerDifferences Expression::trackerDifferences() const noexcept
{
    return m_Program->differences;
}

std::size_t Expression::trackerCodeSize() const noexcept
{
    return m_Program->trackerCodeSize;
}

bool Expression::fitsTrackerLimits() const noexcept
{
    return m_Program->trackerCodeSize <= TRACKER_CODE_LIMIT;
}

ExpressionDialect Expression::dialect() const noexcept
{
    return m_Program->dialect;
}

const std::string& Expression::text() const noexcept
{
    return m_Program->text;
}

std::string Expression::trackerText() const
{
    return m_Program->render(0, m_Program->code.size());
}

std::string Expression::Program::render(std::size_t begin, std::size_t end) const
{
    // Every operation gets its own parentheses and every sign wraps its operand, so Tracker's descent
    // never meets `^-`, a power followed by * or / inside a sum, or a signed addend.
    std::vector<std::string> operands;
    const auto binary = [&](std::string_view symbol)
    {
        auto right = std::move(operands.back());
        operands.pop_back();
        operands.back() = "(" + operands.back() + std::string(symbol) + right + ")";
    };
    for (std::size_t pc = begin; pc < end; ++pc)
    {
        const Instruction& instruction = code[pc];
        switch (instruction.op)
        {
        case OpCode::Number:
            operands.push_back(literals[instruction.operand]);
            break;
        case OpCode::Variable:
            operands.push_back(variableNames[instruction.operand]);
            break;
        case OpCode::Negate:
            // OSP recognizes `!` only before any sign, so a negated !(...) needs its own parentheses.
            operands.back() = operands.back().starts_with('!') ? "(-(" + operands.back() + "))" : "(-" + operands.back() + ")";
            break;
        case OpCode::Not:
            operands.back() = "!(" + operands.back() + ")";
            break;
        case OpCode::Unary:
            operands.back() = std::string(functionName(instruction.function)) + "(" + operands.back() + ")";
            break;
        case OpCode::Binary:
        {
            auto second = std::move(operands.back());
            operands.pop_back();
            operands.back() = std::string(functionName(instruction.function)) + "(" + operands.back() + "," + second + ")";
            break;
        }
        case OpCode::JumpIfZero:
        {
            // The true branch ends with the Jump that skips the false branch.
            const auto jump = pc + instruction.operand;
            const auto falseEnd = jump + 1 + code[jump].operand;
            operands.back() = "if(" + operands.back() + "," + render(pc + 1, jump) + "," + render(jump + 1, falseEnd) + ")";
            pc = falseEnd - 1;
            break;
        }
        case OpCode::Jump:
            break; // Consumed by JumpIfZero.
        case OpCode::Add:
            binary("+");
            break;
        case OpCode::Subtract:
            binary("-");
            break;
        case OpCode::Multiply:
            binary("*");
            break;
        case OpCode::Divide:
            binary("/");
            break;
        case OpCode::Power:
            binary("^");
            break;
        case OpCode::Less:
            binary("<");
            break;
        case OpCode::Greater:
            binary(">");
            break;
        case OpCode::LessEqual:
            binary("<=");
            break;
        case OpCode::GreaterEqual:
            binary(">=");
            break;
        case OpCode::Equal:
            binary("=");
            break;
        case OpCode::NotEqual:
            binary("<>");
            break;
        case OpCode::And:
            binary("&");
            break;
        case OpCode::Or:
            binary("|");
            break;
        }
    }
    return operands.back();
}

std::string_view errorMessage(ExpressionErrorCode code) noexcept
{
    switch (code)
    {
    case ExpressionErrorCode::SyntaxError:
        return "syntax error";
    case ExpressionErrorCode::ParenthesisExpected:
        return "parenthesis expected";
    case ExpressionErrorCode::ExpressionExpected:
        return "expression expected";
    case ExpressionErrorCode::UnknownIdentifier:
        return "unknown identifier";
    case ExpressionErrorCode::OperatorExpected:
        return "operator expected";
    case ExpressionErrorCode::ParenthesesMismatch:
        return "parentheses not match";
    case ExpressionErrorCode::CommaExpected:
        return "comma expected";
    case ExpressionErrorCode::InvalidOperand:
        return "invalid operand type";
    case ExpressionErrorCode::InvalidOperator:
        return "invalid operator";
    case ExpressionErrorCode::ReferenceDefinitionExpected:
        return "bad reference definition (: expected)";
    case ExpressionErrorCode::ReferenceNameExpected:
        return "reference name expected";
    case ExpressionErrorCode::TrackerCodeLimit:
        return "expression exceeds Tracker's 99-code limit";
    case ExpressionErrorCode::TooLong:
        return "expression is too long";
    case ExpressionErrorCode::TooDeep:
        return "expression is nested too deeply";
    case ExpressionErrorCode::InvalidEncoding:
        return "expression or variable name is not valid UTF-8";
    }
    return "unknown error";
}

std::string_view functionName(BuiltinFunction function) noexcept
{
    return FUNCTION_NAMES[static_cast<std::size_t>(function)];
}

bool isReservedName(std::string_view name) noexcept
{
    return name == "pi" || name == "e" || name == "if" || std::ranges::find(FUNCTION_NAMES, name) != FUNCTION_NAMES.end();
}

ExpressionResult parseExpression(std::string_view text, std::span<const std::string_view> variables, const ExpressionOptions& options)
{
    auto utf16 = toUtf16(text);
    std::vector<std::u16string> names;
    names.reserve(variables.size());
    for (const auto variable : variables)
    {
        auto name = toUtf16(variable);
        if (!name)
        {
            utf16.reset();
            break;
        }
        names.push_back(std::move(*name));
    }
    if (!utf16)
    {
        return ExpressionError{.code = ExpressionErrorCode::InvalidEncoding, .position = 0};
    }
    // SuryonoParser.define() removes every backslash before parsing.
    std::erase(*utf16, u'\\');
    if (utf16->size() > options.maxLength)
    {
        return ExpressionError{.code = ExpressionErrorCode::TooLong, .position = options.maxLength + 1};
    }
    Compiler compiler(names, options);
    auto compiled = compiler.compile(*utf16);
    if (const auto* error = std::get_if<ExpressionError>(&compiled))
    {
        return *error;
    }
    return detail::ExpressionBuilder::build(text,
                                            variables,
                                            options.dialect,
                                            {.segment = std::move(std::get<Segment>(compiled)),
                                             .differences = compiler.differences(),
                                             .correctedOnlyCodes = compiler.correctedOnlyCodes(),
                                             .literals = compiler.takeLiterals()});
}

Expression detail::ExpressionBuilder::build(std::string_view text,
                                            std::span<const std::string_view> variables,
                                            ExpressionDialect dialect,
                                            Compilation compilation)
{
    auto& segment = compilation.segment;
    auto program = std::make_shared<Expression::Program>();
    program->text = std::string(text);
    program->literals = std::move(compilation.literals);
    program->variableNames.assign(variables.begin(), variables.end());
    program->variableCount = variables.size();
    program->trackerCodeSize = segment.width - compilation.correctedOnlyCodes;
    program->differences = compilation.differences;
    program->dialect = dialect;
    std::size_t depth = 0;
    std::vector<std::size_t> branches;
    for (const auto& instruction : segment.code)
    {
        switch (instruction.op)
        {
        case OpCode::Number:
            ++depth;
            break;
        case OpCode::Variable:
            ++depth;
            program->variables.push_back(instruction.operand);
            break;
        case OpCode::Unary:
            program->functions.push_back(instruction.function);
            break;
        case OpCode::Binary:
            --depth;
            program->functions.push_back(instruction.function);
            break;
        case OpCode::JumpIfZero:
            --depth;
            branches.push_back(depth);
            break;
        case OpCode::Jump:
            depth = branches.back(); // The false branch starts from the depth before the true branch.
            branches.pop_back();
            break;
        case OpCode::Negate:
        case OpCode::Not:
            break;
        case OpCode::Add:
        case OpCode::Subtract:
        case OpCode::Multiply:
        case OpCode::Divide:
        case OpCode::Power:
        case OpCode::Less:
        case OpCode::Greater:
        case OpCode::LessEqual:
        case OpCode::GreaterEqual:
        case OpCode::Equal:
        case OpCode::NotEqual:
        case OpCode::And:
        case OpCode::Or:
            --depth;
            break;
        }
        program->stackSize = std::max(program->stackSize, depth);
    }
    program->code = std::move(segment.code);
    std::ranges::sort(program->variables);
    program->variables.erase(std::ranges::unique(program->variables).begin(), program->variables.end());
    std::ranges::sort(program->functions);
    program->functions.erase(std::ranges::unique(program->functions).begin(), program->functions.end());
    program->usesRandom = std::ranges::binary_search(program->functions, BuiltinFunction::Random);
    program->differences.inverseHyperbolic = std::ranges::any_of(
        program->functions,
        [](BuiltinFunction function)
        { return function == BuiltinFunction::Asinh || function == BuiltinFunction::Acosh || function == BuiltinFunction::Atanh; });
    program->differences.commonLogarithm = std::ranges::binary_search(program->functions, BuiltinFunction::Log);
    return Expression(std::move(program));
}

ExpressionResult parseExpression(std::string_view text, std::span<const std::string> variables, const ExpressionOptions& options)
{
    const std::vector<std::string_view> views(variables.begin(), variables.end());
    return parseExpression(text, views, options);
}

} // namespace motrack::science
