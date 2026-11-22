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
    if (isPrimitiveCode(component) || value.kind == Kind::Null)
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
        return value.kind == Kind::Resource && value.resource && value.resource->isCollection();
    }
    if (component.starts_with('['))
    {
        return value.kind == Kind::Array && value.arrayClass == component;
    }
    return true;
}

/// Stores the non-null slots of `assigned` (index, value pairs in file order, so a later duplicate wins) sorted by
/// index.
void storeSlots(JavaValue& array, std::vector<std::pair<std::size_t, JavaValue>>& assigned)
{
    const auto byIndex = [](const auto& left, const auto& right)
    {
        return left.first < right.first;
    };
    if (!std::ranges::is_sorted(assigned, byIndex))
    {
        std::ranges::stable_sort(assigned, byIndex);
    }
    for (std::size_t position = 0; position < assigned.size(); ++position)
    {
        auto& [index, value] = assigned[position];
        const bool replaced = position + 1 < assigned.size() && assigned[position + 1].first == index;
        if (replaced || value.kind == Kind::Null)
        {
            continue;
        }
        array.indices.push_back(index);
        array.values.push_back(std::move(value));
    }
}

/// XMLControlElement.arrayValue(String, Class): the compact "{...}" form. Strings and objects are never written
/// compactly, so their elements stay null; primitive values only need to parse.
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
                    ++array.length; // a null row
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
                array.indices.push_back(array.length++);
                array.values.push_back(*std::move(row));
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
    }
    array.length = fields.size();
    return array;
}
} // namespace

bool isKnownJavaClass(std::string_view className) noexcept
{
    return className == RESOURCE_CLASS || className == COLLECTION_CLASS || className == LIBRARY_CLASS || className.starts_with("java.") ||
           className.starts_with("javax.");
}

ValueReader::ValueReader(ResourceObjectLoader loader, std::vector<Failure>* ignored) : m_Loader(loader), m_Ignored(ignored)
{}

ResourceObjectLoader placeholderLoader() noexcept
{
    return ResourceObjectLoader{
        .function = [](void*, const osp::ObjectView&, const std::string&) -> Result<std::unique_ptr<library::Resource>>
        {
            return std::make_unique<library::Resource>();
        }};
}

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

namespace
{

// Loading recurses through getObject, elementValue, arrayValue/collectionValue, objectValue, resourceValue and the
// loader for every nested collection. Debug builds give each temporary its own stack slot, so these functions
// build failures and other values in the small helpers below and return them directly (guaranteed elision).

Result<JavaValue> failure(const std::string& location, std::string_view message)
{
    return fail(location, std::string(message));
}

Result<JavaValue> classFailure(const std::string& location, std::string_view before, std::string_view className, std::string_view after)
{
    return fail(location, std::string(before) + std::string(className) + std::string(after));
}

Result<JavaValue> emptyArray(std::string_view className)
{
    return JavaValue{.kind = Kind::Array, .arrayClass = std::string(className)};
}

Result<JavaValue> otherValue(std::string_view className)
{
    return JavaValue{.kind = Kind::Other, .arrayClass = std::string(className)};
}

Result<JavaValue> resourceValue(Result<std::unique_ptr<Resource>>&& loaded)
{
    if (!loaded)
    {
        return osp::makeUnexpected(std::move(loaded.error()));
    }
    return JavaValue{.kind = Kind::Resource, .resource = std::move(*loaded)};
}

std::optional<Failure> slotFailure(const std::string& location, std::string_view message, std::string_view className)
{
    auto text = std::string(message);
    if (!className.empty())
    {
        text += "\"" + std::string(className) + "\"";
    }
    return Failure{.location = location, .message = std::move(text)};
}
std::string joined(const std::string& location, std::string_view separator, std::string_view name)
{
    return location + std::string(separator) + std::string(name);
}

/// The first element is named "array": XMLControlElement.arrayValue reads its content as a compact string.
Result<JavaValue> compactElement(const osp::PropertyView& first, std::string_view className, const std::string& location)
{
    if (osp::isTextType(first.type()))
    {
        return compactValue(first.content(), componentClass(className), location);
    }
    if (first.node().children.empty())
    {
        return failure(location, "compact array without content");
    }
    return {};
}

/// Scalars and unknown types, which never recurse.
Result<JavaValue> scalarValue(const osp::PropertyView& property, const std::string& location)
{
    switch (property.type())
    {
    case PropertyType::Int:
        if (!osp::parseJavaInt(property.content()))
        {
            return failure(location, "invalid int");
        }
        return JavaValue{.kind = Kind::Other};
    case PropertyType::Double:
        if (!osp::parseJavaDouble(property.content()))
        {
            return failure(location, "invalid double");
        }
        return JavaValue{.kind = Kind::Other};
    case PropertyType::Boolean:
        return JavaValue{.kind = Kind::Other};
    case PropertyType::String:
    {
        auto text = property.stringValue();
        if (!text)
        {
            return failure(location, "malformed CDATA section");
        }
        return JavaValue{.kind = Kind::String, .text = *std::move(text)};
    }
    default:
        return {};
    }
}

} // namespace

