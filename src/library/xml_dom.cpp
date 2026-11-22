#include "library/xml_dom.hpp"

#include "library/java_string.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace motrack::library::xml
{
namespace
{

bool isXmlChar(std::uint32_t code) noexcept
{
    return code == 0x9 || code == 0xA || code == 0xD || (code >= 0x20 && code <= 0xD7FF) || (code >= 0xE000 && code <= 0xFFFD) ||
           (code >= 0x10000 && code <= 0x10FFFF);
}

/// Decodes one strict UTF-8 sequence; nullopt for malformed, overlong or surrogate encodings.
std::optional<std::uint32_t> decodeUtf8(std::string_view text, std::size_t& position) noexcept
{
    const auto lead = static_cast<unsigned char>(text[position]);
    if (lead < 0x80)
    {
        ++position;
        return lead;
    }
    std::size_t length = 0;
    std::uint32_t code = 0;
    std::uint32_t minimum = 0;
    if ((lead & 0xE0U) == 0xC0U)
    {
        length = 2;
        code = lead & 0x1FU;
        minimum = 0x80;
    }
    else if ((lead & 0xF0U) == 0xE0U)
    {
        length = 3;
        code = lead & 0x0FU;
        minimum = 0x800;
    }
    else if ((lead & 0xF8U) == 0xF0U)
    {
        length = 4;
        code = lead & 0x07U;
        minimum = 0x10000;
    }
    else
    {
        return std::nullopt;
    }
    if (position + length > text.size())
    {
        return std::nullopt;
    }
    for (std::size_t index = 1; index < length; ++index)
    {
        const auto next = static_cast<unsigned char>(text[position + index]);
        if ((next & 0xC0U) != 0x80U)
        {
            return std::nullopt;
        }
        code = (code << 6U) | (next & 0x3FU);
    }
    if (code < minimum || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF))
    {
        return std::nullopt;
    }
    position += length;
    return code;
}

void appendUtf8(std::string& output, std::uint32_t code)
{
    if (code < 0x80)
    {
        output.push_back(static_cast<char>(code));
    }
    else if (code < 0x800)
    {
        output.push_back(static_cast<char>(0xC0U | (code >> 6U)));
        output.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
    }
    else if (code < 0x10000)
    {
        output.push_back(static_cast<char>(0xE0U | (code >> 12U)));
        output.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
    }
    else
    {
        output.push_back(static_cast<char>(0xF0U | (code >> 18U)));
        output.push_back(static_cast<char>(0x80U | ((code >> 12U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
    }
}

bool isSpace(char character) noexcept
{
    return character == ' ' || character == '\t' || character == '\n' || character == '\r';
}

bool isNameStart(char character) noexcept
{
    const auto value = static_cast<unsigned char>(character);
    return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') || character == '_' || character == ':' ||
           value >= 0x80;
}

bool isNameChar(char character) noexcept
{
    return isNameStart(character) || (character >= '0' && character <= '9') || character == '-' || character == '.';
}

class Parser
{
  public:
    Parser(std::string text, const Limits& limits) : m_Text(std::move(text)), m_Limits(limits)
    {}

    osp::Expected<Node, ParseError> document()
    {
        if (m_Text.starts_with("\xEF\xBB\xBF"))
        {
            m_Position = 3;
        }
        if (m_Text.compare(m_Position, 5, "<?xml") == 0 && m_Position + 5 < m_Text.size() && isSpace(m_Text[m_Position + 5]))
        {
            if (auto declaration = xmlDeclaration(); !declaration)
            {
                return osp::makeUnexpected(declaration.error());
            }
        }
        std::vector<Node> ignored;
        if (auto misc = miscellaneous(ignored); !misc)
        {
            return osp::makeUnexpected(misc.error());
        }
        if (m_Text.compare(m_Position, 9, "<!DOCTYPE") == 0)
        {
            return error("document type declarations are not supported");
        }
        if (!peek('<'))
        {
            return error("missing root element");
        }
        auto root = element(0);
        if (!root)
        {
            return root;
        }
        if (auto misc = miscellaneous(ignored); !misc)
        {
            return osp::makeUnexpected(misc.error());
        }
        if (m_Position != m_Text.size())
        {
            return error("content after the root element");
        }
        return root;
    }

  private:
    [[nodiscard]] osp::Unexpected<ParseError> error(std::string message) const
    {
        return osp::makeUnexpected(ParseError{.offset = m_Position, .message = std::move(message)});
    }

    [[nodiscard]] bool peek(char character) const noexcept
    {
        return m_Position < m_Text.size() && m_Text[m_Position] == character;
    }

    [[nodiscard]] bool startsWith(std::string_view prefix) const noexcept
    {
        return std::string_view(m_Text).substr(m_Position).starts_with(prefix);
    }

    void skipSpace() noexcept
    {
        while (m_Position < m_Text.size() && isSpace(m_Text[m_Position]))
        {
            ++m_Position;
        }
    }

    osp::Expected<bool, ParseError> counted()
    {
        if (++m_Nodes > m_Limits.maxNodes)
        {
            return error("too many XML nodes");
        }
        return true;
    }

    osp::Expected<std::string, ParseError> name()
    {
        const auto start = m_Position;
        if (m_Position >= m_Text.size() || !isNameStart(m_Text[m_Position]))
        {
            return error("invalid name");
        }
        while (m_Position < m_Text.size() && isNameChar(m_Text[m_Position]))
        {
            ++m_Position;
        }
        return m_Text.substr(start, m_Position - start);
    }

    osp::Expected<bool, ParseError> xmlDeclaration()
    {
        const auto end = m_Text.find("?>", m_Position);
        if (end == std::string::npos)
        {
            return error("unterminated XML declaration");
        }
        const auto declaration = std::string_view(m_Text).substr(m_Position, end - m_Position);
        if (const auto encoding = declaration.find("encoding"); encoding != std::string_view::npos)
        {
            const auto quote = declaration.find_first_of("\"'", encoding);
            const auto close = quote == std::string_view::npos ? quote : declaration.find(declaration[quote], quote + 1);
            if (close == std::string_view::npos)
            {
                return error("malformed XML declaration");
            }
            const auto value = asciiLower(declaration.substr(quote + 1, close - quote - 1));
            if (value != "utf-8" && value != "utf8")
            {
                return error("only UTF-8 documents are supported");
            }
        }
        m_Position = end + 2;
        return true;
    }

    /// Comments, processing instructions and white space outside the root element.
    osp::Expected<bool, ParseError> miscellaneous(std::vector<Node>& nodes)
    {
        while (true)
        {
            skipSpace();
            if (startsWith("<!--"))
            {
                auto node = comment();
                if (!node)
                {
                    return osp::makeUnexpected(node.error());
                }
                nodes.push_back(*std::move(node));
            }
            else if (startsWith("<?"))
            {
                auto node = processingInstruction();
                if (!node)
                {
                    return osp::makeUnexpected(node.error());
                }
                nodes.push_back(*std::move(node));
            }
            else
            {
                return true;
            }
        }
    }

    osp::Expected<Node, ParseError> comment()
    {
        if (auto count = counted(); !count)
        {
            return osp::makeUnexpected(count.error());
        }
        m_Position += 4;
        const auto end = m_Text.find("--", m_Position);
        if (end == std::string::npos || end + 2 >= m_Text.size() || m_Text[end + 2] != '>')
        {
            return error("malformed comment");
        }
        Node node{.type = Node::Type::Comment, .value = m_Text.substr(m_Position, end - m_Position)};
        m_Position = end + 3;
        return node;
    }

    osp::Expected<Node, ParseError> processingInstruction()
    {
        if (auto count = counted(); !count)
        {
            return osp::makeUnexpected(count.error());
        }
        m_Position += 2;
        auto target = name();
        if (!target)
        {
            return osp::makeUnexpected(target.error());
        }
        if (asciiLower(*target) == "xml")
        {
            return error("misplaced XML declaration");
        }
        const auto end = m_Text.find("?>", m_Position);
        if (end == std::string::npos)
        {
            return error("unterminated processing instruction");
        }
        Node node{.type = Node::Type::ProcessingInstruction, .name = *std::move(target)};
        skipSpace();
        node.value = m_Text.substr(m_Position, end - m_Position);
        m_Position = end + 2;
        return node;
    }

    /// An entity or character reference at '&', appended to `output`.
    osp::Expected<bool, ParseError> reference(std::string& output)
    {
        const auto end = m_Text.find(';', m_Position);
        if (end == std::string::npos)
        {
            return error("unterminated reference");
        }
        const auto body = std::string_view(m_Text).substr(m_Position + 1, end - m_Position - 1);
        if (body == "lt")
        {
            output.push_back('<');
        }
        else if (body == "gt")
        {
            output.push_back('>');
        }
        else if (body == "amp")
        {
            output.push_back('&');
        }
        else if (body == "apos")
        {
            output.push_back('\'');
        }
        else if (body == "quot")
        {
            output.push_back('"');
        }
        else if (body.starts_with('#') && body.size() > 1)
        {
            const bool hex = body[1] == 'x';
            const auto digits = body.substr(hex ? 2 : 1);
            if (digits.empty() || digits.size() > 8)
            {
                return error("invalid character reference");
            }
            std::uint32_t code = 0;
            for (const char digit : digits)
            {
                std::uint32_t value = 0;
                if (digit >= '0' && digit <= '9')
                {
                    value = static_cast<std::uint32_t>(digit - '0');
                }
                else if (hex && digit >= 'a' && digit <= 'f')
                {
                    value = static_cast<std::uint32_t>(digit - 'a' + 10);
                }
                else if (hex && digit >= 'A' && digit <= 'F')
                {
                    value = static_cast<std::uint32_t>(digit - 'A' + 10);
                }
                else
                {
                    return error("invalid character reference");
                }
                code = code * (hex ? 16U : 10U) + value;
            }
            if (!isXmlChar(code))
            {
                return error("character reference to an invalid character");
            }
            appendUtf8(output, code);
        }
        else
        {
            return error("undefined entity");
        }
        m_Position = end + 1;
        return true;
    }

    osp::Expected<std::string, ParseError> attributeValue()
    {
        if (!peek('"') && !peek('\''))
        {
            return error("attribute value must be quoted");
        }
        const char quote = m_Text[m_Position++];
        std::string value;
        while (true)
        {
            if (m_Position >= m_Text.size())
            {
                return error("unterminated attribute value");
            }
            const char character = m_Text[m_Position];
            if (character == quote)
            {
                ++m_Position;
                return value;
            }
            if (character == '<')
            {
                return error("'<' in an attribute value");
            }
            if (character == '&')
            {
                if (auto resolved = reference(value); !resolved)
                {
                    return osp::makeUnexpected(resolved.error());
                }
                continue;
            }
            // Attribute-value normalization: literal white space becomes a space.
            value.push_back(isSpace(character) ? ' ' : character);
            ++m_Position;
        }
    }

    osp::Expected<Node, ParseError> element(std::size_t depth)
    {
        if (depth >= m_Limits.maxDepth)
        {
            return error("XML nesting too deep");
        }
        if (auto count = counted(); !count)
        {
            return osp::makeUnexpected(count.error());
        }
        ++m_Position;
        auto elementName = name();
        if (!elementName)
        {
            return osp::makeUnexpected(elementName.error());
        }
        Node node{.type = Node::Type::Element, .name = *std::move(elementName)};
        while (true)
        {
            const auto before = m_Position;
            skipSpace();
            if (peek('/'))
            {
                if (m_Text.compare(m_Position, 2, "/>") != 0)
                {
                    return error("malformed empty-element tag");
                }
                m_Position += 2;
                return node;
            }
            if (peek('>'))
            {
                ++m_Position;
                break;
            }
            if (m_Position == before)
            {
                return error("attributes must be separated by white space");
            }
            auto attributeName = name();
            if (!attributeName)
            {
                return osp::makeUnexpected(attributeName.error());
            }
            skipSpace();
            if (!peek('='))
            {
                return error("attribute without a value");
            }
            ++m_Position;
            skipSpace();
            auto value = attributeValue();
            if (!value)
            {
                return osp::makeUnexpected(value.error());
            }
            if (std::ranges::any_of(node.attributes, [&attributeName](const auto& attribute) { return attribute.first == *attributeName; }))
            {
                return error("duplicate attribute");
            }
            node.attributes.emplace_back(*std::move(attributeName), *std::move(value));
        }
        std::string text;
        bool hasText = false;
        const auto flush = [&node, &text, &hasText, this]() -> osp::Expected<bool, ParseError>
        {
            if (hasText)
            {
                if (auto count = counted(); !count)
                {
                    return count;
                }
                node.children.push_back(Node{.type = Node::Type::Text, .value = std::move(text)});
                text.clear();
                hasText = false;
            }
            return true;
        };
        while (true)
        {
            if (m_Position >= m_Text.size())
            {
                return error("unterminated element \"" + node.name + "\"");
            }
            if (startsWith("</"))
            {
                m_Position += 2;
                auto endName = name();
                if (!endName || *endName != node.name)
                {
                    return error("mismatched end tag for \"" + node.name + "\"");
                }
                skipSpace();
                if (!peek('>'))
                {
                    return error("malformed end tag");
                }
                ++m_Position;
                if (auto flushed = flush(); !flushed)
                {
                    return osp::makeUnexpected(flushed.error());
                }
                return node;
            }
            osp::Expected<Node, ParseError> child = Node{};
            if (startsWith("<!--"))
            {
                child = comment();
            }
            else if (startsWith("<![CDATA["))
            {
                if (auto count = counted(); !count)
                {
                    return osp::makeUnexpected(count.error());
                }
                const auto end = m_Text.find("]]>", m_Position + 9);
                if (end == std::string::npos)
                {
                    return error("unterminated CDATA section");
                }
                child = Node{.type = Node::Type::CData, .value = m_Text.substr(m_Position + 9, end - m_Position - 9)};
                m_Position = end + 3;
            }
            else if (startsWith("<?"))
            {
                child = processingInstruction();
            }
            else if (startsWith("<!"))
            {
                return error("unsupported markup declaration");
            }
            else if (peek('<'))
            {
                child = element(depth + 1);
            }
            else if (peek('&'))
            {
                if (auto resolved = reference(text); !resolved)
                {
                    return osp::makeUnexpected(resolved.error());
                }
                hasText = true;
                continue;
            }
            else
            {
                if (startsWith("]]>"))
                {
                    return error("\"]]>\" in character data");
                }
                text.push_back(m_Text[m_Position++]);
                hasText = true;
                continue;
            }
            if (!child)
            {
                return child;
            }
            if (auto flushed = flush(); !flushed)
            {
                return osp::makeUnexpected(flushed.error());
            }
            node.children.push_back(*std::move(child));
        }
    }

    std::string m_Text;
    Limits m_Limits;
    std::size_t m_Position = 0;
    std::size_t m_Nodes = 0;
};

void collect(const Node& node, std::string_view name, std::vector<const Node*>& found)
{
    if (node.type != Node::Type::Element)
    {
        return;
    }
    if (node.name == name)
    {
        found.push_back(&node);
    }
    for (const auto& child : node.children)
    {
        collect(child, name, found);
    }
}

} // namespace

std::string_view Node::attribute(std::string_view attributeName) const noexcept
{
    for (const auto& [key, value] : attributes)
    {
        if (key == attributeName)
        {
            return value;
        }
    }
    return {};
}

const Node* Node::firstChild(std::string_view childName) const noexcept
{
    for (const auto& child : children)
    {
        if (child.type == Type::Element && child.name == childName)
        {
            return &child;
        }
    }
    return nullptr;
}

std::vector<const Node*> Node::childrenNamed(std::string_view childName) const
{
    std::vector<const Node*> found;
    for (const auto& child : children)
    {
        if (child.type == Type::Element && child.name == childName)
        {
            found.push_back(&child);
        }
    }
    return found;
}

std::optional<std::string_view> Node::textValue() const noexcept
{
    for (const auto& child : children)
    {
        if (child.type == Type::Text)
        {
            return std::string_view(child.value);
        }
    }
    return std::nullopt;
}

std::optional<std::string_view> childValue(const Node* parent, std::string_view childName) noexcept
{
    if (parent == nullptr)
    {
        return std::nullopt;
    }
    const auto* child = parent->firstChild(childName);
    return child == nullptr ? std::nullopt : child->textValue();
}

std::vector<const Node*> elementsByTagName(const Node& root, std::string_view name)
{
    std::vector<const Node*> found;
    collect(root, name, found);
    return found;
}

osp::Expected<Node, ParseError> parse(std::string_view bytes, const Limits& limits)
{
    if (bytes.size() > limits.maxBytes)
    {
        return osp::makeUnexpected(ParseError{.message = "XML document too large"});
    }
    // Validate the characters and normalize line ends ("\r\n" and "\r" become "\n") before parsing, as XML requires.
    std::string text;
    text.reserve(bytes.size());
    for (std::size_t position = 0; position < bytes.size();)
    {
        const auto start = position;
        const auto code = decodeUtf8(bytes, position);
        if (!code || !isXmlChar(*code))
        {
            return osp::makeUnexpected(ParseError{.offset = start, .message = code ? "invalid XML character" : "invalid UTF-8"});
        }
        if (*code == '\r')
        {
            text.push_back('\n');
            if (position < bytes.size() && bytes[position] == '\n')
            {
                ++position;
            }
            continue;
        }
        text.append(bytes.substr(start, position - start));
    }
    return Parser(std::move(text), limits).document();
}

} // namespace motrack::library::xml
