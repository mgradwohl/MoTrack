#include "source_edits.hpp"

#include "osp/java_text.hpp"

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
    return osp::PropertyView(*m_Source, property).stringValue().value_or(m_Source->node(property).content);
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
    auto parsed = osp::parse(*output, limits);
    if (!parsed)
    {
        return failure("The rendered Tracker XML does not parse: " + parsed.error().message);
    }
    if (auto checked = verify(**parsed); !checked)
    {
        return failure("O8: " + checked.error());
    }
    return Rendered{.xml = std::move(*output), .source = std::move(*parsed)};
}

osp::Expected<void, std::string> SourceEdits::verify(const osp::Source& output) const
{
    const auto& before = m_Source->node(m_Source->root());
    const auto& after = output.node(output.root());
    if (before.rawClassName != after.rawClassName || before.version != after.version)
    {
        return failure("the root object changed");
    }
    return verifyChildren(output, m_Source->root(), output.root(), "");
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
        return {};
    }
    switch (original.type)
    {
    case osp::PropertyType::Array:
    case osp::PropertyType::Collection:
        return verifyChildren(output, before, after, location);
    case osp::PropertyType::Object:
        if (original.children.empty() || rendered.children.empty())
        {
            return failure(location + ": an edited object property became null");
        }
        if (m_Source->node(original.children.front()).rawClassName != output.node(rendered.children.front()).rawClassName)
        {
            return failure(location + ": an edited object changed its class");
        }
        return verifyChildren(output, original.children.front(), rendered.children.front(), location);
    default:
        if (original.content != rendered.content)
        {
            return failure(location + ": text changed without an edit");
        }
        return {};
    }
}

} // namespace motrack
