#include "science/fit/curve_fit.hpp"
#include "science/fit/fit_internal.hpp"
#include "science/java_math.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Built-in fit definitions and DatasetCurveFitter.getScratchParams (OSP d56207b). Each model repeats
// the operation order in which SuryonoParser evaluates the fit's expression; `x^2` is Math.pow(x, 2.0),
// which HotSpot computes as x*x.

namespace motrack::science
{
namespace
{

constexpr std::size_t MAX_POLYNOMIAL_DEGREE = 5;
constexpr std::array<std::string_view, MAX_POLYNOMIAL_DEGREE + 1> POLYNOMIAL_NAMES = {"A", "B", "C", "D", "E", "F"};

/// `x^exponent` (exponent >= 1) as the parser evaluates it inside a polynomial expression.
double power(double x, std::size_t exponent)
{
    switch (exponent)
    {
    case 1:
        return x;
    case 2:
        return x * x;
    default:
        return java::pow(x, static_cast<double>(exponent));
    }
}

/// `A*x^n+B*x^(n-1)+...+Z` summed left to right, as the parser evaluates a polynomial test function.
double polynomialExpression(double x, std::span<const double> p)
{
    const std::size_t degree = p.size() - 1;
    double value = p[0] * power(x, degree);
    for (std::size_t i = 1; i < p.size(); ++i)
    {
        const std::size_t exponent = degree - i;
        value = value + (exponent == 0 ? p[i] : p[i] * power(x, exponent));
    }
    return value;
}

std::string polynomialExpressionText(std::size_t degree)
{
    std::string text;
    for (std::size_t i = 0; i <= degree; ++i)
    {
        text += POLYNOMIAL_NAMES[i];
        const std::size_t exponent = degree - i;
        if (exponent > 0)
        {
            text += "*x";
            if (exponent > 1)
            {
                text += '^';
                text += std::to_string(exponent);
            }
            text += '+';
        }
    }
    return text;
}

FitFunction polynomial(std::size_t degree, std::optional<BuiltInFit> kind)
{
    FitFunction f;
    f.name = "Poly" + std::to_string(degree);
    f.description = "Polynomial, degree " + std::to_string(degree);
    f.expression = polynomialExpressionText(degree);
    for (std::size_t i = 0; i <= degree; ++i)
    {
        // KnownPolynomial describes only a line's parameters.
        std::string description = degree == 1 ? (i == 0 ? "Slope" : "Intercept") : "";
        f.parameters.push_back({.name = std::string(POLYNOMIAL_NAMES[i]), .value = 0.0, .description = std::move(description)});
    }
    f.model = polynomialExpression;
    f.builtIn = kind;
    f.polynomialDegree = static_cast<int>(degree);
    return f;
}

FitParameter parameter(std::string_view name, double value, std::string_view description)
{
    return FitParameter{.name = std::string(name), .value = value, .description = std::string(description)};
}

FitFunction userFunction(std::string name,
                         std::string description,
                         std::string expression,
                         std::vector<FitParameter> parameters,
                         FitModel model,
                         BuiltInFit kind)
{
    return FitFunction{.name = std::move(name),
                       .description = std::move(description),
                       .expression = std::move(expression),
                       .parameters = std::move(parameters),
                       .model = std::move(model),
                       .builtIn = kind,
                       .polynomialDegree = 0};
}

constexpr double PI = std::numbers::pi;

/// Double.compare order, which TreeMap<Double, Double> keys follow: -0.0 before 0.0, NaN last.
struct JavaDoubleLess
{
    static std::int64_t key(double value) noexcept
    {
        return std::isnan(value) ? std::bit_cast<std::int64_t>(std::numeric_limits<double>::quiet_NaN())
                                 : std::bit_cast<std::int64_t>(value);
    }

