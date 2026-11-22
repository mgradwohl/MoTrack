#include "osp/render.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <utility>

namespace motrack::osp
{
namespace
{

constexpr std::string_view CDATA_PRE = "<![CDATA[";
constexpr std::string_view CDATA_POST = "]]>";
constexpr std::string_view INDENT = "    "; // XML.INDENT

Unexpected<RenderError> failure(RenderError::Code code, std::string message)
{
    return makeUnexpected(RenderError{.code = code, .message = std::move(message)});
}

/// XML.requiresCDATA (line 141).
bool requiresCdata(std::string_view text) noexcept
{
    return text.find_first_of("\"<>&'") != std::string_view::npos;
}

/// OSP writes attribute values without escaping and finds them again with indexOf, so a quote, an angle
/// bracket or a line break would change how the line is read.
bool safeAttribute(std::string_view value) noexcept
{
    return value.find_first_of("\"<>\r\n") == std::string_view::npos;
}

std::string_view leadingWhitespace(std::string_view line) noexcept
{
    return line.substr(0, std::min(line.find_first_not_of(" \t"), line.size()));
}

std::string withLineEnding(std::string_view content, std::string_view newline)
{
    std::string output;
    output.reserve(content.size());
    for (const auto character : content)
    {
        if (character == '\n')
        {
            output += newline;
        }
        else
        {
            output.push_back(character);
        }
    }
    return output;
}

/// Canonical form of new content: only the fields OSP's reader can reproduce, with logical line breaks.
PropertyValue canonical(const PropertyValue& value);

ObjectValue canonical(const ObjectValue& value)
{
    ObjectValue result{.className = value.className, .version = value.version};
    for (const auto& property : value.properties)
    {
        result.properties.push_back(canonical(property));
    }
    return result;
}

PropertyValue canonical(const PropertyValue& value)
{
    PropertyValue result{.name = value.name, .typeName = value.typeName};
    switch (value.type())
    {
    case PropertyType::Array:
    case PropertyType::Collection:
        result.className = value.className;
        for (const auto& element : value.elements)
        {
            result.elements.push_back(canonical(element));
        }
        break;
    case PropertyType::Object:
        if (value.object)
        {
            result.object = std::make_shared<const ObjectValue>(canonical(*value.object));
        }
        break;
    default:
        result.content = normalizeLineBreaks(value.content);
        break;
    }
    return result;
}

/// Text written for a text property's value. String content uses CDATA when XML.requiresCDATA says so.
Expected<std::string, RenderError> valueText(PropertyType type, std::string_view content, std::string_view newline)
{
    if (type == PropertyType::String)
    {
        if (content.find(CDATA_PRE) != std::string_view::npos)
        {
            // XML.removeCDATA fails on a nested marker and "]]></property>" ends the value early: OSP has no escape
            // for either, so such strings are refused rather than written unreadably (#194).
            return failure(RenderError::Code::InvalidValue, "A string value cannot contain \"<![CDATA[\".");
        }
        if (requiresCdata(content))
        {
            if (content.find("]]></property>") != std::string_view::npos)
            {
                return failure(RenderError::Code::InvalidValue, "A string value cannot contain \"]]></property>\".");
            }
            return std::string(CDATA_PRE) + withLineEnding(content, newline) + std::string(CDATA_POST);
        }
    }
    else if (content.find("</property>") != std::string_view::npos)
    {
        return failure(RenderError::Code::InvalidValue, "A property value cannot contain \"</property>\".");
    }
    return withLineEnding(content, newline);
}

class Writer
{
  public:
    explicit Writer(std::string_view newline) : m_Newline(newline)
    {}

