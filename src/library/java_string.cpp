#include "library/java_string.hpp"

#include "osp/java_text.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace motrack::library
{
namespace
{

/// Decodes one UTF-8 sequence starting at `position` and advances it. Invalid sequences cannot occur in text
/// read through motrack_osp; a stray byte is returned as itself so the comparison stays total.
std::uint32_t nextCodePoint(std::string_view text, std::size_t& position) noexcept
{
    const auto lead = static_cast<unsigned char>(text[position]);
    std::size_t length = 1;
    std::uint32_t value = lead;
    if (lead >= 0xF0)
    {
        length = 4;
        value = lead & 0x07U;
    }
    else if (lead >= 0xE0)
    {
        length = 3;
        value = lead & 0x0FU;
    }
    else if (lead >= 0xC0)
    {
        length = 2;
        value = lead & 0x1FU;
    }
    if (length == 1 || position + length > text.size())
    {
        ++position;
        return lead;
    }
    for (std::size_t index = 1; index < length; ++index)
    {
        value = (value << 6U) | (static_cast<unsigned char>(text[position + index]) & 0x3FU);
    }
    position += length;
    return value;
}

/// The first UTF-16 code unit of a code point: supplementary characters sort as their high surrogate.
std::uint32_t firstCodeUnit(std::uint32_t codePoint) noexcept
{
    if (codePoint < 0x10000U)
    {
        return codePoint;
    }
    return 0xD800U + ((codePoint - 0x10000U) >> 10U);
}

} // namespace

int javaCompare(std::string_view left, std::string_view right) noexcept
{
    std::size_t leftPosition = 0;
    std::size_t rightPosition = 0;
    while (leftPosition < left.size() && rightPosition < right.size())
    {
        const auto leftCode = nextCodePoint(left, leftPosition);
        const auto rightCode = nextCodePoint(right, rightPosition);
        if (leftCode == rightCode)
        {
            continue;
        }
        const auto leftUnit = firstCodeUnit(leftCode);
        const auto rightUnit = firstCodeUnit(rightCode);
        if (leftUnit != rightUnit)
        {
            return leftUnit < rightUnit ? -1 : 1;
        }
        // Two supplementary characters with the same high surrogate: their low surrogates follow code point order.
        return leftCode < rightCode ? -1 : 1;
    }
    if (leftPosition < left.size())
    {
        return 1;
    }
    return rightPosition < right.size() ? -1 : 0;
}

std::vector<std::string> javaSplit(std::string_view text, std::string_view separator)
{
    std::vector<std::string> fields;
    if (separator.empty() || text.find(separator) == std::string_view::npos)
    {
        fields.emplace_back(text);
        return fields;
    }
    std::size_t start = 0;
    while (true)
    {
        const auto next = text.find(separator, start);
        if (next == std::string_view::npos)
        {
            fields.emplace_back(text.substr(start));
            break;
        }
        fields.emplace_back(text.substr(start, next - start));
        start = next + separator.size();
    }
    while (!fields.empty() && fields.back().empty())
    {
        fields.pop_back();
    }
    return fields;
}

std::string asciiLower(std::string_view text)
{
    std::string lowered(text);
    std::ranges::transform(lowered,
                           lowered.begin(),
                           [](char character)
                           { return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character; });
    return lowered;
}

std::string javaTrimmed(std::string_view text)
{
    return std::string(osp::javaTrim(text));
}

} // namespace motrack::library