    bool operator()(double a, double b) const noexcept
    {
        if (a < b)
        {
            return true;
        }
        if (a > b)
        {
            return false;
        }
        return key(a) < key(b);
    }
};

struct SortedData
{
    std::vector<double> x;
    std::vector<double> y;
    double minimum = std::numeric_limits<double>::max();
    double maximum = -std::numeric_limits<double>::max();
};

/// Tracker sorts through a TreeMap keyed by x, which keeps only the last y of a repeated x yet still
/// reads arrays sized for every point, so their tail stays 0.0 (MoTrack #242). Corrected
/// sorts every point stably.
SortedData sortData(std::span<const double> x, std::span<const double> y, FitDialect dialect)
{
    SortedData data;
    const std::size_t count = x.size();
    data.x.assign(count, 0.0);
    data.y.assign(count, 0.0);
    if (dialect == FitDialect::Tracker)
    {
        std::map<double, double, JavaDoubleLess> sorted;
        for (std::size_t i = 0; i < count; ++i)
        {
            sorted.insert_or_assign(x[i], y[i]);
        }
        std::size_t index = 0;
        for (const auto& [key, value] : sorted)
        {
            data.x[index] = key;
            data.y[index] = value;
            data.maximum = java::max(value, data.maximum);
            data.minimum = java::min(value, data.minimum);
            ++index;
        }
        return data;
    }
    std::vector<std::size_t> order(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        order[i] = i;
    }
    std::ranges::stable_sort(order, [&x](std::size_t a, std::size_t b) { return x[a] < x[b]; });
    for (std::size_t i = 0; i < count; ++i)
    {
        data.x[i] = x[order[i]];
        data.y[i] = y[order[i]];
        data.maximum = java::max(data.y[i], data.maximum);
        data.minimum = java::min(data.y[i], data.minimum);
    }
    return data;
}

/// Phase, frequency, amplitude and offset of Sinusoid and DampedSine from the zero crossings.
void sineGuess(const SortedData& data, std::vector<double>& params)
{
    const std::size_t count = data.x.size();
    params[3] = (data.minimum + data.maximum) / 2;
    params[0] = (data.maximum - data.minimum) / 2;
    int crossings = 0;
    constexpr double UNSET = std::numeric_limits<double>::max();
    double firstCrossing = UNSET;
    double lastCrossing = UNSET;
    double previousX = UNSET;
    double previousY = UNSET;
    bool positiveSlope = true;
    for (std::size_t i = 0; i < count; ++i)
    {
        const double shifted = data.y[i] - params[3];
        if (i == 0)
        {
            previousX = data.x[i];
            previousY = shifted;
        }
        const bool crossed = previousY > 0 ? shifted <= 0 : shifted > 0;
        if (crossed)
        {
            ++crossings;
            lastCrossing = data.x[i] - ((shifted / (shifted - previousY)) * (data.x[i] - previousX));
            if (crossings == 1)
            {
                firstCrossing = lastCrossing;
                positiveSlope = shifted > 0;
            }
        }
        previousX = data.x[i];
        previousY = shifted;
    }
    const bool success = firstCrossing != UNSET;
    const double xMinimum = data.x.front();
    const double xMaximum = data.x.back();
    if (crossings > 1)
    {
        params[1] = (PI * static_cast<double>(crossings - 1)) / (lastCrossing - firstCrossing);
    }
    else
    {
        params[1] = (PI * static_cast<double>(std::max(1, crossings))) / (xMaximum - xMinimum);
    }
    const double phaseToFirstCrossing = success ? params[1] * firstCrossing : 0;
    params[2] = positiveSlope ? -phaseToFirstCrossing : PI - phaseToFirstCrossing;
}

/// Offset from three points equally spaced in x, then rate and magnitude through the end points.
void exponentialGuess(const SortedData& data, std::vector<double>& params)
{
    const auto& sx = data.x;
    const auto& sy = data.y;
    const std::size_t last = sx.size() - 1;
    const double range = sx[last] - sx[0];
    std::size_t mid = last / 2;
    while (sx[mid] - sx[0] < range / 2 && mid < last)
    {
        ++mid;
    }
    while (sx[mid] - sx[0] > range / 2 && mid > 0)
    {
        --mid;
    }
    std::size_t tail = std::min(last, 2 * mid);
    while (sx[tail] - sx[0] < 2 * (sx[mid] - sx[0]) && tail < last)
    {
        ++tail;
    }
    while (sx[tail] - sx[0] > 2 * (sx[mid] - sx[0]) && tail > 0)
    {
        --tail;
    }
    params[2] = ((sy[mid] * sy[mid]) - (sy[0] * sy[tail])) / (((2 * sy[mid]) - sy[0]) - sy[tail]);
    // The offset stays below the data so that the logarithm below is defined.
    params[2] = java::min(data.minimum - (0.001 * range), params[2]);
    const double offset = params[2];
    params[1] = std::log((sy[tail] - offset) / (sy[0] - offset)) / (sx[tail] - sx[0]);
    params[0] = (sy[mid] - offset) / std::exp(params[1] * sx[mid]);
}

/// Jean Jacquelin's integral-equation regression for a Gaussian (Regressions et equations integrales, p. 7).
bool gaussianGuess(const SortedData& data, std::vector<double>& params)
{
    const auto& sx = data.x;
    const auto& sy = data.y;
    const std::size_t count = sx.size();
    std::vector<double> s(count, 0.0);
    std::vector<double> t(count, 0.0);
    double sumSSq = 0;
    double sumTSq = 0;
    double sumST = 0;
    double sumSy = 0;
    double sumTy = 0;
    double sumY = sy[0];
    for (std::size_t i = 1; i < count; ++i)
    {
        s[i] = s[i - 1] + ((0.5 * (sy[i] + sy[i - 1])) * (sx[i] - sx[i - 1]));
        t[i] = t[i - 1] + ((0.5 * ((sx[i] * sy[i]) + (sx[i - 1] * sy[i - 1]))) * (sx[i] - sx[i - 1]));
        sumSSq += s[i] * s[i];
        sumST += s[i] * t[i];
        sumTSq += t[i] * t[i];
        sumSy += s[i] * (sy[i] - sy[0]);
        sumTy += t[i] * (sy[i] - sy[0]);
        sumY += sy[i];
    }
    const std::array matrix = {sumSSq, sumST, sumST, sumTSq};
    fit_detail::LupDecomposition system(matrix, 2);
    if (!system.inverse())
    {
        return false;
    }
    const std::array constants = {sumSy, sumTy};
    const std::vector<double> results = system.solve(constants);
    const double a = -results[0] / results[1];
    const double b = -2 / results[1];
    double sumExp = 0;
    for (std::size_t i = 1; i < count; ++i)
    {
        sumExp += std::exp(-((sx[i] - a) * (sx[i] - a)) / b);
    }
    params[0] = sumY / sumExp;
    params[1] = a;
    params[2] = std::sqrt(b / 2);
    return true;
}

/// Log and Power fit a line to logarithms. Tracker reuses its Line fit, whose previous coefficients remain
/// when that fit fails; a fresh Data Tool's are zero.
bool logarithmicLineFit(std::span<const double> x, std::span<const double> y, FitDialect dialect, double& slope, double& intercept)
{
    std::vector<double> coefficients = {0.0, 0.0};
    const bool solved = fit_detail::solvePolynomialNormalEquations(x, y, coefficients);
    if (!solved && dialect == FitDialect::Corrected)
    {
        return false;
    }
    intercept = coefficients[0];
    slope = coefficients[1];
    return true;
}

} // namespace

FitFunction builtInFit(BuiltInFit kind)
{
    switch (kind)
    {
    case BuiltInFit::Line:
        return polynomial(1, kind);
    case BuiltInFit::Parabola:
        return polynomial(2, kind);
    case BuiltInFit::Cubic:
        return polynomial(3, kind);
    case BuiltInFit::Gaussian:
        return userFunction(
            "Gaussian",
            "Gaussian function",
            "A*exp(-(x-B)^2/(2*C^2))",
            {parameter("A", 1.0, "Peak height"), parameter("B", 0.0, "Peak position"), parameter("C", 1.0, "Standard deviation")},
            [](double x, std::span<const double> p)
            {
                const double d = x - p[1];
                return p[0] * std::exp(-(d * d) / (2 * (p[2] * p[2])));
            },
            kind);
    case BuiltInFit::Exponential:
        return userFunction(
            "Exponential",
            "Exponential decay or growth with vertical offset",
            "A*exp(B*x)+C",
            {parameter("A", 1.0, "Magnitude"), parameter("B", -1.0, "Decay or growth rate"), parameter("C", 0.0, "Vertical Offset")},
            [](double x, std::span<const double> p) { return (p[0] * std::exp(p[1] * x)) + p[2]; },
            kind);
    case BuiltInFit::Sinusoid:
        return userFunction(
            "Sinusoid",
            "Sine function with constant amplitude",
            "A*sin(B*x+C)+D",
            {parameter("A", 1.0, "Amplitude"),
             parameter("B", 1.0, "Angular frequency"),
             parameter("C", 0.0, "Initial phase"),
             parameter("D", 0.0, "Vertical Offset")},
            [](double x, std::span<const double> p) { return (p[0] * std::sin((p[1] * x) + p[2])) + p[3]; },
            kind);
    case BuiltInFit::DampedSine:
        // OSP describes the amplitude A as "Intercept".
        return userFunction(
            "DampedSine",
            "Damped sine function with offset",
            "A*exp(E*x)*sin(B*x+C)+D",
            {parameter("A", 1.0, "Intercept"),
             parameter("B", 1.0, "Angular frequency"),
             parameter("C", 0.0, "Initial phase"),
             parameter("D", 0.0, "Vertical Offset"),
             parameter("E", 0.0, "Decay or growth rate")},
            [](double x, std::span<const double> p) { return ((p[0] * std::exp(p[4] * x)) * std::sin((p[1] * x) + p[2])) + p[3]; },
            kind);
    case BuiltInFit::Power:
        return userFunction(
            "Power",
            "Power function with offsets",
            "A*x^B",
            {parameter("A", 1.0, "Coefficient"), parameter("B", 1.0, "Power")},
            [](double x, std::span<const double> p) { return p[0] * java::pow(x, p[1]); },
            kind);
    case BuiltInFit::Log:
        return userFunction(
            "Log",
            "Natural log function with offsets",
            "A*ln(x)+B",
            {parameter("A", 1.0, "Scale Factor"), parameter("B", 0.0, "Vertical Offset")},
            [](double x, std::span<const double> p) { return (p[0] * std::log(x)) + p[1]; },
            kind);
    }
    return {};
}

std::string_view builtInFitDisplayName(BuiltInFit kind) noexcept
{
    switch (kind)
    {
    case BuiltInFit::Line:
        return "Line";
    case BuiltInFit::Parabola:
        return "Parabola";
    case BuiltInFit::Cubic:
        return "Cubic";
    case BuiltInFit::Gaussian:
        return "Gaussian";
    case BuiltInFit::Exponential:
        return "Exponential";
    case BuiltInFit::Sinusoid:
        return "Sine";
    case BuiltInFit::DampedSine:
        return "Damped Sine";
    case BuiltInFit::Power:
        return "Power";
    case BuiltInFit::Log:
        return "Log";
    }
    return {};
}

std::optional<FitFunction> polynomialFit(int degree)
{
    if (degree < 1 || std::cmp_greater(degree, MAX_POLYNOMIAL_DEGREE))
    {
        return std::nullopt;
    }
    constexpr std::array KINDS = {BuiltInFit::Line, BuiltInFit::Parabola, BuiltInFit::Cubic};
    const auto size = static_cast<std::size_t>(degree);
    return polynomial(size, size <= KINDS.size() ? std::optional(KINDS[size - 1]) : std::nullopt);
}

namespace fit_detail
{

std::vector<double> builtInDefaults(BuiltInFit kind)
{
    std::vector<double> values;
    for (const auto& parameter : builtInFit(kind).parameters)
    {
        values.push_back(parameter.value);
    }
    return values;
}

std::optional<std::vector<double>>
initialGuess(BuiltInFit kind, std::span<const double> x, std::span<const double> y, std::span<const double> fallback, FitDialect dialect)
{
    if (kind == BuiltInFit::Line || kind == BuiltInFit::Parabola || kind == BuiltInFit::Cubic)
    {
        return std::nullopt;
    }
    std::vector<double> params(fallback.begin(), fallback.end());
    if (params.empty() || x.size() < params.size())
    {
        return std::nullopt;
    }
    const SortedData data = sortData(x, y, dialect);
    switch (kind)
    {
    case BuiltInFit::Sinusoid:
    case BuiltInFit::DampedSine:
        sineGuess(data, params);
        break;
    case BuiltInFit::Exponential:
        exponentialGuess(data, params);
        break;
    case BuiltInFit::Gaussian:
        if (!gaussianGuess(data, params))
        {
            return std::nullopt;
        }
        break;
    case BuiltInFit::Log:
    {
        if (data.x.front() <= 0)
        {
            return std::nullopt;
        }
        std::vector<double> logX(data.x.size());
        std::ranges::transform(data.x, logX.begin(), [](double value) { return std::log(value); });
        if (!logarithmicLineFit(logX, data.y, dialect, params[0], params[1]))
        {
            return std::nullopt;
        }
        break;
    }
    case BuiltInFit::Power:
    {
        if (data.x.front() <= 0 || data.minimum <= 0)
        {
            return std::nullopt;
        }
        std::vector<double> logX(data.x.size());
        std::vector<double> logY(data.y.size());
        std::ranges::transform(data.x, logX.begin(), [](double value) { return std::log(value); });
        std::ranges::transform(data.y, logY.begin(), [](double value) { return std::log(value); });
        double slope = 0;
        double intercept = 0;
        if (!logarithmicLineFit(logX, logY, dialect, slope, intercept))
        {
            return std::nullopt;
        }
        params[0] = std::exp(intercept);
        params[1] = slope;
        break;
    }
    default:
        return std::nullopt;
    }
    // Tracker starts from NaN values and then skips the fit; Corrected keeps the current values instead.
    if (dialect == FitDialect::Corrected && !std::ranges::all_of(params, [](double value) { return std::isfinite(value); }))
    {
        return std::nullopt;
    }
    return params;
}

} // namespace fit_detail
} // namespace motrack::science
