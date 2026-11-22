#include "science/fit/curve_fit.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace motrack::science
{
namespace
{

/// " +- " with the plus-minus sign in UTF-8, escaped so that the source stays ASCII.
constexpr std::string_view PLUS_MINUS = " \xC2\xB1 ";

/// The shortest round-trip digits of |value| and its decimal exponent: value = 0.d1d2... x 10^exponent,
/// as java.lang.FloatingDecimal reports them (JDK 17 occasionally emits one more digit than shortest).
struct Digits
{
    std::string digits;
    int exponent = 0;
};

Digits shortestDigits(double magnitude)
{
    std::array<char, 64> buffer{};
    const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), magnitude, std::chars_format::scientific);
    const std::string_view text(buffer.data(), static_cast<std::size_t>(end - buffer.data()));
    Digits result;
    const std::size_t e = text.find('e');
    for (const char c : text.substr(0, e))
    {
        if (c != '.')
        {
            result.digits += c;
        }
    }
    int exponent = 0;
    const std::string_view exponentText = text.substr(e + 1);
    std::from_chars(exponentText.data() + (exponentText.front() == '+' ? 1 : 0), exponentText.data() + exponentText.size(), exponent);
    result.exponent = exponent + 1;
    return result;
}

/// Java's String.format("%.nf"): HALF_UP rounding of the Double.toString digits (FormattedFloatingDecimal).
std::string javaFixed(double value, int places)
{
    const bool negative = std::signbit(value);
    std::string text = negative ? "-" : "";
    const double magnitude = std::abs(value);
    if (magnitude == 0)
    {
        text += '0';
        if (places > 0)
        {
            text += '.';
            text.append(static_cast<std::size_t>(places), '0');
        }
        return text;
    }
    Digits number = shortestDigits(magnitude);
    std::string& digits = number.digits;
    const auto count = static_cast<int>(digits.size());
    int exponent = number.exponent;
    // applyPrecision
    const int keep = exponent + places;
    if (keep >= 0 && keep < count)
    {
        if (keep == 0)
        {
            if (digits[0] >= '5')
            {
                digits.assign(static_cast<std::size_t>(count), '0');
                digits[0] = '1';
                ++exponent;
            }
            else
            {
                digits.assign(static_cast<std::size_t>(count), '0');
            }
        }
        else if (digits[static_cast<std::size_t>(keep)] >= '5')
        {
            int i = keep - 1;
            while (i >= 0 && digits[static_cast<std::size_t>(i)] == '9')
            {
                --i;
            }
            if (i < 0)
            {
                digits.assign(static_cast<std::size_t>(count), '0');
                digits[0] = '1';
                ++exponent;
            }
            else
            {
                ++digits[static_cast<std::size_t>(i)];
                std::fill(digits.begin() + i + 1, digits.end(), '0');
            }
        }
        else
        {
            std::fill(digits.begin() + keep, digits.end(), '0');
        }
    }
    // fillDecimal, then Formatter pads the fraction to `places` digits.
    std::string integer;
    std::string fraction;
    if (exponent > 0)
    {
        for (int i = 0; i < exponent; ++i)
        {
            integer += i < count ? digits[static_cast<std::size_t>(i)] : '0';
        }
        for (int i = exponent; i < count && std::cmp_less(fraction.size(), places); ++i)
        {
            fraction += digits[static_cast<std::size_t>(i)];
        }
    }
    else
    {
        integer = "0";
        for (int i = 0; i < -exponent && std::cmp_less(fraction.size(), places); ++i)
        {
            fraction += '0';
        }
        for (int i = 0; i < count && std::cmp_less(fraction.size(), places); ++i)
        {
            fraction += digits[static_cast<std::size_t>(i)];
        }
    }
    fraction.resize(static_cast<std::size_t>(places), '0');
    text += integer;
    if (places > 0)
    {
        text += '.';
        text += fraction;
    }
    return text;
}

