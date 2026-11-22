#include "osp/value.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <utility>

namespace motrack::osp
{
namespace
{

std::size_t compactLength(const CompactArray& array) noexcept
{
    switch (array.kind)
    {
    case CompactArray::Kind::Int:
        return array.ints.size();
    case CompactArray::Kind::Double:
        return array.doubles.size();
    case CompactArray::Kind::Boolean:
        return array.booleans.size();
    case CompactArray::Kind::Nested:
        return array.rows.size();
    case CompactArray::Kind::Null:
        break;
    }
    return 0;
}

/// Checks that values have the structure of `className` ("[[D" holds rows of doubles or null rows).
bool matchesClass(const CompactArray& values, std::string_view className) noexcept
{
    if (className.size() == 2)
    {
        const auto leaf = className[1];
        return (leaf == 'I' && values.kind == CompactArray::Kind::Int) || (leaf == 'D' && values.kind == CompactArray::Kind::Double) ||
               (leaf == 'Z' && values.kind == CompactArray::Kind::Boolean);
    }
    if (values.kind != CompactArray::Kind::Nested)
    {
        return false;
    }
    for (const auto& row : values.rows)
    {
        if (row.kind != CompactArray::Kind::Null && !matchesClass(row, className.substr(1)))
        {
            return false;
        }
    }
    return true;
}

bool sameDouble(double left, double right) noexcept
{
    // Double.toString writes every NaN as "NaN"; any other value must keep its exact bits, including -0.0.
    return (std::isnan(left) && std::isnan(right)) || std::bit_cast<std::uint64_t>(left) == std::bit_cast<std::uint64_t>(right);
}

bool sameValues(const CompactArray& left, const CompactArray& right) noexcept
{
    if (left.kind != right.kind || left.ints != right.ints || left.booleans != right.booleans || left.doubles.size() != right.doubles.size() ||
        left.rows.size() != right.rows.size())
    {
        return false;
    }
    for (std::size_t position = 0; position < left.doubles.size(); ++position)
    {
        if (!sameDouble(left.doubles[position], right.doubles[position]))
        {
            return false;
        }
    }
    for (std::size_t position = 0; position < left.rows.size(); ++position)
    {
        if (!sameValues(left.rows[position], right.rows[position]))
        {
            return false;
        }
    }
    return true;
}

/// OSP's arrayValue (XMLControlElement line 2002) does not read every compact string OSP writes: an empty row
/// "{}" yields one empty field ("".split(",")), so numbers throw and booleans gain a false element, and a final
/// null row is dropped because the closing brace is never scanned. Compact text is used only when that
/// decoder returns exactly the values (MoTrack #193, OpenSourcePhysics/osp#20).
bool readsBack(std::string_view text, std::string_view className, const CompactArray& values)
{
    const auto decoded = parseCompactArray(text, className);
    return decoded && sameValues(*decoded, values);
}

PropertyValue primitiveArray(std::string name, std::string_view className, const CompactArray& values, bool writeNullFinalElement)
{
    // XMLPropertyElement line 111: multiply lengths along first elements while the component is an array.
    auto count = compactLength(values);
    auto remainingLevels = className.size() - 2;
    const auto* current = &values;
    while (count > 0 && remainingLevels > 0)
    {
        --remainingLevels;
        current = &current->rows.front();
        if (current->kind == CompactArray::Kind::Null)
        {
            break;
        }
        count *= compactLength(*current);
    }
    PropertyValue property{.name = std::move(name), .typeName = "array", .className = std::string(className)};
    // Deliberate difference: where OSP would write a compact string that it reads differently, MoTrack writes
    // the element form, which OSP reads back exactly.
    if (remainingLevels == 0 && count > 0)
    {
        auto compact = formatCompactArray(values);
        if (readsBack(compact, className, values))
        {
            property.elements.push_back(stringProperty("array", std::move(compact)));
            return property;
        }
    }
    // Element form: only nested arrays have elements; an empty leaf array has none.
    const auto length = values.kind == CompactArray::Kind::Nested ? values.rows.size() : 0;
    for (std::size_t position = 0; position < length; ++position)
    {
        const auto& row = values.rows[position];
        auto elementName = "[" + std::to_string(position) + "]";
        if (row.kind != CompactArray::Kind::Null)
        {
            property.elements.push_back(primitiveArray(std::move(elementName), className.substr(1), row, writeNullFinalElement));
        }
        else if (writeNullFinalElement && position + 1 == length)
        {
            property.elements.push_back(nullObjectProperty(std::move(elementName)));
        }
    }
    return property;
}

} // namespace

PropertyType PropertyValue::type() const noexcept
{
    return propertyTypeFromName(typeName);
}

bool operator==(const PropertyValue& left, const PropertyValue& right)
{
    if (left.name != right.name || left.typeName != right.typeName || left.className != right.className || left.content != right.content ||
        left.elements != right.elements || static_cast<bool>(left.object) != static_cast<bool>(right.object))
    {
        return false;
    }
    return !left.object || *left.object == *right.object;
}

PropertyValue propertyValue(const Source& source, NodeId property)
{
    const auto& node = source.node(property);
    PropertyValue value{.name = node.name, .typeName = node.typeName};
    switch (node.type)
    {
    case PropertyType::Array:
    case PropertyType::Collection:
        value.className = node.rawClassName;
        for (const auto child : node.children)
        {
            value.elements.push_back(propertyValue(source, child));
        }
        break;
    case PropertyType::Object:
        if (!node.children.empty())
        {
            value.object = std::make_shared<const ObjectValue>(objectValue(source, node.children.front()));
        }
        break;
    default:
        value.content = node.content;
        break;
    }
    return value;
}

ObjectValue objectValue(const Source& source, NodeId object)
{
    const auto& node = source.node(object);
    ObjectValue value{.className = node.rawClassName, .version = node.version};
    for (const auto child : node.children)
    {
        value.properties.push_back(propertyValue(source, child));
    }
    return value;
}

PropertyValue intProperty(std::string name, std::int32_t value)
{
    return {.name = std::move(name), .typeName = "int", .content = formatJavaInt(value)};
}

PropertyValue doubleProperty(std::string name, double value)
{
    return {.name = std::move(name), .typeName = "double", .content = formatJavaDouble(value)};
}

PropertyValue booleanProperty(std::string name, bool value)
{
    return {.name = std::move(name), .typeName = "boolean", .content = value ? "true" : "false"};
}

PropertyValue stringProperty(std::string name, std::string_view value)
{
    return {.name = std::move(name), .typeName = "string", .content = normalizeLineBreaks(value)};
}

PropertyValue objectProperty(std::string name, ObjectValue value)
{
    return {.name = std::move(name), .typeName = "object", .object = std::make_shared<const ObjectValue>(std::move(value))};
}

PropertyValue nullObjectProperty(std::string name)
{
    return {.name = std::move(name), .typeName = "object"};
}

PropertyValue arrayProperty(std::string name,
                            std::string className,
                            const std::vector<std::optional<PropertyValue>>& elements,
                            bool writeNullFinalElement)
{
    PropertyValue property{.name = std::move(name), .typeName = "array", .className = std::move(className)};
    for (std::size_t position = 0; position < elements.size(); ++position)
    {
        auto elementName = "[" + std::to_string(position) + "]";
        if (const auto& slot = elements[position]; slot)
        {
            auto element = *slot;
            element.name = std::move(elementName);
            property.elements.push_back(std::move(element));
        }
        else if (writeNullFinalElement && position + 1 == elements.size())
        {
            property.elements.push_back(nullObjectProperty(std::move(elementName)));
        }
    }
    return property;
}

PropertyValue collectionProperty(std::string name, std::string className, std::vector<PropertyValue> items)
{
    PropertyValue property{.name = std::move(name), .typeName = "collection", .className = std::move(className)};
    for (auto& item : items)
    {
        item.name = "item";
        property.elements.push_back(std::move(item));
    }
    return property;
}

Expected<PropertyValue, DecodeError>
primitiveArrayProperty(std::string name, std::string_view className, const CompactArray& values, bool writeNullFinalElement)
{
    if (!isPrimitiveArrayClass(className) || !matchesClass(values, className))
    {
        return makeUnexpected(DecodeError::WrongType);
    }
    return primitiveArray(std::move(name), className, values, writeNullFinalElement);
}

std::string normalizeLineBreaks(std::string_view text)
{
    std::string output;
    output.reserve(text.size());
    for (std::size_t position = 0; position < text.size(); ++position)
    {
        if (text[position] == '\r')
        {
            output.push_back('\n');
            if (position + 1 < text.size() && text[position + 1] == '\n')
            {
                ++position;
            }
        }
        else
        {
            output.push_back(text[position]);
        }
    }
    return output;
}

} // namespace motrack::osp
