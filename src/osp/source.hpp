#pragma once

#include "osp/expected.hpp"
#include "osp/java_text.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/// Lossless model of Open Source Physics XMLControl files (`.trk`, `.xml`, Data Tool and preference files).
///
/// OSP does not use an XML parser. `XMLControlElement.readInput/readObject/readProperty` read the file line by
/// line with `BufferedReader.readLine()` and `String.indexOf`. MoTrack reproduces that reader exactly so the
/// tree it builds is the tree Tracker builds, including its surprising behaviors (see docs/osp-xmlcontrol.md).
/// The parsed `Source` is immutable and keeps the original bytes and every node's line span, so a render can
/// copy untouched content verbatim. Line numbers in comments refer to OSP d56207b (Tracker 6.3.5).
namespace motrack::osp
{

/// Bounds checked before and during parsing.
struct ParseLimits
{
    std::size_t maxBytes = std::size_t{32} * 1024 * 1024; ///< Same bound as MAX_TRACKER_XML_BYTES.
    std::size_t maxDepth = 256;                           ///< Nested objects and properties.
    std::size_t maxNodes = 2'000'000;
};

/// OSP property type codes (XMLProperty.java). `Unknown` is TYPE_UNKNOWN: an unrecognized type attribute,
/// whose content OSP still reads as text.
enum class PropertyType : std::uint8_t
{
    Int,
    Double,
    Boolean,
    String,
    Array,
    Collection,
    Object,
    Unknown,
};

enum class NodeKind : std::uint8_t
{
    Object,   ///< An `<object class=...>` control.
    Property, ///< A `<property name=... type=...>` element.
};

enum class LineEnding : std::uint8_t
{
    Lf,
    CrLf,
    Cr,
};

/// Index of a node in a `Source`'s node table.
enum class NodeId : std::uint32_t
{
};

/// XMLProperty.getTypeCode: the exact type attribute text, or `Unknown`.
[[nodiscard]] PropertyType propertyTypeFromName(std::string_view name) noexcept;
/// Canonical attribute text for a type; "unknown" for `Unknown`, which MoTrack never writes for new content.
[[nodiscard]] std::string_view propertyTypeName(PropertyType type) noexcept;
/// Types whose value is element text rather than nested elements.
[[nodiscard]] bool isTextType(PropertyType type) noexcept;
/// XMLControlElement.getClassName (line 2143): classes in a package ending with "org.opensourcephysics.media"
/// are moved to its ".core" subpackage, as Tracker 4 reorganized that package.
[[nodiscard]] std::string effectiveClassName(std::string_view className);
[[nodiscard]] std::string_view lineEndingText(LineEnding ending) noexcept;

/// One physical line: text `[begin, end)`, then its terminator `[end, next)` ("\n", "\r\n", "\r" or none).
/// BufferedReader treats each of the three terminators as a line break.
struct LineSpan
{
    std::size_t begin = 0;
    std::size_t end = 0;
    std::size_t next = 0;
};

/// A node as OSP's reader built it, with the source lines it consumed.
struct Node
{
    NodeKind kind = NodeKind::Object;
    PropertyType type = PropertyType::Object; ///< `Object` for object nodes.
    std::optional<NodeId> parent{};
    std::uint32_t depth = 0;
    std::optional<std::string> name{};    ///< Property `name` attribute; absent when the line has none.
    std::string typeName{};               ///< Property `type` attribute text exactly as written.
    std::string className{};              ///< Object class, or array/collection class, after effectiveClassName.
    std::string rawClassName{};           ///< The `class` attribute text exactly as written ("" when unreadable).
    std::optional<std::string> version{}; ///< Root object `version` attribute.
    /// Text value with OSP's reader semantics: no entity decoding; CDATA markers removed only when the reader
    /// took its CDATA path; physical lines joined with "\n" (Java joins with the platform separator).
    std::string content{};
    bool cdata = false;       ///< String read through the CDATA path (line 1764).
    bool isNull = false;      ///< Object property whose opening line contains ">null</property".
    bool selfClosing = false; ///< Object or array/collection whose opening line contains "/>".
    bool closed = true;       ///< False for an object that reached end of file before "</object>".
    std::vector<NodeId> children{};
    std::size_t firstLine = 0; ///< Opening line (0-based).
    std::size_t lastLine = 0;  ///< Last line the reader consumed for this node, inclusive.
    /// Object: the "</object>" line, or the opening line when self-closing; absent when unclosed.
    /// Array/collection: the first non-property line that ended the element list; the opening line when
    /// self-closing.
    std::optional<std::size_t> closeLine{};
    std::size_t valueBegin = 0; ///< Text property: byte offset after the opening line's first '>'.
    std::size_t valueEnd = 0;   ///< Text property: byte offset of the "</property>" that ended the value.
};

struct ParseError
{
    enum class Code : std::uint8_t
    {
        TooLarge,
        InvalidUtf8,         ///< Java would substitute U+FFFD; MoTrack refuses (documented difference).
        UnsupportedEncoding, ///< Declared encoding other than UTF-8; OSP ignores it, MoTrack refuses.
        MissingRoot,         ///< No "<object class=" within the first 10 lines (line 1583).
        MalformedAttribute,  ///< A name, type or version attribute lacks its closing quote.
        MissingType,         ///< Property without a type attribute (getTypeCode(null) throws).
        NullObjectProperty,  ///< ">null</property" directly in an object; setProperty throws (line 1541).
        UnterminatedArray,   ///< End of file inside an array or collection.
        UnterminatedValue,   ///< End of file before a text value's end tag; Java loops until out of memory (#195).
        MissingObject,       ///< End of file where an object property's object should start.
        TooDeep,
        TooManyNodes,
        Encrypted, ///< Password-protected (Cryptic) document: unsupported.
    };
    Code code = Code::MissingRoot;
    std::size_t line = 0; ///< 1-based line where reading failed; 0 when not tied to a line.
    std::string message{};
};

class Source;

/// Parses with OSP's line-oriented reader. The input must be UTF-8; a UTF-8 byte-order mark is kept as text.
[[nodiscard]] Expected<std::shared_ptr<const Source>, ParseError> parse(std::string bytes, const ParseLimits& limits = {});

/// Immutable parse result. Node ids are stable for the lifetime of the source.
class Source
{
  public:
    [[nodiscard]] std::string_view bytes() const noexcept;
    [[nodiscard]] NodeId root() const noexcept;
    [[nodiscard]] std::size_t nodeCount() const noexcept;
    [[nodiscard]] bool contains(NodeId id) const noexcept;
    /// Precondition: `contains(id)`.
    [[nodiscard]] const Node& node(NodeId id) const;
    [[nodiscard]] std::span<const LineSpan> lines() const noexcept;
    [[nodiscard]] std::string_view lineText(std::size_t line) const;
    /// The first line terminator in the file; LF when the file has none.
    [[nodiscard]] LineEnding lineEnding() const noexcept;
    /// From the start of a node's first line to the end of its last line, excluding that terminator.
    [[nodiscard]] std::string_view rawText(NodeId id) const;
    /// Bytes before the root's opening line (declaration, comments) and after its last consumed line.
    [[nodiscard]] std::string_view prolog() const;
    [[nodiscard]] std::string_view epilog() const;
    /// True when an object reached end of file without "</object>". OSP accepts such files.
    [[nodiscard]] bool truncated() const noexcept;

