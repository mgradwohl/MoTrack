#pragma once

#include "osp/expected.hpp"
#include "osp/source.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace motrack::osp
{

struct ObjectValue;

/// The logical tree OSP's reader builds, independent of layout. It describes new content for a `Patch` or a
/// written document, and it is what a render's verification compares after re-parsing.
///
/// Text is the logical content: CDATA markers excluded and physical lines joined with "\n". Attribute values
/// (`name`, `typeName`, `className`, `version`) are kept exactly as written.
struct PropertyValue
{
    std::optional<std::string> name{};
    std::string typeName{};                      ///< Type attribute, normally one of the OSP type names.
    std::string className{};                     ///< Array/collection class attribute.
    std::string content{};                       ///< Text types (int, double, boolean, string, unknown).
    std::vector<PropertyValue> elements{};       ///< Arrays and collections.
    std::shared_ptr<const ObjectValue> object{}; ///< Object type; empty for a null object.

    [[nodiscard]] PropertyType type() const noexcept;
    friend bool operator==(const PropertyValue& left, const PropertyValue& right);
};

struct ObjectValue
{
    std::string className{};              ///< Class attribute as written; readers apply effectiveClassName.
    std::optional<std::string> version{}; ///< Only the root object's version is read.
    std::vector<PropertyValue> properties{};

    friend bool operator==(const ObjectValue& left, const ObjectValue& right) = default;
};

/// Logical value of a parsed property or object (a deep copy).
[[nodiscard]] PropertyValue propertyValue(const Source& source, NodeId property);
[[nodiscard]] ObjectValue objectValue(const Source& source, NodeId object);

// Builders following XMLPropertyElement's constructor (OSP's writer).

[[nodiscard]] PropertyValue intProperty(std::string name, std::int32_t value);
[[nodiscard]] PropertyValue doubleProperty(std::string name, double value);
[[nodiscard]] PropertyValue booleanProperty(std::string name, bool value);
/// The logical string; the writer adds CDATA when it contains `" < > & '` (XML.requiresCDATA).
[[nodiscard]] PropertyValue stringProperty(std::string name, std::string_view value);
[[nodiscard]] PropertyValue objectProperty(std::string name, ObjectValue value);
/// Valid only as an array or collection element; OSP cannot read a null object directly in an object.
[[nodiscard]] PropertyValue nullObjectProperty(std::string name);
/// An object array: elements are renamed "[j]"; empty entries are skipped except the last one, which becomes a
/// null object when `writeNullFinalElement` (OSP's default, XMLPropertyElement line 128).
[[nodiscard]] PropertyValue arrayProperty(std::string name,
                                          std::string className,
                                          const std::vector<std::optional<PropertyValue>>& elements,
                                          bool writeNullFinalElement = true);
/// A collection: items are renamed "item".
[[nodiscard]] PropertyValue collectionProperty(std::string name, std::string className, std::vector<PropertyValue> items);
/// A primitive array as OSP writes it with compactArraySize = 0 (line 123): one "array" string when the
/// element count Java computes is positive, otherwise "[j]" elements. `className` is "[I", "[[D", etc. and must
/// match the values' structure.
[[nodiscard]] Expected<PropertyValue, DecodeError>
primitiveArrayProperty(std::string name, std::string_view className, const CompactArray& values, bool writeNullFinalElement = true);

/// Replaces "\r\n" and "\r" with "\n", the logical line separator.
[[nodiscard]] std::string normalizeLineBreaks(std::string_view text);

} // namespace motrack::osp