Result<JavaValue> ValueReader::getObject(const osp::ObjectView& object, std::string_view name, const std::string& location) const
{
    const auto property = object.property(name);
    if (!property)
    {
        return {};
    }
    return elementValue(*property, joined(location, ".", name));
}

Result<JavaValue> ValueReader::propertyValue(const osp::PropertyView& property, const std::string& location) const
{
    return elementValue(property, location);
}

Result<JavaValue> ValueReader::elementValue(const osp::PropertyView& element, const std::string& location) const
{
    switch (element.type())
    {
    case PropertyType::Object:
        return objectValue(element, location);
    case PropertyType::Array:
        return arrayValue(element, location);
    case PropertyType::Collection:
        return collectionValue(element, location);
    default:
        return scalarValue(element, location);
    }
}

Result<JavaValue> ValueReader::objectValue(const osp::PropertyView& property, const std::string& location) const
{
    const auto object = property.object();
    if (!object)
    {
        return {};
    }
    const auto className = object->className();
    if (className == RESOURCE_CLASS || className == COLLECTION_CLASS)
    {
        return resourceValue(m_Loader.function(m_Loader.context, *object, location));
    }
    if (isKnownJavaClass(className))
    {
        return otherValue(className);
    }
    noteIgnored(location, className);
    return {};
}

void ValueReader::noteIgnored(const std::string& location, std::string_view className) const
{
    if (m_Ignored != nullptr)
    {
        m_Ignored->push_back(Failure{.location = location, .message = "object of class \"" + std::string(className) + "\" ignored"});
    }
}

Result<JavaValue> ValueReader::arrayValue(const osp::PropertyView& property, const std::string& location) const
{
    const auto className = property.className();
    if (!resolvableArrayClass(className))
    {
        return classFailure(location, "array class \"", className, "\" cannot be loaded");
    }
    const auto elements = property.elements();
    if (elements.empty())
    {
        return emptyArray(className);
    }
    if (elements.front().name() == "array")
    {
        return compactElement(elements.front(), className, location);
    }
    const auto lastIndex = elements.back().elementIndex();
    if (!lastIndex || *lastIndex < -1)
    {
        return failure(location, "invalid array element index");
    }
    // Java allocates the whole array; MoTrack stores only the slots the file fills.
    const auto length = *lastIndex < 0 ? std::size_t{0} : static_cast<std::size_t>(*lastIndex) + 1;
    std::vector<std::pair<std::size_t, JavaValue>> assigned;
    assigned.reserve(elements.size());
    for (const auto& element : elements)
    {
        if (auto failed = assign(element, className, location, length, assigned))
        {
            return osp::makeUnexpected(std::move(*failed));
        }
    }
    auto array = emptyArray(className);
    array->length = length;
    storeSlots(*array, assigned);
    return array;
}

std::optional<Failure> ValueReader::assign(const osp::PropertyView& element,
                                           std::string_view className,
                                           const std::string& location,
                                           std::size_t length,
                                           std::vector<std::pair<std::size_t, JavaValue>>& assigned) const
{
    const auto index = element.elementIndex();
    if (!index)
    {
        return slotFailure(location, "invalid array element index", {});
    }
    if (element.type() == PropertyType::Unknown)
    {
        // XMLControlElement.arrayValue has no case for unknown types: the slot is left as it is.
        return std::nullopt;
    }
    const auto itemLocation = joined(location, "", element.name().value_or(""));
    auto value = elementValue(element, itemLocation);
    if (!value)
    {
        return std::move(value.error());
    }
    if (*index < 0 || std::cmp_greater_equal(*index, length))
    {
        return slotFailure(itemLocation, "array index out of bounds", {});
    }
    if (!assignable(componentClass(className), *value))
    {
        return slotFailure(itemLocation, "element type does not match array class ", className);
    }
    assigned.emplace_back(static_cast<std::size_t>(*index), std::move(*value));
    return std::nullopt;
}

