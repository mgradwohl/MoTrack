#pragma once

#include "osp/expected.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

/// Java text conversions used by OSP's XMLControl reader and writer.
///
/// The reader in OSP stores every scalar as text and converts it lazily with the Java standard library.
/// These functions reproduce the conversions the pinned reference (OSP d56207b, Tracker 6.3.5) uses, so
/// MoTrack decodes a property exactly as the Java loader would. All functions are locale-independent and
/// operate on UTF-8 bytes. Documented differences from Java are listed with each function.
namespace motrack::osp
{

/// Why a stored value could not be decoded. Java signals every malformed number with the same
/// NumberFormatException; the distinct codes only make diagnostics clearer.
enum class DecodeError : std::uint8_t
{
    Missing,     ///< No property with the requested name.
    WrongType,   ///< The property's OSP type is not one the Java accessor converts.
    Malformed,   ///< The Java conversion would throw (for example NumberFormatException).
    OutOfRange,  ///< The digits are valid but exceed the target type (Java also throws).
    Unsupported, ///< MoTrack deliberately does not decode this form; see the function's notes.
};

/// `Integer.parseInt(text)` in radix 10: an optional ASCII sign followed by at least one digit, without
/// surrounding whitespace, and within the signed 32-bit range.
/// Difference: Java also accepts non-ASCII Unicode decimal digits (for example U+0663); MoTrack
/// reports them as Unsupported. OSP never writes such digits.
[[nodiscard]] Expected<std::int32_t, DecodeError> parseJavaInt(std::string_view text);

/// `Double.parseDouble(text)`: `String.trim()` is applied first, then Java's floating-point literal grammar
/// is accepted, including `NaN`, `Infinity`, hexadecimal significands with a binary exponent and an
/// optional `f`/`F`/`d`/`D` suffix. The result is correctly rounded (round half to even); overflow gives an
/// infinity and underflow gives zero or a subnormal, as in Java. NaN is returned in its canonical form.
[[nodiscard]] Expected<double, DecodeError> parseJavaDouble(std::string_view text);

/// `Boolean.parseBoolean(text)`: true only for a case-insensitive "true".
[[nodiscard]] bool parseJavaBoolean(std::string_view text) noexcept;

/// `Integer.toString(value)`.
[[nodiscard]] std::string formatJavaInt(std::int32_t value);

/// `Double.toString(value)` as specified since JDK 19: the shortest decimal that rounds to `value`
/// (at least two significant digits when one would suffice), in plain notation for 1e-3 <= |value| < 1e7
/// and computerized scientific notation (`1.0E7`) otherwise.
/// Difference: the JDK 17 runtime used by Tracker 6.3.5 can print a longer, non-shortest digit string for
/// some values. Both strings parse to the same double.
[[nodiscard]] std::string formatJavaDouble(double value);

/// `String.trim()`: removes every leading and trailing byte <= U+0020.
[[nodiscard]] std::string_view javaTrim(std::string_view text) noexcept;

/// `text.split(",")` with Java's limit-zero rules: no separator yields the whole text (even when empty);
/// otherwise trailing empty fields are removed, so "," yields no fields.
[[nodiscard]] std::vector<std::string_view> javaSplitCommas(std::string_view text);

} // namespace motrack::osp
