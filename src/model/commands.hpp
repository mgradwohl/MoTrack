#pragma once

#include "model/dependencies/dependency_graph.hpp"
#include "model/families.hpp"
#include "model/object_id.hpp"
#include "model/tracker_document.hpp"
#include "osp/java_text.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

/// ID-addressed commands on a typed document (docs/tracker-document.md, "ID-addressed commands"). A command is a value
/// that addresses objects by identity, fields by descriptor id and frame-keyed elements by Tracker's loaded frame. It is
/// validated completely against a document and builds the document it produces on copies (`prepare`), so that the
/// owner's history (`DocumentHistory`) commits it with non-throwing swaps or leaves everything unchanged. Commands never
/// touch XML; standard export composes edits from the typed state. Standard-only interface; owner thread only.
namespace motrack::model
{

/// A value a `SetField` compares or writes. A scalar field holds its accessor result: a value of the descriptor's
/// type, or the `osp::DecodeError` that the property reads as when the file does not save it (`Missing`) or saves an
/// unreadable one. An element of a frame-keyed field holds a `FrameElement`, whose baseline node a restoration carries;
/// `std::monostate` is an absent element.
using CommandValue = std::variant<std::monostate,
                                  osp::DecodeError,
                                  bool,
                                  std::int32_t,
                                  double,
                                  std::string,
                                  std::vector<std::string>,
                                  std::vector<std::int32_t>,
                                  FrameElement<Mark>,
                                  FrameElement<VectorSample>,
                                  FrameElement<CoordKey>>;

/// Whether two values are the same, with NaN equal to itself and frame elements compared by value, never by node.
[[nodiscard]] bool sameValue(const CommandValue& left, const CommandValue& right);

/// Sets one field of a live object, or one element of a frame-keyed field. `before` must equal the current value, so a
/// command built from stale state is refused; `after` replaces it (an absent element removes it). A new element keeps
/// the node of the element it replaces, and its `after` names no node of its own.
struct SetField
{
    ObjectId object{};
    std::string field{}; ///< The descriptor's id.
    /// Tracker's loaded (original) frame of a frame-keyed field's element; absent for a scalar field.
    std::optional<std::int32_t> frame{};
    CommandValue before{};
    CommandValue after{};

    bool operator==(const SetField&) const = default;
};

/// A dependency that an inserted object holds, added with `CyclePolicy::Reject`.
struct OwnedDependency
{
    Facet facet = Facet::Geometry;
    Dependency dependency{};

    bool operator==(const OwnedDependency&) const = default;
};

/// Inserts a complete, live object: an element of the panel's `tracks` or a singleton other than the panel. A new
/// object is unbound, takes the fresh identity of its range (`freshIdentity`) and adds its own dependencies; a
/// restoration (the inverse of a `Remove`, or the redo of a creation) brings back a removed identity with what its
/// removal took out of the graph, its binding and its reference issues.
struct Insert
{
    ObjectSlot object{};
    /// The object's index in the document's objects; ignored when a bound object returns to the slot it kept.
    std::size_t position = 0;
    /// The index in the track list (`trackOrder`) of a point mass or vector track; absent for anything else.
    std::optional<std::size_t> trackPosition{};
    /// New objects only.
    std::vector<OwnedDependency> dependencies{};
    /// Restorations only: what `DependencyGraph::removeObject` returned.
    std::optional<RemovedObject> restores{};
    /// Restorations only: the reference issues the removal took out of the document, in document order.
    std::vector<ReferenceIssue> issues{};

    bool operator==(const Insert&) const = default;
};

/// Where an object goes back into the document: its index in the objects and, for a point mass or vector, in the track
/// list.
struct Placement
{
    std::size_t position = 0;
    std::optional<std::size_t> trackPosition{};

    bool operator==(const Placement&) const = default;
};

/// Removes a live object: an element of `tracks`, or a singleton MoTrack inserted. A bound object keeps its slot and
/// binding in the `Removed` state, so that export removes exactly its element and undo restores the same node; an
/// unbound one leaves the document, its identity kept from reuse by the allocators. References to it dangle.
struct Remove
{
    ObjectId object{};
    /// Where the restoration that undoes this removal puts an unbound object back; absent: where it was. The inverse of
    /// an `Insert` keeps the insertion's placement, so every undo and redo of one history entry reinserts the object
    /// where the entry was recorded, as TrackEdits does, whatever reordered the tracks in between.
    std::optional<Placement> restoreAt{};