Result<JavaValue> ValueReader::collectionValue(const osp::PropertyView& property, const std::string& location) const
{
    const auto className = property.className();
    if (std::ranges::find(COLLECTION_CLASSES, className) == COLLECTION_CLASSES.end())
    {
        return {};
    }
    auto collection = emptyArray(className);
    collection->kind = Kind::Collection;
    for (const auto& element : property.elements())
    {
        const auto type = element.type();
        // collectionValue adds only objects, strings, arrays and collections.
        if (type != PropertyType::Object && type != PropertyType::String && type != PropertyType::Array && type != PropertyType::Collection)
        {
            continue;
        }
        auto value = elementValue(element, joined(location, ".", element.name().value_or("item")));
        if (!value)
        {
            // collectionValue catches every exception and returns null.
            return {};
        }
        const auto position = collection->length++;
        if (value->kind != Kind::Null)
        {
            collection->indices.push_back(position);
            collection->values.push_back(std::move(*value));
        }
    }
    return collection;
}

const std::string* StringArray::at(std::size_t index) const noexcept
{
    const auto found = std::ranges::lower_bound(present, index, {}, &std::pair<std::size_t, std::string>::first);
    return found != present.end() && found->first == index ? &found->second : nullptr;
}

Result<StringList> StringArray::dense(std::size_t limit, const std::string& location) const
{
    if (length > limit)
    {
        return fail(location, "array of " + std::to_string(length) + " entries in a document of " + std::to_string(limit) + " nodes");
    }
    StringList list(length);
    for (const auto& [index, text] : present)
    {
        list[index] = text;
    }
    return list;
}

Result<std::optional<StringArray>> castStringArray(const JavaValue& value, const std::string& location)
{
    if (value.kind == Kind::Null)
    {
        return std::optional<StringArray>{};
    }
    if (value.kind != Kind::Array || value.arrayClass != "[Ljava.lang.String;")
    {
        return fail(location, "not a String[] array");
    }
    StringArray strings{.length = value.length};
    strings.present.reserve(value.values.size());
    for (std::size_t slot = 0; slot < value.values.size(); ++slot)
    {
        if (value.values[slot].kind == Kind::String)
        {
            strings.present.emplace_back(value.indices[slot], value.values[slot].text);
        }
    }
    return std::optional<StringArray>(std::move(strings));
}

Result<std::optional<StringMatrix>> castStringMatrix(const JavaValue& value, const std::string& location)
{
    if (value.kind == Kind::Null)
    {
        return std::optional<StringMatrix>{};
    }
    if (value.kind != Kind::Array || value.arrayClass != "[[Ljava.lang.String;")
    {
        return fail(location, "not a String[][] array");
    }
    StringMatrix rows{.length = value.length};
    rows.present.reserve(value.values.size());
    for (std::size_t slot = 0; slot < value.values.size(); ++slot)
    {
        auto strings = castStringArray(value.values[slot], location);
        if (!strings)
        {
            return osp::makeUnexpected(strings.error());
        }
        auto& row = *strings;
        if (!row)
        {
            return fail(location, "not a String[] row");
        }
        rows.present.emplace_back(value.indices[slot], std::move(*row));
    }
    return std::optional<StringMatrix>(std::move(rows));
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

void PatchEditor::insertAfterLast(osp::PropertyValue value)
{
    const auto name = value.name.value_or("");
    const auto& children = m_Source->node(m_Object).children;
    for (std::size_t index = children.size(); index-- > 0;)
    {
        if (m_Source->node(children[index]).name == name)
        {
            m_Patch->insert(m_Object, index + 1, std::move(value));
            return;
        }
    }
    m_Patch->insert(m_Object, insertionIndex(name), std::move(value));
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

void ValueEditor::insertAfterLast(osp::PropertyValue value)
{
    auto& properties = m_Object->properties;
    const auto name = value.name.value_or("");
    const auto position = last(name);
    const auto index = position ? *position + 1 : insertionIndex(name);
    properties.insert(properties.begin() + static_cast<std::ptrdiff_t>(index), std::move(value));
}
} // namespace motrack::library::detail
