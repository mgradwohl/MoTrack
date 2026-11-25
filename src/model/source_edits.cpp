#include "model/source_edits.hpp"

#include "osp/java_text.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <utility>

namespace motrack
{
namespace
{

osp::PropertyValue canonical(const osp::PropertyValue& value);

osp::ObjectValue canonical(const osp::ObjectValue& value)
{
    osp::ObjectValue result{.className = value.className, .version = value.version, .properties = {}};
    for (const auto& property : value.properties)
    {
        result.properties.push_back(canonical(property));
    }
    return result;
}

/// The logical tree OSP reads back for new content (as osp::render compares it).
osp::PropertyValue canonical(const osp::PropertyValue& value)
{
    osp::PropertyValue result{.name = value.name, .typeName = value.typeName};
    switch (value.type())
    {
    case osp::PropertyType::Array:
    case osp::PropertyType::Collection:
        result.className = value.className;
        for (const auto& element : value.elements)
        {
            result.elements.push_back(canonical(element));
        }
        break;
    case osp::PropertyType::Object:
        if (value.object)
        {
            result.object = std::make_shared<const osp::ObjectValue>(canonical(*value.object));
        }
        break;
    default:
        result.content = osp::normalizeLineBreaks(value.content);
        break;
    }
    return result;
}

osp::Unexpected<std::string> failure(std::string message)
{
    return osp::makeUnexpected(std::move(message));
}

} // namespace

SourceEdits::SourceEdits(std::shared_ptr<const osp::Source> source) : m_Source(std::move(source))
{}

const osp::Source& SourceEdits::source() const noexcept
{
    return *m_Source;
}

const std::shared_ptr<const osp::Source>& SourceEdits::sourcePointer() const noexcept
{
    return m_Source;
}

namespace
{

void touch(const osp::Source& source, std::set<osp::NodeId>& touched, osp::NodeId node)
{
    for (std::optional<osp::NodeId> current = node; current; current = source.node(*current).parent)
    {
        touched.insert(*current);
    }
}

} // namespace

void SourceEdits::setString(osp::NodeId property, std::string value)
{
    m_Values.insert_or_assign(property, Value{std::move(value)});
    touch(*m_Source, m_Touched, property);
}

void SourceEdits::setInt(osp::NodeId property, std::int32_t value)
{
    m_Values.insert_or_assign(property, Value{value});
    touch(*m_Source, m_Touched, property);
}

void SourceEdits::setDouble(osp::NodeId property, double value)
{
    m_Values.insert_or_assign(property, Value{value});
    touch(*m_Source, m_Touched, property);
}

void SourceEdits::setBoolean(osp::NodeId property, bool value)
{
    m_Values.insert_or_assign(property, Value{value});
    touch(*m_Source, m_Touched, property);
}

void SourceEdits::remove(osp::NodeId property)
{
    m_Removed.insert(property);
    touch(*m_Source, m_Touched, property);
}

void SourceEdits::insert(osp::NodeId parent, std::size_t index, osp::PropertyValue value)
{
    m_Inserts[parent].push_back({.index = index, .value = std::move(value)});
    touch(*m_Source, m_Touched, parent);
}

void SourceEdits::append(osp::NodeId parent, osp::PropertyValue value)
{
    insert(parent, m_Source->node(parent).children.size(), std::move(value));
}

void SourceEdits::rename(osp::NodeId property, std::string name)
{
    m_Renames.insert_or_assign(property, std::move(name));
    touch(*m_Source, m_Touched, property);
}

bool SourceEdits::removed(osp::NodeId node) const
{
    for (std::optional<osp::NodeId> current = node; current; current = m_Source->node(*current).parent)
    {
        if (m_Removed.contains(*current))
        {
            return true;
        }
    }
    return false;
}

bool SourceEdits::replacedAncestor(osp::NodeId node) const
{
    for (std::optional<osp::NodeId> current = node; current; current = m_Source->node(*current).parent)
    {
        if (m_Renames.contains(*current))
        {
            return true;
        }
    }
    return false;
}

bool SourceEdits::edited(osp::NodeId node) const
{
    return m_Touched.contains(node);
}

namespace
{

std::string valueText(const std::variant<std::string, std::int32_t, double, bool>& value)
{
    if (const auto* text = std::get_if<std::string>(&value))
    {
        return osp::normalizeLineBreaks(*text);
    }
    if (const auto* number = std::get_if<std::int32_t>(&value))
    {
        return osp::formatJavaInt(*number);
    }
    if (const auto* number = std::get_if<double>(&value))
    {
        return osp::formatJavaDouble(*number);
    }
    return std::get<bool>(value) ? "true" : "false";
}

} // namespace

std::string SourceEdits::content(osp::NodeId property) const
{
    if (const auto found = m_Values.find(property); found != m_Values.end())
    {
        return valueText(found->second);
    }
    return m_Source->node(property).content;
}

std::string SourceEdits::text(osp::NodeId property) const
{
    if (m_Values.contains(property))
    {
        return content(property);
    }
    const auto value = osp::PropertyView(*m_Source, property).stringValue();
    return value ? *value : m_Source->node(property).content;
}

std::optional<std::string> SourceEdits::name(osp::NodeId property) const
{
    if (const auto found = m_Renames.find(property); found != m_Renames.end())
    {
        return found->second;
    }
    return m_Source->node(property).name;
}

std::vector<osp::NodeId> SourceEdits::children(osp::NodeId parent) const
{
    std::vector<osp::NodeId> result;
    for (const auto child : m_Source->node(parent).children)
    {
        if (!m_Removed.contains(child))
        {
            result.push_back(child);
        }
    }
    return result;
}

bool SourceEdits::hasInsertions(osp::NodeId parent) const
{
    return m_Inserts.contains(parent);
}

std::vector<osp::PropertyValue> SourceEdits::insertions(osp::NodeId parent) const
{
    std::vector<osp::PropertyValue> result;
    if (const auto found = m_Inserts.find(parent); found != m_Inserts.end())
    {
        for (const auto& pending : found->second)
        {
            result.push_back(pending.value);
        }
    }
    return result;
}

osp::PropertyValue SourceEdits::value(osp::NodeId property) const
{
    return intended(property);
}

bool SourceEdits::empty() const noexcept
{
    return m_Values.empty() && m_Removed.empty() && m_Inserts.empty() && m_Renames.empty();
}

std::vector<osp::PropertyValue> SourceEdits::intendedChildren(osp::NodeId parent) const
{
    const auto& children = m_Source->node(parent).children;
    const auto inserts = m_Inserts.find(parent);
    std::vector<osp::PropertyValue> result;
    for (std::size_t position = 0; position <= children.size(); ++position)
    {
        if (inserts != m_Inserts.end())
        {
            for (const auto& pending : inserts->second)
            {
                if (pending.index == position)
                {
                    result.push_back(canonical(pending.value));
                }
            }
        }
        if (position < children.size() && !m_Removed.contains(children[position]))
        {
            result.push_back(intended(children[position]));
        }
    }
    return result;
}

osp::ObjectValue SourceEdits::intendedObject(osp::NodeId object) const
{
    const auto& node = m_Source->node(object);
    return {.className = node.rawClassName, .version = node.version, .properties = intendedChildren(object)};
}

osp::PropertyValue SourceEdits::intended(osp::NodeId property) const
{
    if (!edited(property))
    {
        return osp::propertyValue(*m_Source, property);
    }
    const auto& node = m_Source->node(property);
    osp::PropertyValue value{.name = name(property), .typeName = node.typeName};
    switch (node.type)
    {
    case osp::PropertyType::Array:
    case osp::PropertyType::Collection:
        value.className = node.rawClassName;
        value.elements = intendedChildren(property);
        break;
    case osp::PropertyType::Object:
        if (!node.children.empty())
        {
            value.object = std::make_shared<const osp::ObjectValue>(intendedObject(node.children.front()));
        }
        break;
    default:
        value.content = content(property);
        break;
    }
    return value;
}

osp::Expected<SourceEdits::Rendered, std::string> SourceEdits::render(const osp::ParseLimits& limits) const
{
    const auto covered = [&](osp::NodeId node)
    {
        return removed(node) || replacedAncestor(node);
    };
    osp::Patch patch(*m_Source);
    for (const auto& [property, value] : m_Values)
    {
        if (covered(property))
        {
            continue;
        }
        if (const auto* text = std::get_if<std::string>(&value))
        {
            patch.setString(property, *text);
        }
        else if (const auto* number = std::get_if<std::int32_t>(&value))
        {
            patch.setInt(property, *number);
        }
        else if (const auto* real = std::get_if<double>(&value))
        {
            patch.setDouble(property, *real);
        }
        else
        {
            patch.setBoolean(property, std::get<bool>(value));
        }
    }
    for (const auto property : m_Removed)
    {
        const auto parent = m_Source->node(property).parent;
        if (!(parent && removed(*parent)) && !replacedAncestor(property))
        {
            patch.remove(property);
        }
    }
    for (const auto& [parent, pending] : m_Inserts)
    {
        if (covered(parent))
        {
            continue;
        }
        for (const auto& item : pending)
        {
            patch.insert(parent, item.index, item.value);
        }
    }
    for (const auto& [property, newName] : m_Renames)
    {
        const auto parent = m_Source->node(property).parent;
        if (!removed(property) && !(parent && replacedAncestor(*parent)))
        {
            patch.replace(property, intended(property));
        }
    }
    auto output = osp::render(*m_Source, patch, limits);
    if (!output)
    {
        return failure("The edited Tracker XML could not be rendered: " + output.error().message);
    }
    return verifyRendered(std::move(*output), limits);
}

osp::Expected<SourceEdits::Rendered, std::string> SourceEdits::verifyRendered(std::string xml, const osp::ParseLimits& limits) const
{
    auto parsed = osp::parse(xml, limits);
    if (!parsed)
    {
        return failure("The rendered Tracker XML does not parse: " + parsed.error().message);
    }
    if (auto checked = verify(**parsed); !checked)
    {
        return failure("O8: " + checked.error());
    }
    return Rendered{.xml = std::move(xml), .source = std::move(*parsed)};
}

namespace
{

// Byte spans of a parsed document (#316). A node's span runs from the start of its first line to the end of its last
// line; the terminator of that line belongs to the gap that follows it.

std::size_t spanBegin(const osp::Source& source, osp::NodeId id)
{
    return source.lines()[source.node(id).firstLine].begin;
}

std::size_t spanEnd(const osp::Source& source, osp::NodeId id)
{
    return source.lines()[source.node(id).lastLine].end;
}

std::string_view bytes(const osp::Source& source, std::size_t begin, std::size_t end)
{
    return source.bytes().substr(begin, end - begin);
}

/// Where the children of a container may start: after its opening line's text, which an object or array shares with
/// nothing else.
std::size_t interiorBegin(const osp::Source& source, osp::NodeId container)
{
    return source.lines()[source.node(container).firstLine].end;
}

/// Where its closing part starts: the closing line of an object or array, or the end of an unclosed object.
std::size_t closingBegin(const osp::Source& source, osp::NodeId container)
{
    const auto& node = source.node(container);
    return node.closeLine && !node.selfClosing ? source.lines()[*node.closeLine].begin : spanEnd(source, container);
}

/// The bytes between consecutive children of a container, before the first and after the last: one more than there
/// are children.
std::vector<std::string_view> gaps(const osp::Source& source, osp::NodeId container)
{
    std::vector<std::string_view> result;
    auto previous = interiorBegin(source, container);
    for (const auto child : source.node(container).children)
    {
        result.push_back(bytes(source, previous, spanBegin(source, child)));
        previous = spanEnd(source, child);
    }
    result.push_back(bytes(source, previous, closingBegin(source, container)));
    return result;
}

/// The terminator of a node's last line, which `osp::render` removes with the node.
std::string_view lastTerminator(const osp::Source& source, osp::NodeId id)
{
    const auto& line = source.lines()[source.node(id).lastLine];
    return bytes(source, line.end, line.next);
}

std::string_view leadingWhitespace(std::string_view line)
{
    return line.substr(0, std::min(line.find_first_not_of(" \t"), line.size()));
}

} // namespace

osp::Expected<void, std::string> SourceEdits::verify(const osp::Source& output) const
{
    const auto& before = m_Source->node(m_Source->root());
    const auto& after = output.node(output.root());
    if (before.rawClassName != after.rawClassName || before.version != after.version)
    {
        return failure("the root object changed");
    }
    if (m_Source->prolog() != output.prolog())
    {
        return failure("the prolog changed");
    }
    // The root's last line terminator and the epilog follow the root's span.
    if (m_Source->bytes().substr(spanEnd(*m_Source, m_Source->root())) != output.bytes().substr(spanEnd(output, output.root())))
    {
        return failure("the epilog changed");
    }
    if (!edited(m_Source->root()))
    {
        if (m_Source->rawText(m_Source->root()) != output.rawText(output.root()))
        {
            return failure("unedited content changed");
        }
        return {};
    }
    return verifyContainer(output, m_Source->root(), output.root(), "");
}

std::vector<std::string> SourceEdits::expectedGaps(osp::NodeId container) const
{
    // osp::render deletes a removed child's lines with the terminator of its last line, writes an insertion at index i
    // just before the first line of original child i (after the gap that precedes it) or, when appending, just before
    // the closing line, each line ending with the file's line break, and rewrites a renamed child in place with that
    // line break after it. Every other byte of every gap is copied.
    const auto newline = std::string(osp::lineEndingText(m_Source->lineEnding()));
    const auto& children = m_Source->node(container).children;
    const auto original = gaps(*m_Source, container);
    const auto inserts = m_Inserts.find(container);
    std::vector<std::string> result;
    // The first gap starts with the opening line's terminator, every later one with the previous child's.
    std::string pending;
    // Leading bytes of the next gap that an edit of the previous child replaced: that child's last terminator.
    std::size_t replaced = 0;
    for (std::size_t position = 0; position <= children.size(); ++position)
    {
        pending += original[position].substr(replaced);
        replaced = 0;
        if (inserts != m_Inserts.end())
        {
            for (const auto& item : inserts->second)
            {
                if (item.index == position)
                {
                    result.push_back(std::move(pending));
                    pending = newline;
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
            // The child's lines vanish with their last terminator.
            replaced = lastTerminator(*m_Source, child).size();
            continue;
        }
        result.push_back(std::move(pending));
        pending.clear();
        if (m_Renames.contains(child))
        {
            // A renamed child is rewritten with the file's line break in place of its last terminator, if it had one.
            replaced = lastTerminator(*m_Source, child).size();
            if (replaced > 0)
            {
                pending = newline;
            }
        }
    }
    result.push_back(std::move(pending));
    return result;
}

osp::Expected<void, std::string>
SourceEdits::verifyContainer(const osp::Source& output, osp::NodeId before, osp::NodeId after, const std::string& path) const
{
    const auto& original = m_Source->node(before);
    const auto& rendered = output.node(after);
    const auto location = path.empty() ? std::string("/") : path;
    const auto openingLine = m_Source->lineText(original.firstLine);
    const auto renderedOpening = output.lineText(rendered.firstLine);
    const auto renderedClosing = bytes(output, closingBegin(output, after), spanEnd(output, after));
    const auto renderedGaps = gaps(output, after);
    if (original.selfClosing)
    {
        // osp::render reopens a self-closing container that receives children by replacing the first "/>" of its
        // opening line, writes every child on lines of its own and adds a closing line before the original line break.
        const auto slash = openingLine.find("/>");
        auto reopened = std::string(openingLine.substr(0, slash));
        reopened += '>';
        reopened += openingLine.substr(slash + 2);
        auto closing = std::string(leadingWhitespace(openingLine));
        closing += original.kind == osp::NodeKind::Object ? "</object>" : "</property>";
        closing += bytes(*m_Source, interiorBegin(*m_Source, before), spanEnd(*m_Source, before));
        if (renderedOpening != reopened || renderedClosing != closing)
        {
            return failure(location + ": a reopened container's opening or closing line changed");
        }
        const auto newline = osp::lineEndingText(m_Source->lineEnding());
        if (!std::ranges::all_of(renderedGaps, [&](std::string_view gap) { return gap == newline; }))
        {
            return failure(location + ": bytes between properties changed");
        }
        return verifyChildren(output, before, after, path);
    }
    if (renderedOpening != openingLine || renderedClosing != bytes(*m_Source, closingBegin(*m_Source, before), spanEnd(*m_Source, before)))
    {
        return failure(location + ": an edited container's opening or closing line changed");
    }
    const auto expected = expectedGaps(before);
    if (!std::ranges::equal(expected, renderedGaps))
    {
        return failure(location + ": bytes between properties changed");
    }
    return verifyChildren(output, before, after, path);
}

osp::Expected<void, std::string>
SourceEdits::verifyChildren(const osp::Source& output, osp::NodeId before, osp::NodeId after, const std::string& path) const
{
    const auto& original = m_Source->node(before).children;
    const auto& rendered = output.node(after).children;
    const auto inserts = m_Inserts.find(before);
    std::size_t next = 0;
    for (std::size_t position = 0; position <= original.size(); ++position)
    {
        if (inserts != m_Inserts.end())
        {
            for (const auto& pending : inserts->second)
            {
                if (pending.index != position)
                {
                    continue;
                }
                if (next >= rendered.size() || osp::propertyValue(output, rendered[next]) != canonical(pending.value))
                {
                    return failure(path + ": an inserted property reads back differently");
                }
                ++next;
            }
        }
        if (position == original.size())
        {
            break;
        }
        if (m_Removed.contains(original[position]))
        {
            continue;
        }
        if (next >= rendered.size())
        {
            return failure(path + ": a retained property is missing");
        }
        if (auto checked = verifyProperty(output, original[position], rendered[next], path); !checked)
        {
            return checked;
        }
        ++next;
    }
    if (next != rendered.size())
    {
        return failure(path + ": the output has unexpected properties");
    }
    return {};
}

osp::Expected<void, std::string>
SourceEdits::verifyProperty(const osp::Source& output, osp::NodeId before, osp::NodeId after, const std::string& path) const
{
    const auto& original = m_Source->node(before);
    const auto& rendered = output.node(after);
    const auto location = path + "/" + original.name.value_or("(unnamed)");
    if (m_Renames.contains(before))
    {
        if (osp::propertyValue(output, after) != intended(before))
        {
            return failure(location + ": a renamed property reads back differently");
        }
        return {};
    }
    if (!edited(before))
    {
        // Unedited content keeps its bytes and its logical value.
        if (m_Source->rawText(before) != output.rawText(after) ||
            osp::propertyValue(*m_Source, before) != osp::propertyValue(output, after))
        {
            return failure(location + ": unedited content changed");
        }
        return {};
    }
    if (original.name != rendered.name || original.typeName != rendered.typeName || original.rawClassName != rendered.rawClassName)
    {
        return failure(location + ": an edited property changed its name, type or class");
    }
    if (const auto found = m_Values.find(before); found != m_Values.end())
    {
        const osp::PropertyView view(output, after);
        bool matches = false;
        if (const auto* text = std::get_if<std::string>(&found->second))
        {
            const auto value = view.stringValue();
            matches = value && *value == osp::normalizeLineBreaks(*text);
        }
        else if (const auto* number = std::get_if<std::int32_t>(&found->second))
        {
            const auto value = view.intValue();
            matches = value && *value == *number;
        }
        else if (const auto* real = std::get_if<double>(&found->second))
        {
            const auto value = view.doubleValue();
            matches = value && std::bit_cast<std::uint64_t>(*value) == std::bit_cast<std::uint64_t>(*real);
        }
        else
        {
            const auto value = view.booleanValue();
            matches = value && *value == std::get<bool>(found->second);
        }
        if (!matches)
        {
            return failure(location + ": an edited value decodes differently");
        }
        // Only the value between the opening line's first '>' and its end tag changes.
        if (bytes(*m_Source, spanBegin(*m_Source, before), original.valueBegin) !=
                bytes(output, spanBegin(output, after), rendered.valueBegin) ||
            bytes(*m_Source, original.valueEnd, spanEnd(*m_Source, before)) != bytes(output, rendered.valueEnd, spanEnd(output, after)))
        {
            return failure(location + ": bytes around an edited value changed");
        }
        return {};
    }
    switch (original.type)
    {
    case osp::PropertyType::Array:
    case osp::PropertyType::Collection:
        return verifyContainer(output, before, after, location);
    case osp::PropertyType::Object:
    {
        if (original.children.empty() || rendered.children.empty())
        {
            return failure(location + ": an edited object property became null");
        }
        const auto object = original.children.front();
        const auto renderedObject = rendered.children.front();
        if (m_Source->node(object).rawClassName != output.node(renderedObject).rawClassName)
        {
            return failure(location + ": an edited object changed its class");
        }
        // The property's own lines around its object: its opening line and whatever follows the object's last line.
        if (bytes(*m_Source, spanBegin(*m_Source, before), spanBegin(*m_Source, object)) !=
                bytes(output, spanBegin(output, after), spanBegin(output, renderedObject)) ||
            bytes(*m_Source, spanEnd(*m_Source, object), spanEnd(*m_Source, before)) !=
                bytes(output, spanEnd(output, renderedObject), spanEnd(output, after)))
        {
            return failure(location + ": an edited object property's own lines changed");
        }
        return verifyContainer(output, object, renderedObject, location);
    }
    default:
        if (original.content != rendered.content)
        {
            return failure(location + ": text changed without an edit");
        }
        return {};
    }
}

} // namespace motrack