    bool operator==(const Remove&) const = default;
};

struct Batch;

using Command = std::variant<SetField, Insert, Remove, Batch>;

/// Ordered commands recorded as one history entry: each is validated against the state the previous ones produce,
/// and the batch commits once or not at all. Its inverse is the members' inverses in reverse order.
struct Batch
{
    std::vector<Command> commands{};
};

[[nodiscard]] bool operator==(const Batch& left, const Batch& right);

/// How a command applies. New commands come from the user: their values pass the descriptors' domain checks, a command
/// that changes nothing is refused, and new dependencies refuse cycles. Undo and redo replay inverses that restore an
/// earlier state, with `CyclePolicy::Record` and without the domain checks.
enum class ApplyMode : std::uint8_t
{
    New,
    Undo,
    Redo,
};

struct ApplyOptions
{
    ApplyMode mode = ApplyMode::New;
    /// For an undo: the clip timeline before the command was first applied. An undo that restores exactly it brings
    /// back a state that existed, so the constant-timing check does not refuse it (a legacy timing kept as it was).
    std::optional<ClipTimeline> restoresTimeline{};
};

enum class CommandErrorCode : std::uint8_t
{
    UnknownObject,   ///< The target is not a live object of the document.
    UnknownField,    ///< The object's family has no such field, or no command edits it.
    InvalidElement,  ///< A frame-keyed field without an element key, a scalar field with one, or a frame out of range.
    StaleValue,      ///< `before` differs from the current value.
    InvalidValue,    ///< A value fails the field's domain check or has the wrong type.
    NoChange,        ///< A new command that changes nothing, or an empty batch.
    InvalidIdentity, ///< An inserted identity that is not fresh, or a restored one outside its allocated range.
    InvalidObject,   ///< An inserted object of the wrong state, class, role or binding, or a duplicate singleton.
    InvalidPosition, ///< An insertion position past the end, or a track position given or missing wrongly.
    NotRemovable,    ///< The panel, or a singleton bound to the baseline.
    TimingInvariant, ///< The clip's constant timing or its timing-provider guard would not hold.
    GraphRefused,    ///< The dependency graph refused a change, for example a new edge that closes a cycle.
    NothingToUndo,   ///< The history has no entry in that direction.
};

/// English diagnostic text for logs and tests; user interfaces translate by code.
[[nodiscard]] std::string_view commandErrorName(CommandErrorCode code) noexcept;

struct CommandError
{
    CommandErrorCode code = CommandErrorCode::UnknownObject;
    std::string message{};

    bool operator==(const CommandError&) const = default;
};

/// A validated command: the document it produces, built on copies that share everything it does not change, and the
/// exact command that undoes it.
// Holds a TrackerDocument, whose implicit move constructor can throw with MSVC's STL (see there).
struct PreparedCommand // NOLINT(bugprone-exception-escape)
{
    TrackerDocument document;
    Command inverse;
};

/// Validates `command` completely against `document` and builds the document it produces, without changing
/// `document`. A refused command returns why. Throws only std::bad_alloc.
[[nodiscard]] std::expected<PreparedCommand, CommandError>
prepare(const TrackerDocument& document, const Command& command, const ApplyOptions& options = {});

/// `document` with every node of its graph marked changed at max(its graph's revision + 1, `floor`), on a copy of the
/// graph: what an owner assigns in place of a live document whose graph revision is `floor` - 1.
[[nodiscard]] TrackerDocument withRevisionFloor(TrackerDocument document, Revision floor);

/// The identity a new object of `range` takes: the counter (when it stays below the countdown low-water mark), or
/// the highest unused value below the low-water mark and at or above the counter. Nothing when the range is exhausted.
[[nodiscard]] std::optional<ObjectId> freshIdentity(const TrackerDocument& document, AllocationRange range);

/// The current value of a field, or of one element of a frame-keyed field (`std::monostate` when absent); nothing when
/// the family has no such field or the element key does not fit its kind.
[[nodiscard]] std::optional<CommandValue>
fieldValue(const TypedValue& value, std::string_view field, std::optional<std::int32_t> frame = std::nullopt);
/// Writes a field or element as `SetField` does, without checks beyond the value's type; false when it does not fit.
[[nodiscard]] bool setFieldValue(TypedValue& value, std::string_view field, std::optional<std::int32_t> frame, const CommandValue& written);

/// A new point mass or vector named `name`, appended after the last track of its family, with Tracker's default mass
/// for a point mass. Nothing when the counter is exhausted or the family is neither.
[[nodiscard]] std::optional<Insert> createTrack(const TrackerDocument& document, Family family, std::string name);
/// A new unbound singleton of `role` (the clip, clip control or coordinate system) holding `value`, at the end of the
/// document's objects. Nothing when the countdown is exhausted.
[[nodiscard]] std::optional<Insert> createSingleton(const TrackerDocument& document, Role role, TypedValue value);
/// Sets or removes (`mark` absent) the mark of a point mass at a loaded frame, with the key frames B1's export writes:
/// for a bound point mass, the baseline's keys without its marks that are gone and with every mark that differs from the
/// baseline's; for a created one, every mark. One `SetField`, or a `Batch` with the key frames when they change.
[[nodiscard]] std::expected<Command, CommandError>
setMark(const TrackerDocument& document, ObjectId pointMass, std::int32_t frame, std::optional<Mark> mark);

} // namespace motrack::model
