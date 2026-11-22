#include "osp/source.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace motrack::osp
{
namespace
{

// XML.java constants.
constexpr std::string_view CDATA_PRE = "<![CDATA[";
constexpr std::string_view CDATA_POST = "]]>";
constexpr std::string_view CDATA_POST_PROPERTY = "]]></property>";
constexpr std::string_view PROPERTY_END = "</property>";
constexpr std::string_view OBJECT_END = "</object>";
constexpr std::string_view CRYPTIC_CLASS = "org.opensourcephysics.controls.Cryptic";
constexpr std::size_t ROOT_SEARCH_LINES = 10;

bool contains(std::string_view text, std::string_view pattern) noexcept
{
    return text.find(pattern) != std::string_view::npos;
}

std::size_t index(NodeId id) noexcept
{
    return static_cast<std::size_t>(id);
}

/// XML.getAttr (line 130): the text after the first `attr="` up to the next quote. A missing closing quote
/// makes Java's substring throw.
enum class AttributeStatus : std::uint8_t
{
    Found,
    Absent,
    Unterminated,
};

struct Attribute
{
    AttributeStatus status = AttributeStatus::Absent;
    std::string_view value{};
};

Attribute attribute(std::string_view line, std::string_view name)
{
    std::string pattern(name);
    pattern += "=\"";
    const auto start = line.find(pattern);
    if (start == std::string_view::npos)
    {
        return {};
    }
    const auto begin = start + pattern.size();
    const auto end = line.find('"', begin);
    if (end == std::string_view::npos)
    {
        return {.status = AttributeStatus::Unterminated};
    }
    return {.status = AttributeStatus::Found, .value = line.substr(begin, end - begin)};
}

/// getClassName catches every exception and returns "".
std::string_view classAttribute(std::string_view line)
{
    const auto value = attribute(line, "class");
    return value.status == AttributeStatus::Found ? value.value : std::string_view{};
}

bool validUtf8(std::string_view bytes, std::size_t& failure) noexcept
{
    std::size_t position = 0;
    while (position < bytes.size())
    {
        const auto lead = static_cast<unsigned char>(bytes[position]);
        if (lead < 0x80)
        {
            ++position;
            continue;
        }
        std::size_t length = 4;
        std::uint32_t minimum = 0x10000;
        std::uint32_t value = lead & 0x07U;
        if (lead >= 0xC2 && lead <= 0xDF)
        {
            length = 2;
            minimum = 0x80;
            value = lead & 0x1FU;
        }
        else if (lead >= 0xE0 && lead <= 0xEF)
        {
            length = 3;
            minimum = 0x800;
            value = lead & 0x0FU;
        }
        else if (lead < 0xF0 || lead > 0xF4)
        {
            failure = position;
            return false;
        }
        if (bytes.size() - position < length)
        {
            failure = position;
            return false;
        }
        for (std::size_t offset = 1; offset < length; ++offset)
        {
            const auto continuation = static_cast<unsigned char>(bytes[position + offset]);
            if ((continuation & 0xC0U) != 0x80U)
            {
                failure = position;
                return false;
            }
            value = (value << 6) | (continuation & 0x3FU);
        }
        if (value < minimum || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF))
        {
            failure = position;
            return false;
        }
        position += length;
    }
    return true;
}

/// BufferedReader.readLine: "\n", "\r\n" and a lone "\r" end a line; a final terminator adds no empty line.
std::vector<LineSpan> splitLines(std::string_view bytes)
{
    std::vector<LineSpan> lines;
    std::size_t begin = 0;
    while (begin < bytes.size())
    {
        const auto end = bytes.find_first_of("\r\n", begin);
        if (end == std::string_view::npos)
        {
            lines.push_back({.begin = begin, .end = bytes.size(), .next = bytes.size()});
            break;
        }
        const auto next = (bytes[end] == '\r' && end + 1 < bytes.size() && bytes[end + 1] == '\n') ? end + 2 : end + 1;
        lines.push_back({.begin = begin, .end = end, .next = next});
        begin = next;
    }
    return lines;
}

char lowerAscii(char value) noexcept
{
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

bool utf8Name(std::string_view value) noexcept
{
    const auto equals = [&](std::string_view name)
    {
        return std::ranges::equal(value, name, [](char left, char right) { return lowerAscii(left) == right; });
    };
    return equals("utf-8") || equals("utf8");
}

/// OSP ignores the XML declaration and always decodes UTF-8 (ResourceLoader.readerForStream). MoTrack refuses
/// a declaration naming another encoding rather than misreading it.
bool declaresOtherEncoding(std::string_view prolog)
{
    const auto declaration = prolog.find("<?xml");
    if (declaration == std::string_view::npos)
    {
        return false;
    }
    const auto close = prolog.find("?>", declaration);
    const auto text = prolog.substr(declaration, close == std::string_view::npos ? std::string_view::npos : close - declaration);
    const auto key = text.find("encoding");
    if (key == std::string_view::npos)
    {
        return false;
    }
    auto position = text.find_first_not_of(" \t\r\n", key + 8);
    if (position == std::string_view::npos || text[position] != '=')
    {
        return true;
    }
    position = text.find_first_not_of(" \t\r\n", position + 1);
    if (position == std::string_view::npos || (text[position] != '"' && text[position] != '\''))
    {
        return true;
    }
    const auto quote = text[position];
    const auto end = text.find(quote, position + 1);
    if (end == std::string_view::npos)
    {
        return true;
    }
    return !utf8Name(text.substr(position + 1, end - position - 1));
}

} // namespace

/// A port of XMLControlElement.readInput/readObject/readProperty. Every readLine() of the Java reader is one
/// `readLine()` here, so line consumption and therefore the resulting tree match exactly.
class SourceParser
{
  public:
    SourceParser(Source& source, const ParseLimits& limits) : m_Source(source), m_Limits(limits)
    {}

    Expected<void, ParseError> run()
    {
        const std::string_view bytes = m_Source.m_Bytes;
        if (bytes.size() > m_Limits.maxBytes)
        {
            return fail(ParseError::Code::TooLarge, 0, "The document exceeds the XML size limit.");
        }
        if (std::size_t failure = 0; !validUtf8(bytes, failure))
        {
            const auto line =
                static_cast<std::size_t>(std::count(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(failure), '\n'));
            return fail(ParseError::Code::InvalidUtf8, line + 1, "The document is not valid UTF-8.");
        }
        m_Source.m_Lines = splitLines(bytes);
        for (const auto& line : m_Source.m_Lines)
        {
            if (line.next > line.end)
            {
                const auto terminator = bytes.substr(line.end, line.next - line.end);
                m_Source.m_LineEnding = terminator == "\r\n" ? LineEnding::CrLf : terminator == "\r" ? LineEnding::Cr : LineEnding::Lf;
                break;
            }
        }
        // readInput (line 1581): the root must be on one of the first ten lines.
        auto rootLine = readLine();
        for (std::size_t count = 0; rootLine && !contains(text(*rootLine), "<object class=");)
        {
            if (++count >= ROOT_SEARCH_LINES)
            {
                return fail(ParseError::Code::MissingRoot, *rootLine + 1, "No OSP root object in the first 10 lines.");
            }
            rootLine = readLine();
        }
        if (!rootLine)
        {
            return fail(ParseError::Code::MissingRoot, 0, "No OSP root object in the first 10 lines.");
        }
        if (declaresOtherEncoding(bytes.substr(0, m_Source.m_Lines[*rootLine].end)))
        {
            return fail(ParseError::Code::UnsupportedEncoding, 1, "Only UTF-8 OSP documents are supported.");
        }
        const auto version = attribute(text(*rootLine), "version");
        if (version.status == AttributeStatus::Unterminated)
        {
            return fail(ParseError::Code::MalformedAttribute, *rootLine + 1, "Unterminated version attribute.");
        }
        auto root = readObject(std::nullopt, *rootLine, 0);
        if (!root)
        {
            return makeUnexpected(std::move(root.error()));
        }
        auto& node = m_Source.m_Nodes[index(*root)];
        if (version.status == AttributeStatus::Found)
        {
            node.version = std::string(version.value);
        }
        if (node.className == CRYPTIC_CLASS)
        {
            return fail(ParseError::Code::Encrypted, *rootLine + 1, "Password-protected OSP documents are not supported.");
        }
        return {};
    }

  private:
    static Unexpected<ParseError> fail(ParseError::Code code, std::size_t line, std::string message)
    {
        return makeUnexpected(ParseError{.code = code, .line = line, .message = std::move(message)});
    }

    std::optional<std::size_t> readLine() noexcept
    {
        if (m_Next < m_Source.m_Lines.size())
        {
            return m_Next++;
        }
        return std::nullopt;
    }

    [[nodiscard]] std::string_view text(std::size_t line) const
    {
        const auto& span = m_Source.m_Lines[line];
        return std::string_view(m_Source.m_Bytes).substr(span.begin, span.end - span.begin);
    }

    Expected<NodeId, ParseError> add(Node node, std::size_t line)
    {
        if (m_Source.m_Nodes.size() >= m_Limits.maxNodes)
        {
            return fail(ParseError::Code::TooManyNodes, line + 1, "The document exceeds the node limit.");
        }
        if (node.depth > m_Limits.maxDepth)
        {
            return fail(ParseError::Code::TooDeep, line + 1, "The document exceeds the nesting limit.");
        }
        m_Source.m_Nodes.push_back(std::move(node));
        return static_cast<NodeId>(m_Source.m_Nodes.size() - 1);
    }

    Node& at(NodeId id)
    {
        return m_Source.m_Nodes[index(id)];
    }

    /// readObject (line 1680).
    Expected<NodeId, ParseError> readObject(std::optional<NodeId> parent, std::size_t line, std::uint32_t depth)
    {
        const auto opening = text(line);
        const auto rawClass = classAttribute(opening);
        auto id = add({.kind = NodeKind::Object,
                       .parent = parent,
                       .depth = depth,
                       .className = effectiveClassName(rawClass),
                       .rawClassName = std::string(rawClass),
                       .firstLine = line,
                       .lastLine = line},
                      line);
        if (!id)
        {
            return id;
        }
        // Any "/>" on the opening line closes the object, and the reader then discards the following line.
        if (contains(opening, "/>"))
        {
            at(*id).selfClosing = true;
            at(*id).closeLine = line;
            at(*id).lastLine = readLine().value_or(line);
            return id;
        }
        for (auto current = readLine(); current; current = readLine())
        {
            const auto content = text(*current);
            if (contains(content, OBJECT_END))
            {
                // "</object>" also discards the following line, normally the parent's "</property>".
                at(*id).closeLine = *current;
                at(*id).lastLine = readLine().value_or(*current);
                return id;
            }
            if (contains(content, "<property"))
            {
                auto child = readProperty(*id, *current, depth + 1);
                if (!child)
                {
                    return child;
                }
                const auto& property = at(*child);
                if (property.type == PropertyType::Object && property.isNull)
                {
                    // setProperty reads the missing object with content.get(0) and the read fails.
                    return fail(
                        ParseError::Code::NullObjectProperty, property.firstLine + 1, "A null object property cannot belong to an object.");
                }
                at(*id).children.push_back(*child);
            }
            // Other lines (blank, comments, stray text) are ignored.
        }
        at(*id).closed = false;
        at(*id).lastLine = m_Next - 1;
        m_Source.m_Truncated = true;
        return id;
    }

    /// readProperty (line 1736).
    Expected<NodeId, ParseError> readProperty(NodeId parent, std::size_t line, std::uint32_t depth)
    {
        const auto opening = text(line);
        const auto name = attribute(opening, "name");
        const auto type = attribute(opening, "type");
        if (name.status == AttributeStatus::Unterminated || type.status == AttributeStatus::Unterminated)
        {
            return fail(ParseError::Code::MalformedAttribute, line + 1, "Unterminated property attribute.");
        }
        if (type.status == AttributeStatus::Absent)
        {
            return fail(ParseError::Code::MissingType, line + 1, "Property without a type attribute.");
        }
        Node node{.kind = NodeKind::Property,
                  .type = propertyTypeFromName(type.value),
                  .parent = parent,
                  .depth = depth,
                  .typeName = std::string(type.value),
                  .firstLine = line,
                  .lastLine = line};
        if (name.status == AttributeStatus::Found)
        {
            node.name = std::string(name.value);
        }
        auto id = add(std::move(node), line);
        if (!id)
        {
            return id;
        }
        switch (at(*id).type)
        {
        case PropertyType::Array:
        case PropertyType::Collection:
            return readElements(*id, line, depth);
        case PropertyType::Object:
            return readObjectProperty(*id, line, depth);
        case PropertyType::String:
            if (const auto start = opening.find(CDATA_PRE); start != std::string_view::npos)
            {
                at(*id).cdata = true;
                return readText(*id, line, start + CDATA_PRE.size(), CDATA_POST_PROPERTY, CDATA_POST.size());
            }
            [[fallthrough]];
        default:
        {
            // Content starts after the line's first '>', or at the line start when there is none.
            const auto close = opening.find('>');
            return readText(*id, line, close == std::string_view::npos ? 0 : close + 1, PROPERTY_END, 0);
        }
        }
    }

    Expected<NodeId, ParseError> readElements(NodeId id, std::size_t line, std::uint32_t depth)
    {
        const auto opening = text(line);
        const auto rawClass = classAttribute(opening);
        at(id).rawClassName = std::string(rawClass);
        at(id).className = effectiveClassName(rawClass);
        if (contains(opening, "/>"))
        {
            at(id).selfClosing = true;
            at(id).closeLine = line;
            return id;
        }
        // Consecutive "<property" lines are elements; the first other line (often a blank line) ends the
        // list and is consumed, even when it is not this property's "</property>".
        auto current = readLine();
        while (current && contains(text(*current), "<property"))
        {
            auto child = readProperty(id, *current, depth + 1);
            if (!child)
            {
                return child;
            }
            at(id).children.push_back(*child);
            current = readLine();
        }
        if (!current)
        {
            return fail(ParseError::Code::UnterminatedArray, line + 1, "End of file inside an array or collection.");
        }
        at(id).closeLine = *current;
        at(id).lastLine = *current;
        return id;
    }

    Expected<NodeId, ParseError> readObjectProperty(NodeId id, std::size_t line, std::uint32_t depth)
    {
        if (contains(text(line), ">null</property"))
        {
            at(id).isNull = true;
            return id;
        }
        // The next line is read as the object's opening tag whatever it contains.
        const auto objectLine = readLine();
        if (!objectLine)
        {
            return fail(ParseError::Code::MissingObject, line + 1, "End of file where an object should start.");
        }
        auto object = readObject(id, *objectLine, depth + 1);
        if (!object)
        {
            return object;
        }
        at(id).children.push_back(*object);
        at(id).className = at(*object).className;
        at(id).rawClassName = at(*object).rawClassName;
        at(id).lastLine = m_Next - 1;
        return id;
    }

    /// Reads text from `start` on the opening line until `terminator`, joining physical lines with "\n".
    /// `valueEndOffset` locates "</property>" inside the terminator.
    Expected<NodeId, ParseError>
    readText(NodeId id, std::size_t line, std::size_t start, std::string_view terminator, std::size_t valueEndOffset)
    {
        const auto opening = text(line);
        const auto& first = m_Source.m_Lines[line];
        const auto close = opening.find('>');
        at(id).valueBegin = first.begin + (close == std::string_view::npos ? 0 : close + 1);
        std::string content;
        auto current = line;
        auto searchFrom = start;
        for (;;)
        {
            const auto lineText = text(current);
            const auto match = lineText.find(terminator, searchFrom);
            if (match != std::string_view::npos)
            {
                content.append(lineText.substr(searchFrom, match - searchFrom));
                at(id).valueEnd = m_Source.m_Lines[current].begin + match + valueEndOffset;
                break;
            }
            content.append(lineText.substr(std::min(searchFrom, lineText.size())));
            const auto next = readLine();
            if (!next)
            {
                return fail(ParseError::Code::UnterminatedValue, line + 1, "End of file inside a property value.");
            }
            content.push_back('\n');
            current = *next;
            searchFrom = 0;
        }
        at(id).content = std::move(content);
        at(id).lastLine = current;
        return id;
    }

    Source& m_Source;
    const ParseLimits& m_Limits;
    std::size_t m_Next = 0;
};

PropertyType propertyTypeFromName(std::string_view name) noexcept
{
    if (name == "int")
        return PropertyType::Int;
    if (name == "double")
        return PropertyType::Double;
    if (name == "boolean")
        return PropertyType::Boolean;
    if (name == "string")
        return PropertyType::String;
    if (name == "array")
        return PropertyType::Array;
    if (name == "collection")
        return PropertyType::Collection;
    if (name == "object")
        return PropertyType::Object;
    return PropertyType::Unknown;
}

std::string_view propertyTypeName(PropertyType type) noexcept
{
    switch (type)
    {
    case PropertyType::Int:
        return "int";
    case PropertyType::Double:
        return "double";
    case PropertyType::Boolean:
        return "boolean";
    case PropertyType::String:
        return "string";
    case PropertyType::Array:
        return "array";
    case PropertyType::Collection:
        return "collection";
    case PropertyType::Object:
        return "object";
    case PropertyType::Unknown:
        break;
    }
    return "unknown";
}

bool isTextType(PropertyType type) noexcept
{
    return type != PropertyType::Array && type != PropertyType::Collection && type != PropertyType::Object;
}

std::string effectiveClassName(std::string_view className)
{
    constexpr std::string_view MEDIA = "org.opensourcephysics.media";
    const auto dot = className.rfind('.');
    if (dot != std::string_view::npos && className.substr(0, dot).ends_with(MEDIA))
    {
        std::string moved(className.substr(0, dot));
        moved += ".core";
        moved += className.substr(dot);
        return moved;
    }
    return std::string(className);
}

std::string_view lineEndingText(LineEnding ending) noexcept
{
    switch (ending)
    {
    case LineEnding::CrLf:
        return "\r\n";
    case LineEnding::Cr:
        return "\r";
    case LineEnding::Lf:
        break;
    }
    return "\n";
}

Expected<std::shared_ptr<const Source>, ParseError> parse(std::string bytes, const ParseLimits& limits)
{
    // The constructor is private so that every Source satisfies the parser's invariants; make_shared cannot use it.
    std::shared_ptr<Source> source(new Source()); // NOLINT(modernize-make-shared)
    source->m_Bytes = std::move(bytes);
    SourceParser parser(*source, limits);
    if (auto result = parser.run(); !result)
    {
        return makeUnexpected(std::move(result.error()));
    }
    return std::shared_ptr<const Source>(std::move(source));
}

std::string_view Source::bytes() const noexcept
{
    return m_Bytes;
}

NodeId Source::root() const noexcept
{
    return NodeId{0};
}

std::size_t Source::nodeCount() const noexcept
{
    return m_Nodes.size();
}

bool Source::contains(NodeId id) const noexcept
{
    return index(id) < m_Nodes.size();
}

const Node& Source::node(NodeId id) const
{
    return m_Nodes.at(index(id));
}

std::span<const LineSpan> Source::lines() const noexcept
{
    return m_Lines;
}

std::string_view Source::lineText(std::size_t line) const
{
    const auto& span = m_Lines.at(line);
    return std::string_view(m_Bytes).substr(span.begin, span.end - span.begin);
}

LineEnding Source::lineEnding() const noexcept
{
    return m_LineEnding;
}

std::string_view Source::rawText(NodeId id) const
{
    const auto& item = node(id);
    const auto begin = m_Lines[item.firstLine].begin;
    return std::string_view(m_Bytes).substr(begin, m_Lines[item.lastLine].end - begin);
}

std::string_view Source::prolog() const
{
    return std::string_view(m_Bytes).substr(0, m_Lines[m_Nodes.front().firstLine].begin);
}

std::string_view Source::epilog() const
{
    return std::string_view(m_Bytes).substr(m_Lines[m_Nodes.front().lastLine].next);
}

bool Source::truncated() const noexcept
{
    return m_Truncated;
}

namespace
{

std::size_t arrayDepth(std::string_view arrayClass) noexcept
{
    return arrayClass.find_first_not_of('[');
}

Expected<CompactArray, DecodeError> parseLeaf(std::string_view text, char component)
{
    // s.substring(1, s.length() - 1).split(",")
    const auto fields = javaSplitCommas(text.substr(1, text.size() - 2));
    CompactArray result;
    switch (component)
    {
    case 'I':
        result.kind = CompactArray::Kind::Int;
        for (const auto field : fields)
        {
            const auto value = parseJavaInt(field);
            if (!value)
            {
                return makeUnexpected(value.error());
            }
            result.ints.push_back(*value);
        }
        break;
    case 'D':
        result.kind = CompactArray::Kind::Double;
        for (const auto field : fields)
        {
            const auto value = parseJavaDouble(field);
            if (!value)
            {
                return makeUnexpected(value.error());
            }
            result.doubles.push_back(*value);
        }
        break;
    default:
        result.kind = CompactArray::Kind::Boolean;
        for (const auto field : fields)
        {
            result.booleans.push_back(parseJavaBoolean(field));
        }
        break;
    }
    return result;
}

Expected<CompactArray, DecodeError> parseCompact(std::string_view text, std::string_view arrayClass)
{
    if (!text.starts_with('{') || !text.ends_with('}') || text.size() < 2)
    {
        return makeUnexpected(DecodeError::Malformed);
    }
    if (arrayClass.size() == 2)
    {
        return parseLeaf(text, arrayClass[1]);
    }
    // Nested: scan the characters between the outer braces (line 2017). The final closing brace is excluded,
    // so a trailing separator adds no null row.
    CompactArray result{.kind = CompactArray::Kind::Nested};
    const auto end = text.size() - 1;
    bool afterRow = false;
    for (std::size_t position = 1; position < end; ++position)
    {
        const auto character = text[position];
        if (character == ',' || character == '}')
        {
            if (afterRow)
            {
                afterRow = false;
            }
            else
            {
                result.rows.push_back({});
            }
        }
        else if (character == '{')
        {
            // indexOfClosingBrace (line 2085) searches before the final brace only.
            std::size_t closing = std::string_view::npos;
            for (std::size_t depth = 0, scan = position; scan < end; ++scan)
            {
                if (text[scan] == '{')
                {
                    ++depth;
                }
                else if (text[scan] == '}' && --depth == 0)
                {
                    closing = scan;
                    break;
                }
            }
            if (closing == std::string_view::npos)
            {
                return makeUnexpected(DecodeError::Malformed);
            }
            auto row = parseCompact(text.substr(position, closing - position + 1), arrayClass.substr(1));
            if (!row)
            {
                return row;
            }
            result.rows.push_back(std::move(*row));
            position = closing;
            afterRow = true;
        }
    }
    return result;
}

void formatCompactInto(std::string& output, const CompactArray& array)
{
    if (array.kind == CompactArray::Kind::Null)
    {
        output += "null";
        return;
    }
    output.push_back('{');
    const auto separator = [&](std::size_t position)
    {
        if (position > 0)
        {
            output.push_back(',');
        }
    };
    switch (array.kind)
    {
    case CompactArray::Kind::Int:
        for (std::size_t position = 0; position < array.ints.size(); ++position)
        {
            separator(position);
            output += formatJavaInt(array.ints[position]);
        }
        break;
    case CompactArray::Kind::Double:
        for (std::size_t position = 0; position < array.doubles.size(); ++position)
        {
            separator(position);
            output += formatJavaDouble(array.doubles[position]);
        }
        break;
    case CompactArray::Kind::Boolean:
        for (std::size_t position = 0; position < array.booleans.size(); ++position)
        {
            separator(position);
            output += array.booleans[position] ? "true" : "false";
        }
        break;
    default:
        for (std::size_t position = 0; position < array.rows.size(); ++position)
        {
            separator(position);
            formatCompactInto(output, array.rows[position]);
        }
        break;
    }
    output.push_back('}');
}

} // namespace

bool isPrimitiveArrayClass(std::string_view arrayClass) noexcept
{
    const auto depth = arrayDepth(arrayClass);
    return depth != std::string_view::npos && depth > 0 && depth + 1 == arrayClass.size() &&
           (arrayClass.back() == 'I' || arrayClass.back() == 'D' || arrayClass.back() == 'Z');
}

Expected<CompactArray, DecodeError> parseCompactArray(std::string_view text, std::string_view arrayClass)
{
    if (!isPrimitiveArrayClass(arrayClass))
    {
        return makeUnexpected(DecodeError::Unsupported);
    }
    return parseCompact(text, arrayClass);
}

std::string formatCompactArray(const CompactArray& array)
{
    std::string output;
    formatCompactInto(output, array);
    return output;
}

Expected<std::string, DecodeError> removeCdata(std::string_view content)
{
    const auto start = content.find(CDATA_PRE);
    if (start == std::string_view::npos)
    {
        return std::string(content);
    }
    // content.substring(pt + 9, content.indexOf("]]>")) searches for the end from the beginning.
    const auto end = content.find(CDATA_POST);
    const auto begin = start + CDATA_PRE.size();
    if (end == std::string_view::npos || end < begin)
    {
        return makeUnexpected(DecodeError::Malformed);
    }
    return std::string(content.substr(begin, end - begin));
}

PropertyView::PropertyView(const Source& source, NodeId id) : m_Source(&source), m_Id(id)
{
    if (source.node(id).kind != NodeKind::Property)
    {
        throw std::invalid_argument("Node is not an OSP property.");
    }
}

NodeId PropertyView::id() const noexcept
{
    return m_Id;
}

const Node& PropertyView::node() const
{
    return m_Source->node(m_Id);
}

std::optional<std::string_view> PropertyView::name() const
{
    return node().name.transform([](const std::string& value) { return std::string_view(value); });
}

PropertyType PropertyView::type() const
{
    return node().type;
}

std::string_view PropertyView::content() const
{
    return node().content;
}

std::string_view PropertyView::className() const
{
    return node().className;
}

bool PropertyView::isNull() const
{
    return node().type == PropertyType::Object && node().children.empty();
}

std::optional<ObjectView> PropertyView::object() const
{
    if (node().type != PropertyType::Object || node().children.empty())
    {
        return std::nullopt;
    }
    return ObjectView(*m_Source, node().children.front());
}

std::vector<PropertyView> PropertyView::elements() const
{
    std::vector<PropertyView> result;
    if (node().type == PropertyType::Array || node().type == PropertyType::Collection)
    {
        for (const auto child : node().children)
        {
            result.emplace_back(*m_Source, child);
        }
    }
    return result;
}

Expected<std::int32_t, DecodeError> PropertyView::intValue() const
{
    if (type() != PropertyType::Int)
    {
        return makeUnexpected(DecodeError::WrongType);
    }
    return parseJavaInt(content());
}

Expected<double, DecodeError> PropertyView::doubleValue() const
{
    if (type() != PropertyType::Double)
    {
        return makeUnexpected(DecodeError::WrongType);
    }
    return parseJavaDouble(content());
}

Expected<bool, DecodeError> PropertyView::booleanValue() const
{
    if (type() != PropertyType::Boolean)
    {
        return makeUnexpected(DecodeError::WrongType);
    }
    return content() == "true";
}

Expected<std::string, DecodeError> PropertyView::stringValue() const
{
    if (type() != PropertyType::String)
    {
        return makeUnexpected(DecodeError::WrongType);
    }
    return removeCdata(content());
}

Expected<CompactArray, DecodeError> PropertyView::compactArray() const
{
    if (type() != PropertyType::Array)
    {
        return makeUnexpected(DecodeError::WrongType);
    }
    const auto& children = node().children;
    if (children.empty())
    {
        return makeUnexpected(DecodeError::Missing);
    }
    const auto& first = m_Source->node(children.front());
    if (first.name != "array" || !isTextType(first.type))
    {
        return makeUnexpected(DecodeError::WrongType);
    }
    return parseCompactArray(first.content, className());
}

Expected<std::int32_t, DecodeError> PropertyView::elementIndex() const
{
    // Integer.parseInt(name.substring(1, name.indexOf("]")))
    const auto& value = node().name;
    if (!value)
    {
        return makeUnexpected(DecodeError::Missing);
    }
    const auto close = value->find(']');
    if (close == std::string::npos || close < 1)
    {
        return makeUnexpected(DecodeError::Malformed);
    }
    return parseJavaInt(std::string_view(*value).substr(1, close - 1));
}

Expected<std::vector<std::optional<PropertyView>>, DecodeError> PropertyView::indexedElements() const
{
    if (type() != PropertyType::Array)
    {
        return makeUnexpected(DecodeError::WrongType);
    }
    const auto items = elements();
    std::vector<std::optional<PropertyView>> slots;
    if (items.empty())
    {
        return slots;
    }
    const auto last = items.back().elementIndex();
    if (!last)
    {
        return makeUnexpected(last.error());
    }
    if (*last < 0)
    {
        return makeUnexpected(DecodeError::OutOfRange);
    }
    slots.resize(static_cast<std::size_t>(*last) + 1);
    for (const auto& item : items)
    {
        const auto position = item.elementIndex();
        if (!position)
        {
            return makeUnexpected(position.error());
        }
        if (*position < 0 || static_cast<std::size_t>(*position) >= slots.size())
        {
            return makeUnexpected(DecodeError::OutOfRange);
        }
        slots[static_cast<std::size_t>(*position)] = item;
    }
    return slots;
}

ObjectView::ObjectView(const Source& source, NodeId id) : m_Source(&source), m_Id(id)
{
    if (source.node(id).kind != NodeKind::Object)
    {
        throw std::invalid_argument("Node is not an OSP object.");
    }
}

NodeId ObjectView::id() const noexcept
{
    return m_Id;
}

const Node& ObjectView::node() const
{
    return m_Source->node(m_Id);
}

std::string_view ObjectView::className() const
{
    return node().className;
}

std::vector<PropertyView> ObjectView::properties() const
{
    std::vector<PropertyView> result;
    for (const auto child : node().children)
    {
        result.emplace_back(*m_Source, child);
    }
    return result;
}

std::optional<PropertyView> ObjectView::property(std::string_view name) const
{
    const auto& children = node().children;
    for (auto child = children.rbegin(); child != children.rend(); ++child)
    {
        if (m_Source->node(*child).name == name)
        {
            return PropertyView(*m_Source, *child);
        }
    }
    return std::nullopt;
}

std::optional<ObjectView> ObjectView::child(std::string_view name) const
{
    const auto& children = node().children;
    for (auto child = children.rbegin(); child != children.rend(); ++child)
    {
        const auto& item = m_Source->node(*child);
        if (item.name == name && item.type == PropertyType::Object && !item.children.empty())
        {
            return ObjectView(*m_Source, item.children.front());
        }
    }
    return std::nullopt;
}

Expected<std::int32_t, DecodeError> ObjectView::getInt(std::string_view name) const
{
    const auto item = property(name);
    if (!item)
    {
        return makeUnexpected(DecodeError::Missing);
    }
    switch (item->type())
    {
    case PropertyType::Int:
    case PropertyType::String:
        return parseJavaInt(item->content());
    case PropertyType::Object:
        return makeUnexpected(DecodeError::Unsupported);
    default:
        return makeUnexpected(DecodeError::WrongType);
    }
}

Expected<double, DecodeError> ObjectView::getDouble(std::string_view name) const
{
    const auto item = property(name);
    if (!item)
    {
        return makeUnexpected(DecodeError::Missing);
    }
    switch (item->type())
    {
    case PropertyType::Double:
    case PropertyType::Int:
    case PropertyType::String:
        return parseJavaDouble(item->content());
    default:
        return makeUnexpected(DecodeError::WrongType);
    }
}

Expected<bool, DecodeError> ObjectView::getBoolean(std::string_view name) const
{
    const auto item = property(name);
    if (!item)
    {
        return makeUnexpected(DecodeError::Missing);
    }
    if (item->type() != PropertyType::Boolean && item->type() != PropertyType::String)
    {
        return makeUnexpected(DecodeError::WrongType);
    }
    return item->content() == "true";
}

Expected<std::string, DecodeError> ObjectView::getString(std::string_view name) const
{
    const auto item = property(name);
    if (!item)
    {
        return makeUnexpected(DecodeError::Missing);
    }
    return item->stringValue();
}

} // namespace motrack::osp