/// Double.toString's letters: NaN, Infinity, or an exponent outside [1e-3, 1e7).
std::string javaDoubleText(double value)
{
    if (std::isnan(value))
    {
        return "NaN";
    }
    if (std::isinf(value))
    {
        return value > 0 ? "Infinity" : "-Infinity";
    }
    const double magnitude = std::abs(value);
    std::string text = std::signbit(value) ? "-" : "";
    if (magnitude == 0)
    {
        return text + "0.0";
    }
    const Digits number = shortestDigits(magnitude);
    if (magnitude >= 1e-3 && magnitude < 1e7)
    {
        return text + javaFixed(magnitude, std::max(1, static_cast<int>(number.digits.size()) - number.exponent));
    }
    text += number.digits.substr(0, 1) + "." + (number.digits.size() > 1 ? number.digits.substr(1) : "0");
    return text + "E" + std::to_string(number.exponent - 1);
}

bool delimiter(char c)
{
    return std::string_view(" +-*/^(),<>=&|!").find(c) != std::string_view::npos;
}

/// Whether String.replace(name, ...) would touch anything but whole occurrences of the name.
bool corrupts(std::string_view text, std::string_view name)
{
    for (std::size_t position = text.find(name); position != std::string_view::npos; position = text.find(name, position + name.size()))
    {
        const bool before = position == 0 || delimiter(text[position - 1]);
        const std::size_t after = position + name.size();
        if (!before || (after < text.size() && !delimiter(text[after])))
        {
            return true;
        }
    }
    return false;
}

std::string replaceAll(std::string text, std::string_view name, std::string_view replacement)
{
    for (std::size_t position = text.find(name); position != std::string::npos; position = text.find(name, position + replacement.size()))
    {
        text.replace(position, name.size(), replacement);
    }
    return text;
}

} // namespace

std::optional<std::string> formatUncertainParameter(double value, double sigma, int extraPlaces)
{
    if (std::isnan(sigma) || sigma <= 0 || !std::isfinite(sigma) || !std::isfinite(value))
    {
        return std::nullopt;
    }
    int exponent = value == 0 ? 0 : static_cast<int>(std::floor(std::log10(std::abs(value))));
    const int sigmaExponent = static_cast<int>(std::floor(std::log10(sigma)));
    exponent = std::max(exponent, sigmaExponent);
    const int shift = exponent - sigmaExponent;
    const double multiplier = std::pow(10.0, static_cast<double>(-exponent));
    const int places = std::max(0, shift) + extraPlaces;
    std::string text = javaFixed(value * multiplier, places) + std::string(PLUS_MINUS) + javaFixed(sigma * multiplier, places);
    if (exponent != 0)
    {
        text = "(" + text + ") E" + std::to_string(exponent);
    }
    return text;
}

std::vector<std::string> trackerSubstitutionCollisions(const FitFunction& function, const std::vector<bool>& fixed)
{
    std::vector<std::string> collisions;
    const auto note = [&collisions](const std::string& name)
    {
        if (std::ranges::find(collisions, name) == collisions.end())
        {
            collisions.push_back(name);
        }
    };
    // getTestFunction substitutes the fixed parameters in order, then each free one in turn.
    std::string text = function.expression;
    const std::size_t count = function.parameters.size();
    for (std::size_t i = 0; i < count && i < fixed.size(); ++i)
    {
        const auto& parameter = function.parameters[i];
        if (!fixed[i] || parameter.name.empty())
        {
            continue;
        }
        if (corrupts(text, parameter.name))
        {
            note(parameter.name);
        }
        text = replaceAll(text, parameter.name, "(" + javaDoubleText(parameter.value) + ")");
    }
    for (std::size_t i = 0; i < count; ++i)
    {
        const auto& parameter = function.parameters[i];
        if ((i < fixed.size() && fixed[i]) || parameter.name.empty())
        {
            continue;
        }
        if (corrupts(text, parameter.name))
        {
            note(parameter.name);
        }
    }
    return collisions;
}

} // namespace motrack::science