  private:
    friend class SourceParser;
    friend Expected<std::shared_ptr<const Source>, ParseError> parse(std::string bytes, const ParseLimits& limits);
    Source() = default;

    std::string m_Bytes;
    std::vector<LineSpan> m_Lines;
    std::vector<Node> m_Nodes;
    LineEnding m_LineEnding = LineEnding::Lf;
    bool m_Truncated = false;
};

/// Decoded compact primitive array: OSP writes primitive arrays as one "array" string such as `{1.0,2.0}` or
/// `{{1,2},null,{3}}` (XMLPropertyElement line 123).
struct CompactArray
{
    enum class Kind : std::uint8_t
    {
        Null, ///< A null row of a nested array.
        Int,
        Double,
        Boolean,
        Nested,
    };
    Kind kind = Kind::Null;
    std::vector<std::int32_t> ints{};
    std::vector<double> doubles{};
    std::vector<bool> booleans{};
    std::vector<CompactArray> rows{};

    friend bool operator==(const CompactArray&, const CompactArray&) = default;
};

/// XMLControlElement.arrayValue(String, Class) (line 2002) for an array class of `[`s followed by I, D or Z.
/// Rows of nested arrays separated by a bare comma decode as null rows, a trailing comma adds none, and
/// leaf values use Integer.parseInt, Double.parseDouble or Boolean.parseBoolean after Java's String.split.
[[nodiscard]] Expected<CompactArray, DecodeError> parseCompactArray(std::string_view text, std::string_view arrayClass);
/// XMLPropertyElement.getArrayString: `{a,b}` with Java number formatting and "null" for null rows.
[[nodiscard]] std::string formatCompactArray(const CompactArray& array);
/// True for "[I", "[D", "[Z" and their multi-dimensional forms.
[[nodiscard]] bool isPrimitiveArrayClass(std::string_view arrayClass) noexcept;

class ObjectView;

/// Borrowed view of a property node; the source must outlive it.
class PropertyView
{
  public:
    /// Precondition: `id` is a property node of `source`.
    PropertyView(const Source& source, NodeId id);

