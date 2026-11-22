#pragma once

#include <optional>
#include <string_view>

namespace motrack::science
{

/// Converts Java decimal floating-point text to the nearest double, like `Double.parseDouble`.
///
/// Accepts `[+|-](digits[.[digits]] | .digits)[(e|E)[+|-]digits]` and rounds the exact decimal value to
/// nearest, ties to even, for any number of digits. Overflow yields an infinity and underflow a zero of
/// the parsed sign, as in Java. Java's whitespace trimming, `Infinity`/`NaN` names, hexadecimal form and
/// `d`/`f` suffixes are not accepted. The conversion is locale-independent and allocation-free.
///
/// @return the value, or no value when the text is not in the accepted form.
[[nodiscard]] std::optional<double> parseJavaDecimal(std::string_view text) noexcept;

} // namespace motrack::science