    /// Appends a property as XMLPropertyElement.toString lays it out; each line ends with the newline.
    /// `inObject` is false for array and collection elements.
    Expected<void, RenderError> property(std::string& out, const PropertyValue& value, std::string_view indent, bool inObject)
    {
        if ((value.name && !safeAttribute(*value.name)) || !safeAttribute(value.typeName))
        {
            return failure(RenderError::Code::InvalidValue,
                           "Property names and types cannot contain quotes, angle brackets or line breaks.");
        }
        const auto type = value.type();
        out += indent;
        out += "<property";
        if (value.name)
        {
            out += " name=\"" + *value.name + "\"";
        }
        out += " type=\"" + value.typeName + "\"";
        switch (type)
        {
        case PropertyType::Array:
        case PropertyType::Collection:
        {
            if (!safeAttribute(value.className))
            {
                return failure(RenderError::Code::InvalidValue, "Class names cannot contain quotes, angle brackets or line breaks.");
            }
            out += " class=\"" + value.className + "\"";
            if (value.elements.empty())
            {
                out += "/>";
                out += m_Newline;
                return {};
            }
            out += '>';
            out += m_Newline;
            const auto inner = std::string(indent) + std::string(INDENT);
            for (const auto& element : value.elements)
            {
                if (auto result = property(out, element, inner, false); !result)
                {
                    return result;
                }
            }
            out += indent;
            out += "</property>";
            out += m_Newline;
            return {};
        }
        case PropertyType::Object:
            if (!value.object)
            {
                if (inObject)
                {
                    return failure(RenderError::Code::InvalidValue, "OSP cannot read a null object property directly in an object.");
                }
                out += ">null</property>";
                out += m_Newline;
                return {};
            }
            out += '>';
            out += m_Newline;
            if (auto result = object(out, *value.object, indent, false); !result)
            {
                return result;
            }
            out += indent;
            out += "</property>";
            out += m_Newline;
            return {};
        default:
        {
            auto text = valueText(type, value.content, m_Newline);
            if (!text)
            {
                return makeUnexpected(std::move(text.error()));
            }
            out += ">" + *text + "</property>";
            out += m_Newline;
            return {};
        }
        }
    }

    /// Appends an object as XMLControlElement.toString lays it out (the object shares its property's indent).
    Expected<void, RenderError> object(std::string& out, const ObjectValue& value, std::string_view indent, bool root)
    {
        if (!safeAttribute(value.className) || (value.version && !safeAttribute(*value.version)))
        {
            return failure(RenderError::Code::InvalidValue, "Class names cannot contain quotes, angle brackets or line breaks.");
        }
        if (value.version && !root)
        {
            return failure(RenderError::Code::InvalidValue, "Only the root object has a version.");
        }
        out += indent;
        out += "<object class=\"" + value.className + "\"";
        if (value.version)
        {
            out += " version=\"" + *value.version + "\"";
        }
        if (value.properties.empty())
        {
            out += "/>";
            out += m_Newline;
            return {};
        }
        out += '>';
        out += m_Newline;
        const auto inner = std::string(indent) + std::string(INDENT);
        for (const auto& item : value.properties)
        {
            if (auto result = property(out, item, inner, true); !result)
            {
                return result;
            }
        }
        out += indent;
        out += "</object>";
        out += m_Newline;
        return {};
    }