    [[nodiscard]] NodeId id() const noexcept;
    [[nodiscard]] const Node& node() const;
    [[nodiscard]] std::optional<std::string_view> name() const;
    [[nodiscard]] PropertyType type() const;
    [[nodiscard]] std::string_view content() const;
    [[nodiscard]] std::string_view className() const;
    [[nodiscard]] bool isNull() const;
    /// The object of a non-null object property.
    [[nodiscard]] std::optional<ObjectView> object() const;
    /// Elements of an array or collection, in file order.
    [[nodiscard]] std::vector<PropertyView> elements() const;

    /// XMLControlElement.intValue (line 1879): an int property's Integer.parseInt value.
    [[nodiscard]] Expected<std::int32_t, DecodeError> intValue() const;
    /// XMLControlElement.doubleValue: a double property's Double.parseDouble value.
    [[nodiscard]] Expected<double, DecodeError> doubleValue() const;
    /// XMLControlElement.booleanValue: content equals "true" (case-sensitive).
    [[nodiscard]] Expected<bool, DecodeError> booleanValue() const;
    /// XMLControlElement.stringValue: XML.removeCDATA of a string property's content (XML line 691).
    [[nodiscard]] Expected<std::string, DecodeError> stringValue() const;
    /// The compact form of a primitive array: its first element is named "array" and holds text.
    [[nodiscard]] Expected<CompactArray, DecodeError> compactArray() const;
    /// Index of an "[n]" array element: Integer.parseInt of the text between '[' and the first ']'.
    [[nodiscard]] Expected<std::int32_t, DecodeError> elementIndex() const;
    /// Indexed array slots as XMLControlElement.arrayValue (line 1913) fills them: the last element's index
    /// sets the length, later duplicates replace earlier ones and absent indices stay empty.
    [[nodiscard]] Expected<std::vector<std::optional<PropertyView>>, DecodeError> indexedElements() const;

  private:
    const Source* m_Source;
    NodeId m_Id;
};

/// Borrowed view of an object node; the source must outlive it. Accessors mirror XMLControlElement.
class ObjectView
{
  public:
    /// Precondition: `id` is an object node of `source`.
    ObjectView(const Source& source, NodeId id);

    [[nodiscard]] NodeId id() const noexcept;
    [[nodiscard]] const Node& node() const;
    [[nodiscard]] std::string_view className() const;
    [[nodiscard]] std::vector<PropertyView> properties() const;
    /// The last property with this name: readObject keeps every duplicate in order, but the lookup map keeps
    /// the last one put (line 1539).
    [[nodiscard]] std::optional<PropertyView> property(std::string_view name) const;
    /// getChildControl: the last object-typed property with this name, even when a later property of another
    /// type has the same name (the child map only receives object properties).
    [[nodiscard]] std::optional<ObjectView> child(std::string_view name) const;

    /// getInt (line 383): int or string property through Integer.parseInt; Java returns Integer.MIN_VALUE
    /// on any error. OSPCombo objects are not decoded (Unsupported).
    [[nodiscard]] Expected<std::int32_t, DecodeError> getInt(std::string_view name) const;
    /// getDouble (line 360): double, int or string property through Double.parseDouble; Java returns NaN
    /// on any error.
    [[nodiscard]] Expected<double, DecodeError> getDouble(std::string_view name) const;
    /// getBoolean (line 343): boolean or string content equal to "true"; Java returns false otherwise.
    [[nodiscard]] Expected<bool, DecodeError> getBoolean(std::string_view name) const;
    /// getString (line 413): XML.removeCDATA of a string property; Java returns null otherwise.
    [[nodiscard]] Expected<std::string, DecodeError> getString(std::string_view name) const;

  private:
    const Source* m_Source;
    NodeId m_Id;
};

/// XML.removeCDATA: text between the first "<![CDATA[" and the first "]]>"; unchanged when there is none.
/// Malformed when Java's substring would throw.
[[nodiscard]] Expected<std::string, DecodeError> removeCdata(std::string_view content);

} // namespace motrack::osp
