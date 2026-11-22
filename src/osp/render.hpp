#pragma once

#include "osp/expected.hpp"
#include "osp/source.hpp"
#include "osp/value.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace motrack::osp
{

struct RenderError
{
    enum class Code : std::uint8_t
    {
        InvalidNode,        ///< An operation names a node of the wrong kind or outside the source.
        TypeMismatch,       ///< A typed setter targets a property of another OSP type.
        ConflictingEdits,   ///< Two operations touch the same node, or one targets a removed/replaced subtree.
        InvalidValue,       ///< New content that OSP cannot read back as written.
        UnsupportedLayout,  ///< The source layout cannot receive the edit (an insertion at an unclosed end).
        TooLarge,           ///< The output exceeds the parse limits.
        VerificationFailed, ///< Re-parsing the output did not yield the intended tree.
    };
    Code code = Code::VerificationFailed;
    std::string message{};
};

/// Edits against an immutable `Source`, applied by `render`. Node ids refer to that source; an insertion index
/// counts the parent's original children (several insertions at one index keep their order).
class Patch
{
  public:
    /// Replaces a text property's logical content. For string properties, CDATA is added when required.
    Patch& setContent(NodeId property, std::string content);
    /// Typed setters require the property to have the corresponding OSP type and write Java's formatting.
    Patch& setString(NodeId property, std::string value);
    Patch& setInt(NodeId property, std::int32_t value);
    Patch& setDouble(NodeId property, double value);
    Patch& setBoolean(NodeId property, bool value);
    /// Inserts into an object (as a property) or an array/collection property (as an element).
    Patch& insert(NodeId parent, std::size_t index, PropertyValue value);
    Patch& append(NodeId parent, PropertyValue value);
    Patch& remove(NodeId property);
    /// Replaces a whole property subtree.
    Patch& replace(NodeId property, PropertyValue value);
    [[nodiscard]] bool empty() const noexcept;

  private:
    friend class Renderer;
    enum class Expect : std::uint8_t
    {
        Text,
        Int,
        Double,
        Boolean,
        String,
    };
    struct SetContent
    {
        NodeId property{};
        std::string content{};
        Expect expect{};
    };
    struct Insert
    {
        NodeId parent{};
        std::size_t index{};
        PropertyValue value{};
    };
    struct Remove
    {
        NodeId property{};
    };
    struct Replace
    {
        NodeId property{};
        PropertyValue value{};
    };
    using Operation = std::variant<SetContent, Insert, Remove, Replace>;
    static constexpr std::size_t END = static_cast<std::size_t>(-1);
    std::vector<Operation> m_Operations;
};

/// Applies `patch` to `source`. Untouched bytes are copied verbatim, including other lines' terminators, comments
/// and ignored text; changed content uses OSP's writer layout, four-space nesting below the indentation of the
/// line it replaces or of a sibling, and the source's own line ending. The output is re-parsed with the same
/// reader and must produce exactly the intended logical tree, or a VerificationFailed error is returned.
[[nodiscard]] Expected<std::string, RenderError> render(const Source& source, const Patch& patch, const ParseLimits& limits = {});

struct WriteOptions
{
    LineEnding lineEnding = LineEnding::Lf;
    ParseLimits limits{};
};

/// Writes a new document as XMLControlElement.toString does: an XML declaration, the root object with
/// four-space indentation and no final line break. The output is verified by re-parsing.
[[nodiscard]] Expected<std::string, RenderError> write(const ObjectValue& root, const WriteOptions& options = {});

} // namespace motrack::osp