  private:
    std::string_view m_Newline;
};

/// Path of the first difference between two logical trees, for diagnostics.
std::string difference(const ObjectValue& expected, const ObjectValue& actual, const std::string& path);

std::string difference(const PropertyValue& expected, const PropertyValue& actual, const std::string& path)
{
    if (expected.name != actual.name || expected.typeName != actual.typeName || expected.className != actual.className)
    {
        return path + ": name, type or class";
    }
    if (expected.content != actual.content)
    {
        return path + ": content";
    }
    if (expected.elements.size() != actual.elements.size())
    {
        return path + ": element count";
    }
    for (std::size_t position = 0; position < expected.elements.size(); ++position)
    {
        if (expected.elements[position] != actual.elements[position])
        {
            return difference(expected.elements[position], actual.elements[position], path + "[" + std::to_string(position) + "]");
        }
    }
    if (static_cast<bool>(expected.object) != static_cast<bool>(actual.object))
    {
        return path + ": null object";
    }
    return expected.object ? difference(*expected.object, *actual.object, path) : path;
}

std::string difference(const ObjectValue& expected, const ObjectValue& actual, const std::string& path)
{
    if (expected.className != actual.className || expected.version != actual.version)
    {
        return path + ": class or version";
    }
    if (expected.properties.size() != actual.properties.size())
    {
        return path + ": property count";
    }
    for (std::size_t position = 0; position < expected.properties.size(); ++position)
    {
        if (expected.properties[position] != actual.properties[position])
        {
            return difference(expected.properties[position],
                              actual.properties[position],
                              path + "/" + expected.properties[position].name.value_or("(unnamed)"));
        }
    }
    return path;
}

Expected<void, RenderError> verify(const std::string& output, const ObjectValue& intended, const ParseLimits& limits)
{
    if (output.size() > limits.maxBytes)
    {
        return failure(RenderError::Code::TooLarge, "The rendered document exceeds the XML size limit.");
    }
    const auto reparsed = parse(output, limits);
    if (!reparsed)
    {
        return failure(RenderError::Code::VerificationFailed, "The rendered document does not parse: " + reparsed.error().message);
    }
    const auto actual = objectValue(**reparsed, (*reparsed)->root());
    if (actual != intended)
    {
        return failure(RenderError::Code::VerificationFailed,
                       "The rendered document reads differently at " + difference(intended, actual, "root"));
    }
    return {};
}

} // namespace

/// Turns a validated patch into byte splices of the source and checks the result.
class Renderer
{
  public:
    Renderer(const Source& source, const Patch& patch, const ParseLimits& limits)
        : m_Source(source), m_Patch(patch), m_Limits(limits), m_Newline(lineEndingText(source.lineEnding()))
    {}

    Expected<std::string, RenderError> run()
    {
        if (auto valid = collect(); !valid)
        {
            return makeUnexpected(std::move(valid.error()));
        }
        if (auto spliced = splices(); !spliced)
        {
            return makeUnexpected(std::move(spliced.error()));
        }
        std::ranges::sort(m_Splices,
                          [](const Splice& left, const Splice& right)
                          { return std::tie(left.begin, left.end, left.order) < std::tie(right.begin, right.end, right.order); });
        const auto bytes = m_Source.bytes();
        std::string output;
        std::size_t position = 0;
        for (const auto& splice : m_Splices)
        {
            if (splice.begin < position)
            {
                return failure(RenderError::Code::ConflictingEdits, "Edits overlap in the source.");
            }
            output.append(bytes.substr(position, splice.begin - position));
            output += splice.text;
            position = splice.end;
        }
        output.append(bytes.substr(position));
        if (auto checked = verify(output, intendedObject(m_Source.root()), m_Limits); !checked)
        {
            return makeUnexpected(std::move(checked.error()));
        }
        return output;
    }

  private:
    struct Splice
    {
        std::size_t begin = 0;
        std::size_t end = 0;
        std::string text{};
        std::size_t order = 0;
    };

    struct PendingInsert
    {
        std::size_t index = 0;
        PropertyValue value{};
    };

    [[nodiscard]] const Node& node(NodeId id) const
    {
        return m_Source.node(id);
    }

    [[nodiscard]] std::size_t lineBegin(std::size_t line) const
    {
        return m_Source.lines()[line].begin;
    }

    Expected<void, RenderError> claim(NodeId id)
    {
        if (!m_Claimed.insert(id).second)
        {
            return failure(RenderError::Code::ConflictingEdits, "Two edits target the same property.");
        }
        return {};
    }

    [[nodiscard]] Expected<void, RenderError> requireProperty(NodeId id) const
    {
        if (!m_Source.contains(id) || node(id).kind != NodeKind::Property)
        {
            return failure(RenderError::Code::InvalidNode, "The edit does not name a property of this source.");
        }
        return {};
    }

