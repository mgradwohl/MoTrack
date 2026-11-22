#pragma once

#include "model/dependencies/name_binding.hpp"
#include "model/families.hpp"
#include "osp/source.hpp"
#include "osp/value.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

/// Field descriptors of the typed families and the reference-only descriptors of untyped classes
/// (docs/tracker-document.md, "Field descriptors" and "Reference-only descriptors"). One static table per family
/// names every typed field: its stable id, OSP property and type, whether a command edits it and the graph facet
/// that edit changes, and Tracker's loader default. Descriptors also give each typed object's saver tree, the
/// properties Tracker's saver writes for the values its loaded object holds (oracle O3). Standard-only interface.
namespace motrack::model
{

/// A Qt-free scalar value: absent (`std::monostate`) or one of the OSP value types.
using FieldValue =
    std::variant<std::monostate, bool, std::int32_t, double, std::string, std::vector<std::string>, std::vector<std::int32_t>>;

enum class FieldKind : std::uint8_t
{
    Scalar,
    /// `framedata`: Tracker's loaded frame number to an element (a mark, vector sample or calibration key).
    FrameKeyed,
};

struct FieldDescriptor
{
    /// Stable within the family; commands, native v7 and dumps address the field by it.
    std::string_view id{};
    /// The OSP property, which is the id for every B2 field.
    std::string_view property{};
    /// The type Tracker's saver writes.
    osp::PropertyType type = osp::PropertyType::String;
    FieldKind kind = FieldKind::Scalar;
    /// Whether a command edits the field. Other fields are typed so that they are stored and dumped, never edited.
    bool commandEdits = false;
    /// The facet an edit changes (`markChanged`); absent for a label, which renames instead, and for fields no
    /// command edits.
    std::optional<Facet> facet{};
    /// Whether an edit is a rename (`renameObject`).
    bool label = false;
    /// Tracker's value when the file does not save the property (#214), as Tracker writes it; empty when the loader
    /// keeps no default of its own or depends on other objects (the frame duration).
    std::string_view loaderDefault{};
};

/// The family's typed fields in Tracker's saver order; empty for Unknown.
[[nodiscard]] std::span<const FieldDescriptor> fieldDescriptors(Family family) noexcept;
/// The descriptor with this id, or null.
[[nodiscard]] const FieldDescriptor* findField(Family family, std::string_view id) noexcept;
/// A scalar field's loaded value: the accessor result when readable, otherwise absent. Frame-keyed fields are read
/// through their series and return absent here.
[[nodiscard]] FieldValue scalarValue(const TypedValue& value, const FieldDescriptor& field);

/// The shape of a saved reference slot.
enum class SlotShape : std::uint8_t
{
    String,      ///< One string property, e.g. `booster`.
    StringArray, ///< A `[Ljava.lang.String;` array of `[n]` elements; typed nulls are empty slots.
    Collection,  ///< A `java.util.ArrayList` of string items.
};

/// A reference slot of an untyped owner class, from the audit behind the source reference index
/// (src/tracker_references.hpp), or the panel's `referenceframe`. Each saved name binds through `NameIndex` with
/// the field's filter at load and becomes one graph dependency per slot.
struct ReferenceDescriptor
{
    std::string_view id{};
    /// Owner classes that store the field.
    std::span<const std::string_view> ownerClasses{};
    std::string_view property{};
    SlotShape shape = SlotShape::String;
    /// The largest number of slots the field can hold (DynamicSystem has two particles).
    std::uint32_t maxSlots = 10000;
    /// Whether array indices must be exactly 0 to n-1 (DynamicSystem.particles); otherwise gaps are empty slots.
    bool contiguous = false;
    DependencyKind kind = DependencyKind::MemberOf;
    Facet ownerFacet = Facet::Geometry;
    Facet targetFacet = Facet::Geometry;
    MissingTarget onMissing = MissingTarget::Blocks;
    /// The first dependency slot; fields of one kind on one owner use disjoint ranges.
    std::uint32_t slotBase = 0;
    /// Tracker's type rule for the target, as the source reference index applies it.
    CandidateFilter filter{};
};

/// Every reference-only descriptor: CenterOfMass.masses, VectorSum.vectors, the tools' attachments,
/// CircleFitter.step_attachment, the models' booster, DynamicSystem.particles and TrackerPanel.referenceframe.
[[nodiscard]] std::span<const ReferenceDescriptor> referenceDescriptors();

/// The saved names of one reference field, in slot order; an empty slot (typed null) is absent. Nothing when the
/// owner does not save the field, and `malformed` when it saves it in a shape Tracker's loader would not read the
/// same way, which the source reference index also reports.
struct SavedReferences
{
    std::vector<std::optional<std::string>> names{};
    bool malformed = false;
};
[[nodiscard]] std::optional<SavedReferences> readReferences(const osp::Source& source, osp::NodeId owner, const ReferenceDescriptor& field);

/// TTrack's saved `name` as the source reference index reads it: one string property of at most 4096 UTF-16 units,
/// otherwise none (an object without a readable name, which no saved name can refer to).
[[nodiscard]] std::optional<std::string> savedLabel(const osp::Source& source, osp::NodeId object);

/// Tracker's saver tree for an object's typed fields, in its saver order, with the values its loaded object holds.
/// Point masses: `mass`, `name`, `framedata` and `keyFrames` (as PointMass.Loader fills them). Vectors:
/// `name`, `framedata`. Other families return their scalar fields that hold a value; Unknown objects return none.
[[nodiscard]] std::vector<osp::PropertyValue> saverProperties(const TypedValue& value);

} // namespace motrack::model
