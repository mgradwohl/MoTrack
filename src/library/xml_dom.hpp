#pragma once

#include "osp/expected.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/// A small, bounded, non-validating XML 1.0 reader producing the DOM that `javax.xml.parsers.DocumentBuilder`
/// builds with its defaults (not namespace aware, comments kept, CDATA not coalesced). ComPADRE's REST service
/// returns real XML, which LibraryComPADRE reads with that parser, unlike OSP's own line-oriented XMLControl
/// format. Only UTF-8 input is accepted; a document type declaration is refused.
namespace motrack::library::xml
{

struct Node
{
    enum class Type : std::uint8_t
    {
        Element,
        Text, ///< Character data with references resolved and adjacent chunks merged, as Xerces builds Text nodes.
        CData,
        Comment,
        ProcessingInstruction,
    };
    Type type = Type::Element;
    std::string name{};  ///< Qualified element name (Node.getNodeName); the target of a processing instruction.
    std::string value{}; ///< Text, CDATA or comment content.
    std::vector<std::pair<std::string, std::string>> attributes{};
    std::vector<Node> children{};

    /// Element.getAttribute: the value, or "" when absent.
    [[nodiscard]] std::string_view attribute(std::string_view attributeName) const noexcept;
    /// The first child element with this name (LibraryComPADRE.getFirstChild).
    [[nodiscard]] const Node* firstChild(std::string_view childName) const noexcept;
    /// Every child element with this name (LibraryComPADRE.getAllChildren).
    [[nodiscard]] std::vector<const Node*> childrenNamed(std::string_view childName) const;
    /// LibraryComPADRE.getNodeValue: the first Text child's value (CDATA sections and comments are not Text).
    [[nodiscard]] std::optional<std::string_view> textValue() const noexcept;
};

/// LibraryComPADRE.getChildValue: the first child element's text value.
[[nodiscard]] std::optional<std::string_view> childValue(const Node* parent, std::string_view childName) noexcept;

/// Document.getElementsByTagName: every element with this name in document order, the root included.
[[nodiscard]] std::vector<const Node*> elementsByTagName(const Node& root, std::string_view name);

struct Limits
{
    std::size_t maxBytes = std::size_t{32} * 1024 * 1024;
    std::size_t maxDepth = 256;
    std::size_t maxNodes = 2'000'000;
};

struct ParseError
{
    std::size_t offset = 0;
    std::string message{};
};

/// Parses a whole document and returns its root element.
[[nodiscard]] osp::Expected<Node, ParseError> parse(std::string_view bytes, const Limits& limits = {});

} // namespace motrack::library::xml
