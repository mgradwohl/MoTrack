#pragma once

#include "library/resource.hpp"
#include "osp/expected.hpp"
#include "osp/render.hpp"
#include "osp/source.hpp"
#include "osp/value.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/// Internal: what `XMLControlElement.getObject`/`getString` return for the properties that OSP's library loaders
/// cast, and editors that change one object's properties either as a `Patch` or in an `ObjectValue`.
namespace motrack::library::detail
{

/// A point where OSP's loader throws (ClassCastException, ArrayIndexOutOfBoundsException, NullPointerException,
/// NumberFormatException, IllegalArgumentException from Array.set). The Library Browser then cannot open the file.
struct Failure
{
    std::string location;
    std::string message;
};

template <typename T> using Result = osp::Expected<T, Failure>;

[[nodiscard]] inline osp::Unexpected<Failure> fail(std::string location, std::string message)
{
    return osp::makeUnexpected(Failure{.location = std::move(location), .message = std::move(message)});
}

/// The Java value of a property, reduced to what a cast can distinguish.
///
/// Arrays are sparse: `length` is the Java length and only non-null slots are stored, so a file naming a large
/// index (Java allocates a reference array of that length) costs nothing until a caller asks for a dense list.
/// A loaded resource lives on the heap, which keeps the loader's recursion frames small.
struct JavaValue
{
    enum class Kind : std::uint8_t
    {
        Null,
        String,
        Array,      ///< `arrayClass` is its runtime class.
        Collection, ///< `arrayClass` holds the collection class; `indices` are positions.
        Resource,   ///< A LibraryResource/LibraryCollection object.
        Other,      ///< Any other non-null value (boxed numbers, other objects).
    };
    Kind kind = Kind::Null;
    std::string text{};
    std::string arrayClass{};
    std::size_t length = 0;             ///< Arrays and collections.
    std::vector<std::size_t> indices{}; ///< Increasing indices of the non-null slots.
    std::vector<JavaValue> values{};    ///< Their values.
    std::unique_ptr<library::Resource> resource{};
};

/// Loads a LibraryResource/LibraryCollection object found inside a property. A plain function and context rather
/// than std::function: the call is on the loader's recursion path, where every frame counts.
struct ResourceObjectLoader
{
    using Function = Result<std::unique_ptr<library::Resource>> (*)(void* context,
                                                                    const osp::ObjectView& object,
                                                                    const std::string& location);
    Function function = nullptr;
    void* context = nullptr;
};

/// A loader that only marks resource objects as present; for reading values that hold no resources.
[[nodiscard]] ResourceObjectLoader placeholderLoader() noexcept;

/// Classes MoTrack treats as present on Tracker's class path: the library classes and the `java.`/`javax.`
/// packages. Java resolves classes by name; a property whose class it cannot load reads as null, while a known
/// class of the wrong type makes the loader throw.
[[nodiscard]] bool isKnownJavaClass(std::string_view className) noexcept;

class ValueReader
{
  public:
    /// `ignored` collects objects of classes OSP cannot load, which it reads as null.
    explicit ValueReader(ResourceObjectLoader loader, std::vector<Failure>* ignored = nullptr);

    /// XMLControlElement.getString: the string property's removeCDATA value; null when the property is missing or
    /// not a string. Malformed CDATA throws in Java.
    [[nodiscard]] Result<std::optional<std::string>>
    getString(const osp::ObjectView& object, std::string_view name, const std::string& location) const;
    /// XMLControlElement.getObject for the last property with this name.
    [[nodiscard]] Result<JavaValue> getObject(const osp::ObjectView& object, std::string_view name, const std::string& location) const;
    [[nodiscard]] Result<JavaValue> propertyValue(const osp::PropertyView& property, const std::string& location) const;

  private:
    [[nodiscard]] Result<JavaValue> objectValue(const osp::PropertyView& property, const std::string& location) const;
    /// The value an array or collection element contributes, by its type.
    [[nodiscard]] Result<JavaValue> elementValue(const osp::PropertyView& element, const std::string& location) const;
    /// Stores one "[n]" element of an array, or reports why Java's Array.set or the element's value throws.
    [[nodiscard]] std::optional<Failure> assign(const osp::PropertyView& element,
                                                std::string_view className,
                                                const std::string& location,
                                                std::size_t length,
                                                std::vector<std::pair<std::size_t, JavaValue>>& assigned) const;
    void noteIgnored(const std::string& location, std::string_view className) const;
    [[nodiscard]] Result<JavaValue> arrayValue(const osp::PropertyView& property, const std::string& location) const;
    [[nodiscard]] Result<JavaValue> collectionValue(const osp::PropertyView& property, const std::string& location) const;

