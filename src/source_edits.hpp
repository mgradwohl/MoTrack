#pragma once

#include "osp/expected.hpp"
#include "osp/render.hpp"
#include "osp/source.hpp"
#include "osp/value.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace motrack
{

/// Edits against an immutable OSP source, composed and rendered as one `osp::Patch`.
///
/// `osp::Patch` accepts one operation per node. The Tracker writers make several decisions in sequence (a
/// rename, then a deletion that renumbers the same array), so this overlay lets later edits refine earlier
/// ones: a value can be set again, edits inside a removed subtree are dropped, and a renamed array element is
/// rendered as one replacement that carries the edits inside it. Reads (`content`, `children`) see the
/// pending state. `render` re-parses the output and checks oracle O8: every node outside the edit set keeps
/// its bytes and logical value, every edited value decodes to the intended one and new content reads back as
/// written. No Qt dependency.
class SourceEdits
{
  public:
    explicit SourceEdits(std::shared_ptr<const osp::Source> source);

    [[nodiscard]] const osp::Source& source() const noexcept;
    [[nodiscard]] const std::shared_ptr<const osp::Source>& sourcePointer() const noexcept;

    /// Typed setters require a property of the matching OSP type when rendered.
    void setString(osp::NodeId property, std::string value);
    void setInt(osp::NodeId property, std::int32_t value);
    void setDouble(osp::NodeId property, double value);
    void setBoolean(osp::NodeId property, bool value);
    void remove(osp::NodeId property);
    /// `index` counts the parent's original children; insertions at one index keep their order.
    void insert(osp::NodeId parent, std::size_t index, osp::PropertyValue value);
    void append(osp::NodeId parent, osp::PropertyValue value);
    /// Renames a property (an array element after renumbering). Rendered as a replacement of its subtree.
    void rename(osp::NodeId property, std::string name);

    /// True when the node or an ancestor is removed.
    [[nodiscard]] bool removed(osp::NodeId node) const;
    /// The pending logical content of a text property, or its source content.
    [[nodiscard]] std::string content(osp::NodeId property) const;
    /// The pending string, or XMLControlElement.getString of the source property (its content when not a string).
    [[nodiscard]] std::string text(osp::NodeId property) const;
    /// The property's pending name.
    [[nodiscard]] std::optional<std::string> name(osp::NodeId property) const;
    /// Source children that remain (not removed), in order. Insertions are not included.
    [[nodiscard]] std::vector<osp::NodeId> children(osp::NodeId parent) const;
    [[nodiscard]] bool hasInsertions(osp::NodeId parent) const;
    /// Pending insertions into a parent, in insertion order.
    [[nodiscard]] std::vector<osp::PropertyValue> insertions(osp::NodeId parent) const;
    /// The logical value a property will have after the pending edits inside it.
    [[nodiscard]] osp::PropertyValue value(osp::NodeId property) const;
    [[nodiscard]] bool empty() const noexcept;

    struct Rendered
    {
        std::string xml;
        std::shared_ptr<const osp::Source> source; ///< The parsed output.
    };
    /// Renders the edits, re-parses the output and verifies O8. The message describes a failure.
    [[nodiscard]] osp::Expected<Rendered, std::string> render(const osp::ParseLimits& limits = {}) const;

  private:
    using Value = std::variant<std::string, std::int32_t, double, bool>;
    struct Pending
    {
        std::size_t index = 0;
        osp::PropertyValue value{};
    };

    [[nodiscard]] bool edited(osp::NodeId node) const;
    [[nodiscard]] bool replacedAncestor(osp::NodeId node) const;
    [[nodiscard]] osp::PropertyValue intended(osp::NodeId property) const;
    [[nodiscard]] osp::ObjectValue intendedObject(osp::NodeId object) const;
    [[nodiscard]] std::vector<osp::PropertyValue> intendedChildren(osp::NodeId parent) const;
    [[nodiscard]] osp::Expected<void, std::string> verify(const osp::Source& output) const;
    [[nodiscard]] osp::Expected<void, std::string>
    verifyChildren(const osp::Source& output, osp::NodeId before, osp::NodeId after, const std::string& path) const;
    [[nodiscard]] osp::Expected<void, std::string>
    verifyProperty(const osp::Source& output, osp::NodeId before, osp::NodeId after, const std::string& path) const;

    std::shared_ptr<const osp::Source> m_Source;
    std::map<osp::NodeId, Value> m_Values;
    std::set<osp::NodeId> m_Removed;
    std::map<osp::NodeId, std::vector<Pending>> m_Inserts;
    std::map<osp::NodeId, std::string> m_Renames;
    /// Nodes with an edit in their subtree (including themselves); maintained by every mutation.
    std::set<osp::NodeId> m_Touched;
};

} // namespace motrack