    Expected<void, RenderError> collect()
    {
        for (const auto& operation : m_Patch.m_Operations)
        {
            Expected<void, RenderError> result;
            if (const auto* set = std::get_if<Patch::SetContent>(&operation))
            {
                result = collectSet(*set);
            }
            else if (const auto* insert = std::get_if<Patch::Insert>(&operation))
            {
                result = collectInsert(*insert);
            }
            else if (const auto* remove = std::get_if<Patch::Remove>(&operation))
            {
                result = requireProperty(remove->property);
                if (result)
                {
                    result = claim(remove->property);
                }
                if (result)
                {
                    m_Removed.insert(remove->property);
                }
            }
            else
            {
                const auto& replace = std::get<Patch::Replace>(operation);
                result = requireProperty(replace.property);
                if (result)
                {
                    result = claim(replace.property);
                }
                if (result)
                {
                    m_Replaced.emplace(replace.property, canonical(replace.value));
                }
            }
            if (!result)
            {
                return result;
            }
        }
        // No edit may target a removed or replaced subtree.
        for (const auto id : m_Claimed)
        {
            if (inRemovedSubtree(node(id).parent))
            {
                return failure(RenderError::Code::ConflictingEdits, "An edit targets a removed or replaced subtree.");
            }
        }
        for (const auto& [parent, pending] : m_Inserts)
        {
            if (inRemovedSubtree(parent))
            {
                return failure(RenderError::Code::ConflictingEdits, "An insertion targets a removed or replaced subtree.");
            }
        }
        return {};
    }

    Expected<void, RenderError> collectSet(const Patch::SetContent& set)
    {
        if (auto valid = requireProperty(set.property); !valid)
        {
            return valid;
        }
        const auto type = node(set.property).type;
        if (!isTextType(type))
        {
            return failure(RenderError::Code::TypeMismatch, "Only text properties have content.");
        }
        const bool matches = set.expect == Patch::Expect::Text || (set.expect == Patch::Expect::Int && type == PropertyType::Int) ||
                             (set.expect == Patch::Expect::Double && type == PropertyType::Double) ||
                             (set.expect == Patch::Expect::Boolean && type == PropertyType::Boolean) ||
                             (set.expect == Patch::Expect::String && type == PropertyType::String);
        if (!matches)
        {
            return failure(RenderError::Code::TypeMismatch, "The typed setter does not match the property's OSP type.");
        }
        if (auto claimed = claim(set.property); !claimed)
        {
            return claimed;
        }
        m_Contents.emplace(set.property, normalizeLineBreaks(set.content));
        return {};
    }

    Expected<void, RenderError> collectInsert(const Patch::Insert& insert)
    {
        if (!m_Source.contains(insert.parent))
        {
            return failure(RenderError::Code::InvalidNode, "The insertion parent is not part of this source.");
        }
        const auto& parent = node(insert.parent);
        if (parent.kind == NodeKind::Property && parent.type != PropertyType::Array && parent.type != PropertyType::Collection)
        {
            return failure(RenderError::Code::InvalidNode, "Properties can only be inserted into objects, arrays and collections.");
        }
        const auto index = insert.index == Patch::END ? parent.children.size() : insert.index;
        if (index > parent.children.size())
        {
            return failure(RenderError::Code::InvalidNode, "The insertion index is past the parent's children.");
        }
        if (index == parent.children.size() && !parent.closeLine)
        {
            return failure(RenderError::Code::UnsupportedLayout, "Cannot append to an object that is not closed before end of file.");
        }
        m_Inserts[insert.parent].push_back({.index = index, .value = canonical(insert.value)});
        return {};
    }

