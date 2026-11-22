#pragma once

#include <string>
#include <string_view>
#include <vector>

/// Java `String` operations that the OSP library classes rely on, applied to UTF-8 text.
///
/// Java strings are UTF-16. The operations below only search for ASCII characters or compare whole strings, so
/// byte offsets in valid UTF-8 preserve every Java result except ordering, which `javaCompare` reproduces.
namespace motrack::library
{

/// `String.compareTo`: lexicographic order of UTF-16 code units. It differs from UTF-8 byte order only when a
/// supplementary character (encoded as a surrogate pair, 0xD800-0xDFFF) meets a character in 0xE000-0xFFFF.
/// Precondition: valid UTF-8 (motrack_osp refuses other input).
[[nodiscard]] int javaCompare(std::string_view left, std::string_view right) noexcept;

/// Strict weak ordering by `javaCompare`, as Java's `TreeMap`/`TreeSet` with natural ordering use.
struct JavaLess
{
    [[nodiscard]] bool operator()(std::string_view left, std::string_view right) const noexcept
    {
        return javaCompare(left, right) < 0;
    }
};

/// `text.split(separator)` for a literal separator with Java's limit-zero rules: no match yields the whole
/// text (even when empty), and trailing empty fields are removed, so "\n".split("\n") is empty.
[[nodiscard]] std::vector<std::string> javaSplit(std::string_view text, std::string_view separator);

/// `String.toLowerCase()` restricted to ASCII letters. Difference: Java also lowers non-ASCII letters with the
/// default locale. MoTrack only uses it to compare with ASCII constants or path prefixes; see
/// docs/library-collections.md.
[[nodiscard]] std::string asciiLower(std::string_view text);

/// `String.trim()`: removes every leading and trailing character <= U+0020.
[[nodiscard]] std::string javaTrimmed(std::string_view text);

} // namespace motrack::library
