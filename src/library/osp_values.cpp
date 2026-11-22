#include "library/osp_values.hpp"

#include "library/java_string.hpp"
#include "osp/java_text.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <ranges>
#include <utility>

namespace motrack::library::detail
{
namespace
{

using Kind = JavaValue::Kind;
using osp::PropertyType;

constexpr std::string_view STRING_CLASS = "java.lang.String";

/// Collection classes that `Class.newInstance` can create; XMLControlElement.collectionValue returns null for
/// any other class.
constexpr std::array<std::string_view, 9> COLLECTION_CLASSES = {"java.util.ArrayList",
                                                                "java.util.LinkedList",
                                                                "java.util.HashSet",
                                                                "java.util.LinkedHashSet",
                                                                "java.util.TreeSet",
                                                                "java.util.Vector",
                                                                "java.util.Stack",
                                                                "java.util.ArrayDeque",
                                                                "java.util.PriorityQueue"};

bool isPrimitiveCode(std::string_view code) noexcept
{
    return code.size() == 1 && std::string_view("ZBCSIJFD").find(code.front()) != std::string_view::npos;
}

/// Class.forName for an array class name: '[' prefixes, then a primitive code or "L<class>;".
bool resolvableArrayClass(std::string_view className) noexcept
{
    if (!className.starts_with('['))
    {
        return false;
    }
    const auto dimensions = className.find_first_not_of('[');
    // The JVM limits arrays to 255 dimensions; Class.forName fails beyond that.
    if (dimensions == std::string_view::npos || dimensions > 255)
    {
        return false;
    }
    const auto leaf = className.substr(dimensions);
    if (isPrimitiveCode(leaf))
    {
        return true;
    }
    return leaf.size() > 2 && leaf.starts_with('L') && leaf.ends_with(';') && isKnownJavaClass(leaf.substr(1, leaf.size() - 2));
}

/// The class a "[X" array's elements must have, as the element class name ("[Ljava.lang.String;" for
/// String[][], "java.lang.String" for String[]), or the primitive code.
std::string componentClass(std::string_view arrayClass)
{
    const auto component = arrayClass.substr(1);
    if (component.starts_with('L') && component.ends_with(';'))
    {
        return std::string(component.substr(1, component.size() - 2));
    }
    return std::string(component);
}

/// The array class of a component class ("java.lang.String" -> "[Ljava.lang.String;").
std::string arrayClassOf(std::string_view component)
{
    if (component.starts_with('[') || isPrimitiveCode(component))
    {
        return "[" + std::string(component);
    }
    return "[L" + std::string(component) + ";";
}

/// Array.set(array, index, value) accepts the value for arrays MoTrack casts to. Other arrays are never cast
/// successfully, so their elements need no check: the loader fails at the cast either way.
bool assignable(std::string_view component, const JavaValue& value)
{
    if (isPrimitiveCode(component))
    {
        return true;
    }
    if (value.kind == Kind::Null)
    {
        return true;
    }
    if (component == STRING_CLASS)
    {
        return value.kind == Kind::String;
    }
    if (component == RESOURCE_CLASS)
    {
        return value.kind == Kind::Resource;
    }
    if (component == COLLECTION_CLASS)
    {
        return value.kind == Kind::Resource && value.resource.isCollection();
    }
    if (component.starts_with('['))
    {
        return value.kind == Kind::Array && value.arrayClass == component;
    }
    return true;
}

/// XMLControlElement.arrayValue(String, Class): the compact "{...}" form. Strings and objects are never written
/// compactly, so their elements stay null.
Result<JavaValue> compactValue(std::string_view text, std::string_view component, const std::string& location)
{
    if (!(text.starts_with('{') && text.ends_with('}')))
    {
        return JavaValue{};
    }
    JavaValue array{.kind = Kind::Array, .arrayClass = arrayClassOf(component)};
    if (component.starts_with('['))
    {
        const auto end = text.size() - 1;
        bool wasArray = false;
        for (std::size_t position = 1; position < end; ++position)
        {
            const char character = text[position];
            if (character == ',' || character == '}')
            {
                if (wasArray)
                {
                    wasArray = false;
                }
                else
                {
                    array.elements.emplace_back();
                }
            }
            else if (character == '{')
            {
                std::size_t close = std::string_view::npos;
                int depth = 0;
                for (std::size_t scan = position; scan < end; ++scan)
                {
                    depth += text[scan] == '{' ? 1 : text[scan] == '}' ? -1 : 0;
                    if (text[scan] == '}' && depth == 0)
                    {
                        close = scan;
                        break;
                    }
                }
                if (close == std::string_view::npos)
                {
                    return fail(location, "unbalanced braces in a compact array");
                }
                auto row = compactValue(text.substr(position, close - position + 1), componentClass(component), location);
                if (!row)
                {
                    return row;
                }
                array.elements.push_back(*std::move(row));
                position = close;
                wasArray = true;
            }
        }
        return array;
    }
    // "{}" splits into one empty field, as in Java.
    const auto fields = javaSplit(text.substr(1, text.size() - 2), ",");
    for (const auto& field : fields)
    {
        if (component == "I" && !osp::parseJavaInt(field))
        {
            return fail(location, "invalid int in a compact array");
        }
        if (component == "D" && !osp::parseJavaDouble(field))
        {
            return fail(location, "invalid double in a compact array");
        }
        array.elements.push_back(isPrimitiveCode(component) ? JavaValue{.kind = Kind::Other} : JavaValue{});
    }
    return array;
}

std::string elementLocation(const std::string& location, std::string_view name)
{
    return location + std::string(name);
}

} // namespace

bool isKnownJavaClass(std::string_view className) noexcept
{
    return className == RESOURCE_CLASS || className == COLLECTION_CLASS || className == LIBRARY_CLASS || className.starts_with("java.") ||
           className.starts_with("javax.");
}

ValueReader::ValueReader(ResourceObjectLoader loader, std::vector<Failure>* ignored) : m_Loader(std::move(loader)), m_Ignored(ignored)
{}

Result<std::optional<std::string>>
ValueReader::getString(const osp::ObjectView& object, std::string_view name, const std::string& location) const
{
    auto value = object.getString(name);
    if (value)
    {
        return std::optional<std::string>(*std::move(value));
    }
    if (value.error() == osp::DecodeError::Malformed)
    {
        return fail(location + "." + std::string(name), "malformed CDATA section");
    }
    return std::optional<std::string>{};
}

Result<JavaValue> ValueReader::getObject(const osp::ObjectView& object, std::string_view name, const std::string& location) const
{
    const auto property = object.property(name);
    if (!property)
    {
        return JavaValue{};
    }
    return propertyValue(*property, location + "." + std::string(name));
}

Result<JavaValue> ValueReader::propertyValue(const osp::PropertyView& property, const std::string& location) const
{
    switch (property.type())
    {
    case PropertyType::Object:
        return objectValue(property, location);
    case PropertyType::Array:
        return arrayValue(property, location);
    case PropertyType::Collection:
        return collectionValue(property, location);
    case PropertyType::Int:
        if (!osp::parseJavaInt(property.content()))
        {
            return fail(location, "invalid int");
        }
        return JavaValue{.kind = Kind::Other};
    case PropertyType::Double:
        if (!osp::parseJavaDouble(property.content()))
        {
            return fail(location, "invalid double");
        }
        return JavaValue{.kind = Kind::Other};
    case PropertyType::Boolean:
        return JavaValue{.kind = Kind::Other};
    case PropertyType::String:
    {
        auto text = property.stringValue();
        if (!text)
        {
            return fail(location, "malformed CDATA section");
        }
        return JavaValue{.kind = Kind::String, .text = *std::move(text)};
    }
    case PropertyType::Unknown:
        break;
    }
    return JavaValue{};
}

Result<JavaValue> ValueReader::objectValue(const osp::PropertyView& property, const std::string& location) const
{
    const auto object = property.object();
    if (!object)
    {
        return JavaValue{};
    }
    const auto className = object->className();
    if (className == RESOURCE_CLASS || className == COLLECTION_CLASS)
    {
        auto resource = m_Loader(*object, location);
        if (!resource)
        {
            return osp::makeUnexpected(resource.error());
        }
        return JavaValue{.kind = Kind::Resource, .resource = *std::move(resource)};
    }
    if (isKnownJavaClass(className))
    {
        return JavaValue{.kind = Kind::Other, .arrayClass = std::string(className)};
    }
    if (m_Ignored != nullptr)
    {
        m_Ignored->push_back(Failure{.location = location, .message = "object of class \"" + std::string(className) + "\" ignored"});
    }
    return JavaValue{};
}

Result<JavaValue> ValueReader::arrayValue(const osp::PropertyView& property, const std::string& location) const
{
    const auto className = property.className();
    if (!resolvableArrayClass(className))
    {
        return fail(location, "array class \"" + std::string(className) + "\" cannot be loaded");
    }
    const auto component = componentClass(className);
    const auto elements = property.elements();
    JavaValue array{.kind = Kind::Array, .arrayClass = std::string(className)};
    if (elements.empty())
    {
        return array;
    }
    const auto& first = elements.front();
    if (first.name() == "array")
    {
        if (osp::isTextType(first.type()))
        {
            return compactValue(first.content(), component, location);
        }
        if (first.node().children.empty())
        {
            return fail(location, "compact array without content");
        }
        return JavaValue{};
    }
    const auto lastIndex = elements.back().elementIndex();
    if (!lastIndex || *lastIndex < -1)
    {
        return fail(location, "invalid array element index");
    }
    array.elements.resize(*lastIndex < 0 ? 0 : static_cast<std::size_t>(*lastIndex) + 1);
    for (const auto& element : elements)
    {
        const auto index = element.elementIndex();
        if (!index)
        {
            return fail(location, "invalid array element index");
        }
        const auto itemLocation = elementLocation(location, element.name().value_or(""));
        Result<JavaValue> value = JavaValue{};
        switch (element.type())
        {
        case PropertyType::Object:
            value = objectValue(element, itemLocation);
            break;
        case PropertyType::Array:
            value = arrayValue(element, itemLocation);
            break;
        case PropertyType::Collection:
            value = collectionValue(element, itemLocation);
            break;
        case PropertyType::Unknown:
            // XMLControlElement.arrayValue has no case for unknown types: the slot is left as it is.
            continue;
        default:
            value = propertyValue(element, itemLocation);
            break;
        }
        if (!value)
        {
            return value;
        }
        if (*index < 0 || static_cast<std::size_t>(*index) >= array.elements.size())
        {
            return fail(itemLocation, "array index out of bounds");
        }
        if (!assignable(component, *value))
        {
            return fail(itemLocation, "element type does not match array class \"" + std::string(className) + "\"");
        }
        array.elements[static_cast<std::size_t>(*index)] = *std::move(value);
    }
    return array;
}

Result<JavaValue> ValueReader::collectionValue(const osp::PropertyView& property, const std::string& location) const
{
    const auto className = property.className();
    if (std::ranges::find(COLLECTION_CLASSES, className) == COLLECTION_CLASSES.end())
    {
        return JavaValue{};
    }
    JavaValue collection{.kind = Kind::Collection, .arrayClass = std::string(className)};
    for (const auto& element : property.elements())
    {
        const auto itemLocation = location + "." + std::string(element.name().value_or("item"));
        Result<JavaValue> value = JavaValue{};
        switch (element.type())
        {
        case PropertyType::Object:
            value = objectValue(element, itemLocation);
            break;
        case PropertyType::String:
        case PropertyType::Array:
        case PropertyType::Collection:
            value = propertyValue(element, itemLocation);
            break;
        default:
            // collectionValue adds only objects, strings, arrays and collections.
            continue;
        }
        if (!value)
        {
            // collectionValue catches every exception and returns null.
            return JavaValue{};
        }
        collection.elements.push_back(*std::move(value));
    }
    return collection;
}

Result<std::optional<StringList>> castStringArray(const JavaValue& value, const std::string& location)
{
    if (value.kind == Kind::Null)
    {
        return std::optional<StringList>{};
    }
    if (value.kind != Kind::Array || value.arrayClass != "[Ljava.lang.String;")
    {
        return fail(location, "not a String[] array");
    }
    StringList strings;
    strings.reserve(value.elements.size());
    for (const auto& element : value.elements)
    {
        strings.push_back(element.kind == Kind::String ? std::optional<std::string>(element.text) : std::nullopt);
    }
    return std::optional<StringList>(std::move(strings));
}

Result<std::optional<StringRows>> castStringMatrix(const JavaValue& value, const std::string& location)
{
    if (value.kind == Kind::Null)
    {
        return std::optional<StringRows>{};
    }
    if (value.kind != Kind::Array || value.arrayClass != "[[Ljava.lang.String;")
    {
        return fail(location, "not a String[][] array");
    }
    StringRows rows;
    rows.reserve(value.elements.size());
    for (const auto& row : value.elements)
    {
        if (row.kind == Kind::Null)
        {
            rows.emplace_back();
            continue;
        }
        auto strings = castStringArray(row, location);
        if (!strings)
        {
            return osp::makeUnexpected(strings.error());
        }
        auto& list = *strings;
        if (!list)
        {
            return fail(location, "not a String[] row");
        }
        rows.emplace_back(std::move(*list));
    }
    return std::optional<StringRows>(std::move(rows));
}

osp::PropertyValue stringArrayProperty(std::string name, const StringList& values)
{
    std::vector<std::optional<osp::PropertyValue>> elements;
    elements.reserve(values.size());
    for (const auto& value : values)
    {
        elements.push_back(value ? std::optional<osp::PropertyValue>(osp::stringProperty("", *value)) : std::nullopt);
    }
    return osp::arrayProperty(std::move(name), "[Ljava.lang.String;", elements);
}

osp::PropertyValue stringMatrixProperty(std::string name, const StringRows& rows)
{
    std::vector<std::optional<osp::PropertyValue>> elements;
    elements.reserve(rows.size());
    for (const auto& row : rows)
    {
        elements.push_back(row ? std::optional<osp::PropertyValue>(stringArrayProperty("", *row)) : std::nullopt);
    }
    return osp::arrayProperty(std::move(name), "[[Ljava.lang.String;", elements);
}

namespace
{

std::optional<std::size_t> rank(std::span<const std::string_view> order, std::string_view name)
{
    const auto found = std::ranges::find(order, name);
    if (found == order.end())
    {
        return std::nullopt;
    }
    return static_cast<std::size_t>(found - order.begin());
}

} // namespace

PatchEditor::PatchEditor(const osp::Source& source, osp::NodeId object, osp::Patch& patch, std::span<const std::string_view> order)
    : m_Source(&source), m_Object(object), m_Patch(&patch), m_Order(order)
{}

std::optional<osp::NodeId> PatchEditor::last(std::string_view name) const
{
    for (const auto child : std::views::reverse(m_Source->node(m_Object).children))
    {
        if (m_Source->node(child).name == name)
        {
            return child;
        }
    }
    return std::nullopt;
}

std::size_t PatchEditor::insertionIndex(std::string_view name) const
{
    const auto& children = m_Source->node(m_Object).children;
    const auto wanted = rank(m_Order, name);
    for (std::size_t index = 0; index < children.size(); ++index)
    {
        const auto& childName = m_Source->node(children[index]).name;
        const auto existing = childName ? rank(m_Order, *childName) : std::nullopt;
        if (wanted && existing && *existing > *wanted)
        {
            return index;
        }
    }
    return children.size();
}

void PatchEditor::setString(std::string_view name, std::string value)
{
    if (const auto property = last(name))
    {
        if (m_Source->node(*property).type == PropertyType::String)
        {
            m_Patch->setString(*property, std::move(value));
        }
        else
        {
            m_Patch->replace(*property, osp::stringProperty(std::string(name), value));
        }
        return;
    }
    m_Patch->insert(m_Object, insertionIndex(name), osp::stringProperty(std::string(name), value));
}

void PatchEditor::setProperty(osp::PropertyValue value)
{
    const auto name = value.name.value_or("");
    if (const auto property = last(name))
    {
        m_Patch->replace(*property, std::move(value));
        return;
    }
    m_Patch->insert(m_Object, insertionIndex(name), std::move(value));
}

void PatchEditor::removeAll(std::string_view name)
{
    for (const auto child : m_Source->node(m_Object).children)
    {
        if (m_Source->node(child).name == name)
        {
            m_Patch->remove(child);
        }
    }
}

ValueEditor::ValueEditor(osp::ObjectValue& object, std::span<const std::string_view> order) : m_Object(&object), m_Order(order)
{}

std::optional<std::size_t> ValueEditor::last(std::string_view name) const
{
    const auto& properties = m_Object->properties;
    for (std::size_t index = properties.size(); index-- > 0;)
    {
        if (properties[index].name == name)
        {
            return index;
        }
    }
    return std::nullopt;
}

std::size_t ValueEditor::insertionIndex(std::string_view name) const
{
    const auto& properties = m_Object->properties;
    const auto wanted = rank(m_Order, name);
    for (std::size_t index = 0; index < properties.size(); ++index)
    {
        const auto& name = properties[index].name;
        const auto existing = name ? rank(m_Order, *name) : std::nullopt;
        if (wanted && existing && *existing > *wanted)
        {
            return index;
        }
    }
    return properties.size();
}

void ValueEditor::setString(std::string_view name, std::string value)
{
    setProperty(osp::stringProperty(std::string(name), value));
}

void ValueEditor::setProperty(osp::PropertyValue value)
{
    auto& properties = m_Object->properties;
    if (const auto index = last(value.name.value_or("")))
    {
        properties[*index] = std::move(value);
        return;
    }
    const auto position = insertionIndex(value.name.value_or(""));
    properties.insert(properties.begin() + static_cast<std::ptrdiff_t>(position), std::move(value));
}

void ValueEditor::removeAll(std::string_view name)
{
    std::erase_if(m_Object->properties, [name](const osp::PropertyValue& property) { return property.name == name; });
}

} // namespace motrack::library::detail