    ResourceObjectLoader m_Loader;
    std::vector<Failure>* m_Ignored;
};

/// A dense String[] for writing: null entries allowed.
using StringList = std::vector<std::optional<std::string>>;
/// Dense String[][] rows for writing.
using StringRows = std::vector<std::optional<StringList>>;

/// A decoded `String[]`: its Java length and its non-null entries.
struct StringArray
{
    std::size_t length = 0;
    std::vector<std::pair<std::size_t, std::string>> present{}; ///< Increasing indices.

    /// The entry at `index`, or nullptr when it is null (or beyond the length).
    [[nodiscard]] const std::string* at(std::size_t index) const noexcept;
    /// The dense list, refused when the length exceeds `limit` (a document cannot hold more entries than nodes,
    /// so a longer array is almost entirely nulls written as one large index).
    [[nodiscard]] Result<StringList> dense(std::size_t limit, const std::string& location) const;
};

/// A decoded `String[][]`: its Java length and its non-null rows.
struct StringMatrix
{
    std::size_t length = 0;
    std::vector<std::pair<std::size_t, StringArray>> present{};
};

/// `(String[]) value`: null, or the array.
[[nodiscard]] Result<std::optional<StringArray>> castStringArray(const JavaValue& value, const std::string& location);
/// `(String[][]) value`: null, or the rows.
[[nodiscard]] Result<std::optional<StringMatrix>> castStringMatrix(const JavaValue& value, const std::string& location);

/// A one-dimensional String[] property as OSP's writer emits it (`[j]` elements, nulls skipped but the last).
[[nodiscard]] osp::PropertyValue stringArrayProperty(std::string name, const StringList& values);
/// A String[][] property of rows.
[[nodiscard]] osp::PropertyValue stringMatrixProperty(std::string name, const StringRows& rows);

/// Edits the properties of one object. Lookups use the last property with a name, as OSP does; new properties
/// are inserted before the first property that OSP's writer emits later (`order`), or appended.
class PropertyEditor
{
  public:
    virtual ~PropertyEditor() = default;
    PropertyEditor() = default;
    PropertyEditor(const PropertyEditor&) = delete;
    PropertyEditor& operator=(const PropertyEditor&) = delete;
    PropertyEditor(PropertyEditor&&) = delete;
    PropertyEditor& operator=(PropertyEditor&&) = delete;

    /// Sets a string property: edits the text of the last string property with this name, replaces a property of
    /// another type, or inserts one.
    virtual void setString(std::string_view name, std::string value) = 0;
    /// Replaces the last property with `value.name`, or inserts it.
    virtual void setProperty(osp::PropertyValue value) = 0;
    /// Removes every property with this name.
    virtual void removeAll(std::string_view name) = 0;
    /// Inserts alue right after the last property with its name, which stays in place but is shadowed (OSP reads
    /// the last one), or where setProperty would insert it.
    virtual void insertAfterLast(osp::PropertyValue value) = 0;
};

class PatchEditor final : public PropertyEditor
{
  public:
    PatchEditor(const osp::Source& source, osp::NodeId object, osp::Patch& patch, std::span<const std::string_view> order);
    void setString(std::string_view name, std::string value) override;
    void setProperty(osp::PropertyValue value) override;
    void removeAll(std::string_view name) override;
    void insertAfterLast(osp::PropertyValue value) override;

  private:
    [[nodiscard]] std::optional<osp::NodeId> last(std::string_view name) const;
    [[nodiscard]] std::size_t insertionIndex(std::string_view name) const;

    const osp::Source* m_Source;
    osp::NodeId m_Object;
    osp::Patch* m_Patch;
    std::span<const std::string_view> m_Order;
};

class ValueEditor final : public PropertyEditor
{
  public:
    ValueEditor(osp::ObjectValue& object, std::span<const std::string_view> order);
    void setString(std::string_view name, std::string value) override;
    void setProperty(osp::PropertyValue value) override;
    void removeAll(std::string_view name) override;
    void insertAfterLast(osp::PropertyValue value) override;

  private:
    [[nodiscard]] std::optional<std::size_t> last(std::string_view name) const;
    [[nodiscard]] std::size_t insertionIndex(std::string_view name) const;

    osp::ObjectValue* m_Object;
    std::span<const std::string_view> m_Order;
};

} // namespace motrack::library::detail