    [[nodiscard]] bool inRemovedSubtree(std::optional<NodeId> id) const
    {
        for (; id; id = node(*id).parent)
        {
            if (m_Removed.contains(*id) || m_Replaced.contains(*id))
            {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool childOfObject(NodeId property) const
    {
        const auto parent = node(property).parent;
        return parent && node(*parent).kind == NodeKind::Object;
    }

    Expected<std::string, RenderError> generate(const PropertyValue& value, std::string_view indent, bool inObject)
    {
        std::string text;
        Writer writer(m_Newline);
        if (auto result = writer.property(text, value, indent, inObject); !result)
        {
            return makeUnexpected(std::move(result.error()));
        }
        return text;
    }

    Expected<void, RenderError> splices()
    {
        for (const auto& [id, content] : m_Contents)
        {
            const auto& item = node(id);
            auto text = valueText(item.type, content, m_Newline);
            if (!text)
            {
                return makeUnexpected(std::move(text.error()));
            }
            m_Splices.push_back({.begin = item.valueBegin, .end = item.valueEnd, .text = std::move(*text), .order = m_Splices.size()});
        }
        for (const auto id : m_Removed)
        {
            const auto& item = node(id);
            m_Splices.push_back(
                {.begin = lineBegin(item.firstLine), .end = m_Source.lines()[item.lastLine].next, .order = m_Splices.size()});
        }
        for (const auto& [id, value] : m_Replaced)
        {
            const auto& item = node(id);
            auto text = generate(value, leadingWhitespace(m_Source.lineText(item.firstLine)), childOfObject(id));
            if (!text)
            {
                return makeUnexpected(std::move(text.error()));
            }
            const auto& last = m_Source.lines()[item.lastLine];
            if (last.next == last.end)
            {
                text->resize(text->size() - m_Newline.size());
            }
            m_Splices.push_back(
                {.begin = lineBegin(item.firstLine), .end = last.next, .text = std::move(*text), .order = m_Splices.size()});
        }
        for (const auto& [parent, pending] : m_Inserts)
        {
            if (auto result = insertSplices(parent, pending); !result)
            {
                return result;
            }
        }
        return {};
    }

    Expected<void, RenderError> insertSplices(NodeId parentId, const std::vector<PendingInsert>& pending)
    {
        const auto& parent = node(parentId);
        const bool inObject = parent.kind == NodeKind::Object;
        const auto& children = parent.children;
        const auto parentIndent = std::string(leadingWhitespace(m_Source.lineText(parent.firstLine))) + std::string(INDENT);
        const auto indentFor = [&](std::size_t index) -> std::string_view
        {
            if (children.empty())
            {
                return parentIndent;
            }
            return leadingWhitespace(m_Source.lineText(node(children[std::min(index, children.size() - 1)]).firstLine));
        };
        if (parent.selfClosing)
        {
            // Reopen "<... />" by replacing the first "/>" (the one the reader found) and add a closing line.
            // The swallowed line that followed a self-closing object now follows its "</object>".
            const auto opening = m_Source.lineText(parent.firstLine);
            const auto slash = opening.find("/>");
            std::string text(opening.substr(0, slash));
            text += '>';
            text += opening.substr(slash + 2);
            text += m_Newline;
            for (const auto& insert : pending)
            {
                auto generated = generate(insert.value, parentIndent, inObject);
                if (!generated)
                {
                    return makeUnexpected(std::move(generated.error()));
                }
                text += *generated;
            }
            text += leadingWhitespace(opening);
            text += inObject ? "</object>" : "</property>";
            const auto& line = m_Source.lines()[parent.firstLine];
            m_Splices.push_back({.begin = line.begin, .end = line.end, .text = std::move(text), .order = m_Splices.size()});
            return {};
        }
        for (const auto& insert : pending)
        {
            auto generated = generate(insert.value, indentFor(insert.index), inObject);
            if (!generated)
            {
                return makeUnexpected(std::move(generated.error()));
            }
            std::size_t position = 0;
            if (insert.index < children.size())
            {
                position = lineBegin(node(children[insert.index]).firstLine);
            }
            else if (parent.closeLine)
            {
                position = lineBegin(*parent.closeLine);
            }
            else
            {
                return failure(RenderError::Code::UnsupportedLayout, "Cannot append to an object that is not closed before end of file.");
            }
            m_Splices.push_back({.begin = position, .end = position, .text = std::move(*generated), .order = m_Splices.size()});
        }
        return {};
    }

    std::vector<PropertyValue> intendedChildren(NodeId parentId)
    {
        const auto& children = node(parentId).children;
        const auto inserts = m_Inserts.find(parentId);
        std::vector<PropertyValue> result{};
        for (std::size_t position = 0; position <= children.size(); ++position)
        {
            if (inserts != m_Inserts.end())
            {
                for (const auto& insert : inserts->second)
                {
                    if (insert.index == position)
                    {
                        result.push_back(insert.value);
                    }
                }
            }
            if (position == children.size())
            {
                break;
            }
            const auto child = children[position];
            if (m_Removed.contains(child))
            {
                continue;
            }
            if (const auto replaced = m_Replaced.find(child); replaced != m_Replaced.end())
            {
                result.push_back(replaced->second);
                continue;
            }
            result.push_back(intendedProperty(child));
        }
        return result;
    }

    PropertyValue intendedProperty(NodeId id)
    {
        const auto& item = node(id);
        PropertyValue value{.name = item.name, .typeName = item.typeName};
        switch (item.type)
        {
        case PropertyType::Array:
        case PropertyType::Collection:
            value.className = item.rawClassName;
            value.elements = intendedChildren(id);
            break;
        case PropertyType::Object:
            if (!item.children.empty())
            {
                value.object = std::make_shared<const ObjectValue>(intendedObject(item.children.front()));
            }
            break;
        default:
            if (const auto content = m_Contents.find(id); content != m_Contents.end())
            {
                value.content = content->second;
            }
            else
            {
                value.content = item.content;
            }
            break;
        }
        return value;
    }

    ObjectValue intendedObject(NodeId id)
    {
        const auto& item = node(id);
        return {.className = item.rawClassName, .version = item.version, .properties = intendedChildren(id)};
    }

    const Source& m_Source;
    const Patch& m_Patch;
    const ParseLimits& m_Limits;
    std::string_view m_Newline;
    std::set<NodeId> m_Claimed;
    std::set<NodeId> m_Removed;
    std::map<NodeId, PropertyValue> m_Replaced;
    std::map<NodeId, std::string> m_Contents;
    std::map<NodeId, std::vector<PendingInsert>> m_Inserts;
    std::vector<Splice> m_Splices;
};

Patch& Patch::setContent(NodeId property, std::string content)
{
    m_Operations.emplace_back(SetContent{.property = property, .content = std::move(content), .expect = Expect::Text});
    return *this;
}

Patch& Patch::setString(NodeId property, std::string value)
{
    m_Operations.emplace_back(SetContent{.property = property, .content = std::move(value), .expect = Expect::String});
    return *this;
}

Patch& Patch::setInt(NodeId property, std::int32_t value)
{
    m_Operations.emplace_back(SetContent{.property = property, .content = formatJavaInt(value), .expect = Expect::Int});
    return *this;
}

Patch& Patch::setDouble(NodeId property, double value)
{
    m_Operations.emplace_back(SetContent{.property = property, .content = formatJavaDouble(value), .expect = Expect::Double});
    return *this;
}

Patch& Patch::setBoolean(NodeId property, bool value)
{
    m_Operations.emplace_back(SetContent{.property = property, .content = value ? "true" : "false", .expect = Expect::Boolean});
    return *this;
}

Patch& Patch::insert(NodeId parent, std::size_t index, PropertyValue value)
{
    m_Operations.emplace_back(Insert{.parent = parent, .index = index, .value = std::move(value)});
    return *this;
}

Patch& Patch::append(NodeId parent, PropertyValue value)
{
    return insert(parent, END, std::move(value));
}

Patch& Patch::remove(NodeId property)
{
    m_Operations.emplace_back(Remove{.property = property});
    return *this;
}

Patch& Patch::replace(NodeId property, PropertyValue value)
{
    m_Operations.emplace_back(Replace{.property = property, .value = std::move(value)});
    return *this;
}

bool Patch::empty() const noexcept
{
    return m_Operations.empty();
}

Expected<std::string, RenderError> render(const Source& source, const Patch& patch, const ParseLimits& limits)
{
    Renderer renderer(source, patch, limits);
    return renderer.run();
}

Expected<std::string, RenderError> write(const ObjectValue& root, const WriteOptions& options)
{
    const auto newline = lineEndingText(options.lineEnding);
    const auto intended = canonical(root);
    std::string output = R"(<?xml version="1.0" encoding="UTF-8"?>)";
    output += newline;
    Writer writer(newline);
    if (auto result = writer.object(output, intended, "", true); !result)
    {
        return makeUnexpected(std::move(result.error()));
    }
    // XMLControlElement.toString ends with "</object>" and no line break.
    output.resize(output.size() - newline.size());
    if (auto checked = verify(output, intended, options.limits); !checked)
    {
        return makeUnexpected(std::move(checked.error()));
    }
    return output;
}

} // namespace motrack::osp
