#include "model/commands.hpp"

#include "model/codecs.hpp"
#include "model/descriptors.hpp"
#include "model/tracker_loaders.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <ranges>
#include <set>
#include <type_traits>
#include <utility>

namespace motrack::model
{

/// The only writer of a document's parts and derived state (TrackerDocument befriends it).
class DocumentEditor
{
  public:
    static TrackerDocument make(DocumentParts parts, std::shared_ptr<const DependencyGraph> graph, std::vector<ReferenceIssue> issues)
    {
        TrackerDocument document(std::move(parts));
        document.m_Graph = std::move(graph);
        document.m_ReferenceIssues = std::move(issues);
        const auto& objects = document.m_Parts.objects;
        document.m_Index.reserve(objects.size());
        for (std::size_t index = 0; index < objects.size(); ++index)
        {
            document.m_Index.emplace(objects[index].id, index);
        }
        return document;
    }

    static void setGraph(TrackerDocument& document, std::shared_ptr<const DependencyGraph> graph) noexcept
    {
        document.m_Graph = std::move(graph);
    }
};

static_assert(std::is_nothrow_move_constructible_v<Command> && std::is_nothrow_move_assignable_v<Command>,
              "A history replaces its entries' commands after committing, where nothing may throw.");

namespace
{

std::unexpected<CommandError> refuse(CommandErrorCode code, std::string message)
{
    return std::unexpected(CommandError{.code = code, .message = std::move(message)});
}

template <typename T> CommandValue fromRead(const Read<T>& value)
{
    return value ? CommandValue(*value) : CommandValue(value.error());
}

template <typename T> bool toRead(Read<T>& field, const CommandValue& value)
{
    if (const auto* item = std::get_if<T>(&value))
    {
        field = *item;
        return true;
    }
    if (const auto* error = std::get_if<osp::DecodeError>(&value))
    {
        field = osp::makeUnexpected(*error);
        return true;
    }
    return false;
}

/// Calls `visit` with the `Read` field of a family state named by its descriptor id; false when there is none.
template <typename State, typename Visit> bool scalarField(State& state, std::string_view id, Visit&& visit)
{
    const auto match = [&](std::string_view name, auto& field)
    {
        if (id != name)
        {
            return false;
        }
        visit(field);
        return true;
    };
    using Plain = std::remove_const_t<State>;
    if constexpr (std::is_same_v<Plain, PanelState>)
    {
        return match("semantic_version", state.semanticVersion) || match("description", state.description) ||
               match("time_unit", state.timeUnit) || match("length_unit", state.lengthUnit) || match("mass_unit", state.massUnit) ||
               match("referenceframe", state.referenceFrame) || match("selectedtrack", state.selectedTrack);
    }
    else if constexpr (std::is_same_v<Plain, ClipState>)
    {
        return match("video_framecount", state.frameCount) || match("startframe", state.startFrame) || match("stepsize", state.stepSize) ||
               match("stepcount", state.stepCount) || match("starttime", state.startTime) || match("frameshift", state.frameShift);
    }
    else if constexpr (std::is_same_v<Plain, VideoState>)
    {
        return match("path", state.path) || match("delta_t", state.deltaT);
    }
    else if constexpr (std::is_same_v<Plain, ClipControlState>)
    {
        return match("rate", state.rate) || match("delta_t", state.deltaT) || match("frame", state.frame) ||
               match("looping", state.looping);
    }
    else if constexpr (std::is_same_v<Plain, CoordinatesState>)
    {
        return match("fixedorigin", state.fixedOrigin) || match("fixedangle", state.fixedAngle) || match("fixedscale", state.fixedScale) ||
               match("locked", state.locked);
    }
    else if constexpr (std::is_same_v<Plain, PointMassState>)
    {
        return match("mass", state.mass) || match("name", state.name) || match("keyFrames", state.keyFrames);
    }
    else if constexpr (std::is_same_v<Plain, VectorState>)
    {
        return match("name", state.name);
    }
    else
    {
        return false;
    }
}

/// Calls `visit` with the frame-keyed series of a family state; false when the family has none.
template <typename State, typename Visit> bool seriesField(State& state, Visit&& visit)
{
    using Plain = std::remove_const_t<State>;
    if constexpr (std::is_same_v<Plain, PointMassState>)
    {
        visit(state.marks);
        return true;
    }
    else if constexpr (std::is_same_v<Plain, VectorState>)
    {
        visit(state.samples);
        return true;
    }
    else if constexpr (std::is_same_v<Plain, CoordinatesState>)
    {
        visit(state.keys);
        return true;
    }
    else
    {
        return false;
    }
}

bool finite(const Mark& mark)
{
    return std::isfinite(mark.x) && std::isfinite(mark.y);
}

bool finite(const VectorSample& sample)
{
    // The tip is drawn and exported too, so it must stay finite.
    return std::isfinite(sample.xTail) && std::isfinite(sample.yTail) && std::isfinite(sample.xComponent) &&
           std::isfinite(sample.yComponent) && std::isfinite(sample.xTail + sample.xComponent) &&
           std::isfinite(sample.yTail + sample.yComponent);
}

bool finite(const CoordKey& key)
{
    // A zero scale cannot be inverted into world coordinates.
    return std::isfinite(key.xOrigin) && std::isfinite(key.yOrigin) && std::isfinite(key.angle) && std::isfinite(key.xScale) &&
           std::isfinite(key.yScale) && key.xScale != 0.0 && key.yScale != 0.0;
}

/// A frame-keyed element as a new command writes it: a finite value without a node of its own (it keeps the node of the
/// element it replaces), or nothing.
template <typename T> bool validElement(Family family, const CommandValue& value)
{
    if (std::holds_alternative<std::monostate>(value))
    {
        return true;
    }
    const auto* element = std::get_if<FrameElement<T>>(&value);
    const bool fits = (family == Family::PointMass && std::is_same_v<T, Mark>) ||
                      (family == Family::Vector && std::is_same_v<T, VectorSample>) ||
                      (family == Family::Coordinates && std::is_same_v<T, CoordKey>);
    return fits && element != nullptr && !element->node && finite(element->value);
}

bool validKeyFrames(const CommandValue& value)
{
    // A key list that equals the baseline's reading is restored as that reading, which may be unsaved or unreadable.
    if (std::holds_alternative<osp::DecodeError>(value))
    {
        return true;
    }
    const auto* keys = std::get_if<std::vector<std::int32_t>>(&value);
    return keys != nullptr && std::ranges::all_of(*keys, [](std::int32_t frame) { return frame >= 0; }) &&
           std::ranges::adjacent_find(*keys, std::greater_equal<>{}) == keys->end();
}

/// The descriptor's domain check for a value a new command writes.
bool validNewValue(Family family, const FieldDescriptor& field, const CommandValue& value)
{
    if (field.kind == FieldKind::FrameKeyed)
    {
        return validElement<Mark>(family, value) || validElement<VectorSample>(family, value) || validElement<CoordKey>(family, value);
    }
    const auto id = field.id;
    if (id == "keyFrames")
    {
        return validKeyFrames(value);
    }
    if (const auto* text = std::get_if<std::string>(&value))
    {
        // The unit and name codecs, which also keep every string representable in OSP XML (#194).
        if (field.label)
        {
            return isTrackerTrackName(*text);
        }
        return (id == "time_unit" || id == "length_unit" || id == "mass_unit") && isTrackerUnitLabel(*text);
    }
    if (const auto* number = std::get_if<std::int32_t>(&value))
    {
        // Tracker's frame bounds: a clip starts at or after frame 0 and has at least one step of at least one frame.
        return (id == "startframe" && *number >= 0) || ((id == "stepsize" || id == "stepcount") && *number >= 1);
    }
    if (const auto* number = std::get_if<double>(&value))
    {
        if (!std::isfinite(*number))
        {
            return false;
        }
        return id == "starttime" || (id == "delta_t" && *number > 0.0) || (id == "mass" && *number >= MINIMUM_POINT_MASS);
    }
    return false;
}

/// The class a new object of a creatable family takes, and the role it must have.
struct Creatable
{
    std::string_view className;
    Role role;
};

std::optional<Creatable> creatable(Family family)
{
    switch (family)
    {
    case Family::Clip:
        return Creatable{.className = tracker::VIDEO_CLIP_CLASS, .role = Role::Clip};
    case Family::ClipControl:
        return Creatable{.className = tracker::STEPPER_CLIP_CONTROL_CLASS, .role = Role::ClipControl};
    case Family::Coordinates:
        return Creatable{.className = tracker::COORD_SYSTEM_CLASS, .role = Role::Coordinates};
    case Family::PointMass:
        return Creatable{.className = tracker::POINT_MASS_CLASS, .role = Role::Track};
    case Family::Vector:
        return Creatable{.className = tracker::VECTOR_CLASS, .role = Role::Track};
    case Family::Panel:
    case Family::Video:
    case Family::Unknown:
        break;
    }
    return std::nullopt;
}

/// Whether an inserted object belongs to its role: never the panel; Unknown objects anywhere else.
bool fitsRole(const ObjectSlot& object)
{
    const auto family = object.family();
    if (object.role == Role::Panel || family == Family::Panel)
    {
        return false;
    }
    if (family == Family::Unknown)
    {
        return true;
    }
    if (family == Family::Video)
    {
        return object.role == Role::Video;
    }
    const auto kind = creatable(family);
    return kind && kind->role == object.role;
}

bool typedTrack(const ObjectSlot& object)
{
    return object.role == Role::Track && (object.family() == Family::PointMass || object.family() == Family::Vector);
}

/// The working copy a command edits: the document's parts, its graph (copied on the first change) and its issues.
struct Working
{
    explicit Working(const TrackerDocument& document)
        : parts(document.parts()), issues(document.referenceIssues().begin(), document.referenceIssues().end()),
          shared(document.sharedGraph())
    {}

    DocumentParts parts;
    std::vector<ReferenceIssue> issues;
    /// The document's graph until the first change, then a copy of it.
    std::shared_ptr<const DependencyGraph> shared;
    std::shared_ptr<DependencyGraph> copy{};

    DependencyGraph& editGraph()
    {
        if (!copy)
        {
            copy = std::make_shared<DependencyGraph>(*shared);
        }
        return *copy;
    }

    [[nodiscard]] std::shared_ptr<const DependencyGraph> takeGraph()
    {
        return copy ? std::shared_ptr<const DependencyGraph>(std::move(copy)) : shared;
    }

    [[nodiscard]] std::optional<std::size_t> indexOf(ObjectId id) const
    {
        const auto found = std::ranges::find(parts.objects, id, &ObjectSlot::id);
        return found == parts.objects.end() ? std::nullopt : std::optional(static_cast<std::size_t>(found - parts.objects.begin()));
    }

    [[nodiscard]] ObjectSlot* live(ObjectId id)
    {
        const auto index = indexOf(id);
        return index && parts.objects[*index].state == ObjectState::Live ? &parts.objects[*index] : nullptr;
    }
};

std::optional<ObjectId> fresh(const DocumentParts& parts, AllocationRange range)
{
    const auto next = parts.nextObjectId;
    const auto lowWater = parts.countdownLowWater;
    if (range == AllocationRange::Counter)
    {
        return next > 0 && next < lowWater ? std::optional(ObjectId{next}) : std::nullopt;
    }
    // Every counter identity lies below the counter and every countdown identity at or above the low-water mark, so
    // the value just below the mark is unused while it stays at or above the counter.
    return lowWater > 1 && lowWater - 1 >= next ? std::optional(ObjectId{lowWater - 1}) : std::nullopt;
}

std::vector<std::string> unknownTrackClasses(std::span<const ObjectSlot> objects)
{
    std::vector<std::string> classes;
    for (const auto& object : objects)
    {
        if (object.role == Role::Track && object.state == ObjectState::Live && object.family() == Family::Unknown &&
            !object.className.empty())
        {
            classes.push_back(object.className);
        }
    }
    return classes;
}

std::expected<Command, CommandError> applyCommand(Working& working, const Command& command, ApplyMode mode);

std::expected<Command, CommandError> applySet(Working& working, const SetField& set, ApplyMode mode)
{
    auto* object = working.live(set.object);
    if (object == nullptr)
    {
        return refuse(CommandErrorCode::UnknownObject, "The command's object is not a live object of the document.");
    }
    const auto family = object->family();
    const auto* field = findField(family, set.field);
    if (field == nullptr || !field->commandEdits)
    {
        return refuse(CommandErrorCode::UnknownField,
                      "No command edits the field '" + set.field + "' of a " + std::string(familyName(family)) + ".");
    }
    if ((field->kind == FieldKind::FrameKeyed) != set.frame.has_value() || (mode == ApplyMode::New && set.frame && *set.frame < 0))
    {
        return refuse(CommandErrorCode::InvalidElement, "The element key does not fit the field '" + set.field + "'.");
    }
    const auto current = fieldValue(*object->value, set.field, set.frame).value_or(CommandValue{});
    if (!sameValue(current, set.before))
    {
        return refuse(CommandErrorCode::StaleValue, "The field '" + set.field + "' no longer holds the command's expected value.");
    }
    auto written = set.after;
    if (mode == ApplyMode::New)
    {
        if (!validNewValue(family, *field, written))
        {
            return refuse(CommandErrorCode::InvalidValue, "The value cannot be stored in the field '" + set.field + "'.");
        }
        if (sameValue(current, written))
        {
            return refuse(CommandErrorCode::NoChange, "The command does not change the field '" + set.field + "'.");
        }
        // A replaced element keeps its frame object, so unknown properties on it survive the edit.
        std::visit(
            [&](auto& element)
            {
                using Item = std::decay_t<decltype(element)>;
                if constexpr (requires { element.node; })
                {
                    if (const auto* previous = std::get_if<Item>(&current))
                    {
                        element.node = previous->node;
                    }
                }
            },
            written);
    }
    auto value = *object->value;
    if (!setFieldValue(value, set.field, set.frame, written))
    {
        return refuse(CommandErrorCode::InvalidValue, "The value has the wrong type for the field '" + set.field + "'.");
    }
    object->value = std::make_shared<const TypedValue>(std::move(value));
    if (field->label)
    {
        const auto* name = std::get_if<std::string>(&written);
        if (!working.editGraph().renameObject(set.object, name ? std::optional(*name) : std::nullopt))
        {
            return refuse(CommandErrorCode::GraphRefused, "The dependency graph does not hold the object.");
        }
    }
    if (field->facet && !working.editGraph().markChanged({.object = set.object, .facet = *field->facet}))
    {
        return refuse(CommandErrorCode::GraphRefused, "The dependency graph does not hold the object.");
    }
    return SetField{.object = set.object, .field = set.field, .frame = set.frame, .before = std::move(written), .after = current};
}

std::expected<Command, CommandError> applyRemove(Working& working, const Remove& remove)
{
    const auto index = working.indexOf(remove.object);
    if (!index || working.parts.objects[*index].state != ObjectState::Live)
    {
        return refuse(CommandErrorCode::UnknownObject, "The command's object is not a live object of the document.");
    }
    auto& objects = working.parts.objects;
    const auto& object = objects[*index];
    if (object.role == Role::Panel || (object.role != Role::Track && object.binding))
    {
        return refuse(CommandErrorCode::NotRemovable, "Only tracks and singletons MoTrack inserted can be removed.");
    }
    auto removed = working.editGraph().removeObject(object.id);
    if (!removed)
    {
        return refuse(CommandErrorCode::GraphRefused, std::string(errorMessage(removed.error().code)));
    }
    Insert inverse{
        .object = object, .position = *index, .trackPosition = std::nullopt, .dependencies = {}, .restores = *std::move(removed)};
    const auto owned = std::ranges::stable_partition(working.issues, [&](const ReferenceIssue& issue) { return issue.owner != object.id; });
    inverse.issues.assign(owned.begin(), owned.end());
    working.issues.erase(owned.begin(), owned.end());
    auto& order = working.parts.trackOrder;
    if (const auto found = std::ranges::find(order, object.id); found != order.end())
    {
        inverse.trackPosition = static_cast<std::size_t>(found - order.begin());
        order.erase(found);
    }
    // A bound object returns to the slot it keeps; an unbound one where the history entry was recorded.
    if (remove.restoreAt && !object.binding)
    {
        inverse.position = remove.restoreAt->position;
        inverse.trackPosition = remove.restoreAt->trackPosition;
    }
    if (object.binding)
    {
        objects[*index].state = ObjectState::Removed;
    }
    else
    {
        objects.erase(objects.begin() + static_cast<std::ptrdiff_t>(*index));
    }
    return inverse;
}

/// Checks an inserted object's identity, class and values, and adds it to the graph; restorations are handled apart.
std::expected<void, CommandError> addNewObject(Working& working, const Insert& insert, ApplyMode mode)
{
    const auto& object = insert.object;
    const auto family = object.family();
    const auto kind = creatable(family);
    if (!kind || object.className != kind->className || object.binding || !insert.issues.empty())
    {
        return refuse(CommandErrorCode::InvalidObject, "Commands create only unbound clips, clip controls, coordinate systems and tracks.");
    }
    const auto range = object.role == Role::Track ? AllocationRange::Counter : AllocationRange::Countdown;
    if (object.range != range || fresh(working.parts, range) != object.id)
    {
        return refuse(CommandErrorCode::InvalidIdentity, "A new object takes the fresh identity of its allocation range.");
    }
    if (mode == ApplyMode::New)
    {
        for (const auto& field : fieldDescriptors(family))
        {
            if (field.kind == FieldKind::FrameKeyed)
            {
                bool valid = true;
                std::visit(
                    [&](const auto& state)
                    {
                        seriesField(state,
                                    [&](const auto& series)
                                    {
                                        series.forEach([&](std::int32_t frame, const auto& element)
                                                       { valid = valid && frame >= 0 && !element.node && finite(element.value); });
                                    });
                    },
                    *object.value);
                if (!valid)
                {
                    return refuse(CommandErrorCode::InvalidValue,
                                  "A new object's elements must be finite, at loaded frames, without nodes.");
                }
                continue;
            }
            const auto value = fieldValue(*object.value, field.id).value_or(CommandValue{});
            const bool missing =
                std::get_if<osp::DecodeError>(&value) != nullptr && std::get<osp::DecodeError>(value) == osp::DecodeError::Missing;
            if (field.commandEdits && !(missing && !field.label) && !validNewValue(family, field, value))
            {
                return refuse(CommandErrorCode::InvalidValue,
                              "A new object's field '" + std::string(field.id) + "' fails its domain check.");
            }
        }
    }
    auto& graph = working.editGraph();
    if (auto added = graph.addObject(object.id, {.label = graphLabel(object), .className = object.className}); !added)
    {
        return refuse(CommandErrorCode::GraphRefused, std::string(errorMessage(added.error().code)));
    }
    for (const auto& owned : insert.dependencies)
    {
        const auto policy = mode == ApplyMode::New ? CyclePolicy::Reject : CyclePolicy::Record;
        if (auto added = graph.addDependency({.object = object.id, .facet = owned.facet}, owned.dependency, policy); !added)
        {
            auto message = std::string(errorMessage(added.error().code));
            if (!added.error().cycle.edges.empty())
            {
                message += " " + describeCycle(graph, added.error().cycle);
            }
            return refuse(CommandErrorCode::GraphRefused, std::move(message));
        }
    }
    if (range == AllocationRange::Counter)
    {
        working.parts.nextObjectId = object.id.value + 1;
    }
    else
    {
        working.parts.countdownLowWater = object.id.value;
    }
    working.parts.objects.insert(working.parts.objects.begin() + static_cast<std::ptrdiff_t>(insert.position), object);
    return {};
}

std::expected<void, CommandError> restoreObject(Working& working, const Insert& insert, const RemovedObject& removed)
{
    const auto& object = insert.object;
    const auto& parts = working.parts;
    if (removed.id != object.id || !insert.dependencies.empty())
    {
        return refuse(CommandErrorCode::InvalidObject, "A restoration carries its own removal record.");
    }
    if (object.range == AllocationRange::Counter ? object.id.value >= parts.nextObjectId : object.id.value < parts.countdownLowWater)
    {
        return refuse(CommandErrorCode::InvalidIdentity, "A restored identity lies outside its allocated range.");
    }
    const auto existing = working.indexOf(object.id);
    if (existing)
    {
        // A bound object returns to the slot it kept, wherever that is now.
        const auto& slot = parts.objects[*existing];
        if (slot.binding != object.binding || slot.className != object.className || slot.role != object.role || slot.range != object.range)
        {
            return refuse(CommandErrorCode::InvalidObject, "A removed object returns only to the slot it kept.");
        }
    }
    else if (object.binding)
    {
        return refuse(CommandErrorCode::InvalidObject, "A bound object keeps its slot while it is removed.");
    }
    auto& graph = working.editGraph();
    // A graph rebuilt from the document after an update no longer lists an unbound identity that left the document;
    // it was allocated, so it returns as retired.
    if (!graph.contains(object.id) && !graph.wasRemoved(object.id) && !graph.retireIdentity(object.id))
    {
        return refuse(CommandErrorCode::GraphRefused, "The identity cannot be retired.");
    }
    if (auto restored = graph.restoreObject(removed, CyclePolicy::Record); !restored)
    {
        return refuse(CommandErrorCode::GraphRefused, std::string(errorMessage(restored.error().code)));
    }
    auto& objects = working.parts.objects;
    const auto index = existing.value_or(insert.position);
    if (existing)
    {
        objects[index] = object;
    }
    else
    {
        objects.insert(objects.begin() + static_cast<std::ptrdiff_t>(index), object);
    }
    // The owner's issues return to where document order puts them.
    const auto ownerIndex = [&](ObjectId owner)
    {
        const auto found = std::ranges::find(objects, owner, &ObjectSlot::id);
        return static_cast<std::size_t>(found - objects.begin());
    };
    const auto at = std::ranges::find_if(working.issues, [&](const ReferenceIssue& issue) { return ownerIndex(issue.owner) > index; });
    working.issues.insert(at, insert.issues.begin(), insert.issues.end());
    return {};
}

std::expected<Command, CommandError> applyInsert(Working& working, const Insert& insert, ApplyMode mode)
{
    const auto& object = insert.object;
    if (!object.value || object.state != ObjectState::Live || !object.id.isValid() || !fitsRole(object))
    {
        return refuse(CommandErrorCode::InvalidObject, "An inserted object is a live object of its role other than the panel.");
    }
    if (working.live(object.id) != nullptr)
    {
        return refuse(CommandErrorCode::InvalidIdentity, "The inserted identity is already live.");
    }
    const auto& parts = working.parts;
    if (typedTrack(object) != insert.trackPosition.has_value() ||
        (insert.trackPosition && *insert.trackPosition > parts.trackOrder.size()) ||
        (!working.indexOf(object.id) && insert.position > parts.objects.size()))
    {
        return refuse(CommandErrorCode::InvalidPosition, "The insertion position is past the end, or a track position is wrong.");
    }
    if (object.role != Role::Track && std::ranges::any_of(parts.objects,
                                                          [&](const ObjectSlot& slot)
                                                          {
                                                              return slot.role == object.role && slot.state == ObjectState::Live &&
                                                                     (slot.family() == Family::Unknown) ==
                                                                         (object.family() == Family::Unknown);
                                                          }))
    {
        return refuse(CommandErrorCode::InvalidObject, "The document already has this singleton.");
    }
    if (insert.restores && mode == ApplyMode::New)
    {
        return refuse(CommandErrorCode::InvalidObject, "Only undo and redo restore a removed object.");
    }
    if (auto added = insert.restores ? restoreObject(working, insert, *insert.restores) : addNewObject(working, insert, mode); !added)
    {
        return std::unexpected(std::move(added.error()));
    }
    if (insert.trackPosition)
    {
        auto& order = working.parts.trackOrder;
        order.insert(order.begin() + static_cast<std::ptrdiff_t>(*insert.trackPosition), object.id);
    }
    return Remove{.object = object.id, .restoreAt = Placement{.position = insert.position, .trackPosition = insert.trackPosition}};
}

std::expected<Command, CommandError> applyBatch(Working& working, const Batch& batch, ApplyMode mode)
{
    if (batch.commands.empty() && mode == ApplyMode::New)
    {
        return refuse(CommandErrorCode::NoChange, "An empty batch changes nothing.");
    }
    Batch inverse;
    inverse.commands.reserve(batch.commands.size());
    for (const auto& member : batch.commands)
    {
        auto undone = applyCommand(working, member, mode);
        if (!undone)
        {
            return undone;
        }
        inverse.commands.push_back(*std::move(undone));
    }
    std::ranges::reverse(inverse.commands);
    return inverse;
}

std::expected<Command, CommandError> applyCommand(Working& working, const Command& command, ApplyMode mode)
{
    return std::visit(
        [&](const auto& item) -> std::expected<Command, CommandError>
        {
            using Item = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<Item, SetField>)
            {
                return applySet(working, item, mode);
            }
            else if constexpr (std::is_same_v<Item, Insert>)
            {
                return applyInsert(working, item, mode);
            }
            else if constexpr (std::is_same_v<Item, Remove>)
            {
                return applyRemove(working, item);
            }
            else
            {
                return applyBatch(working, item, mode);
            }
        },
        command);
}

/// The clip family's invariants for the state a command produces: a command that changes the timing needs a media-free
/// experiment without other time providers, and a media-free timed clip keeps constant timing, unless an undo restores
/// exactly the timeline the command started from. A new command also keeps the clip within the source frames.
std::expected<void, CommandError> checkClip(const TrackerDocument& before, const Working& working, const ApplyOptions& options)
{
    const auto previous = clipTimeline(before);
    const auto next = clipTimeline(working.parts.objects, working.parts.media);
    if (next == previous)
    {
        return {};
    }
    const bool timing = next.frameDurationMs != previous.frameDurationMs || next.startTimeMs != previous.startTimeMs;
    if (timing && !supportsConstantClipTimingEdits(working.parts.media.references, unknownTrackClasses(working.parts.objects)))
    {
        return refuse(CommandErrorCode::TimingInvariant,
                      "Constant clip timing edits require a media-free experiment without other time providers.");
    }
    const bool restoring = options.mode == ApplyMode::Undo && options.restoresTimeline == next;
    if (!next.media && next.frameDurationMs && !restoring && !validConstantClipTiming(next))
    {
        return refuse(CommandErrorCode::TimingInvariant, "The clip would not keep constant timing that Tracker can load and save.");
    }
    if (options.mode == ApplyMode::New && next.stepCount && next.sourceFrameCount &&
        std::int64_t{next.firstFrame} + ((std::int64_t{*next.stepCount} - 1) * next.stepSize) >= *next.sourceFrameCount)
    {
        return refuse(CommandErrorCode::TimingInvariant, "The clip would extend past the source frames.");
    }
    return {};
}

/// The key frames B1's mark export writes for a point mass whose marks become `marks`.
Read<std::vector<std::int32_t>> keyFramesFor(const TrackerDocument& document, ObjectId id, const FrameSeries<Mark>& marks)
{
    if (const auto baseline = baselinePointMass(document, id))
    {
        const auto baselineKeys = loadedKeyFrames(*baseline);
        std::set<std::int32_t> keys(baselineKeys.begin(), baselineKeys.end());
        baseline->marks.forEach(
            [&](std::int32_t frame, const FrameElement<Mark>& /*mark*/)
            {
                if (marks.find(frame) == nullptr)
                {
                    keys.erase(frame);
                }
            });
        marks.forEach(
            [&](std::int32_t frame, const FrameElement<Mark>& mark)
            {
                if (const auto* original = baseline->marks.find(frame); original == nullptr || original->value != mark.value)
                {
                    keys.insert(frame);
                }
            });
        if (std::ranges::equal(keys, baselineKeys))
        {
            return baseline->keyFrames;
        }
        return std::vector<std::int32_t>(keys.begin(), keys.end());
    }
    // A created point mass: every mark is a key frame, and one without marks saves none.
    std::vector<std::int32_t> keys;
    marks.forEach([&](std::int32_t frame, const FrameElement<Mark>& /*mark*/) { keys.push_back(frame); });
    if (keys.empty())
    {
        return missing<std::vector<std::int32_t>>();
    }
    return keys;
}

} // namespace

bool sameValue(const CommandValue& left, const CommandValue& right)
{
    if (left.index() != right.index())
    {
        return false;
    }
    return std::visit(
        [&](const auto& item)
        {
            using Item = std::decay_t<decltype(item)>;
            const auto& other = std::get<Item>(right);
            if constexpr (std::is_same_v<Item, double>)
            {
                return item == other || (std::isnan(item) && std::isnan(other));
            }
            else if constexpr (requires { item.value; })
            {
                return item.value == other.value;
            }
            else
            {
                return item == other;
            }
        },
        left);
}

bool operator==(const Batch& left, const Batch& right)
{
    return left.commands == right.commands;
}

std::string_view commandErrorName(CommandErrorCode code) noexcept
{
    switch (code)
    {
    case CommandErrorCode::UnknownObject:
        return "unknown-object";
    case CommandErrorCode::UnknownField:
        return "unknown-field";
    case CommandErrorCode::InvalidElement:
        return "invalid-element";
    case CommandErrorCode::StaleValue:
        return "stale-value";
    case CommandErrorCode::InvalidValue:
        return "invalid-value";
    case CommandErrorCode::NoChange:
        return "no-change";
    case CommandErrorCode::InvalidIdentity:
        return "invalid-identity";
    case CommandErrorCode::InvalidObject:
        return "invalid-object";
    case CommandErrorCode::InvalidPosition:
        return "invalid-position";
    case CommandErrorCode::NotRemovable:
        return "not-removable";
    case CommandErrorCode::TimingInvariant:
        return "timing-invariant";
    case CommandErrorCode::GraphRefused:
        return "graph-refused";
    case CommandErrorCode::NothingToUndo:
        break;
    }
    return "nothing-to-undo";
}

std::expected<PreparedCommand, CommandError> prepare(const TrackerDocument& document, const Command& command, const ApplyOptions& options)
{
    Working working(document);
    auto inverse = applyCommand(working, command, options.mode);
    if (!inverse)
    {
        return std::unexpected(std::move(inverse.error()));
    }
    if (auto checked = checkClip(document, working, options); !checked)
    {
        return std::unexpected(std::move(checked.error()));
    }
    auto graph = working.takeGraph();
    return PreparedCommand{.document = DocumentEditor::make(std::move(working.parts), std::move(graph), std::move(working.issues)),
                           .inverse = *std::move(inverse)};
}

TrackerDocument withRevisionFloor(TrackerDocument document, Revision floor)
{
    auto graph = std::make_shared<DependencyGraph>(document.graph());
    graph->markAllChanged(floor);
    DocumentEditor::setGraph(document, std::move(graph));
    return document;
}

std::optional<ObjectId> freshIdentity(const TrackerDocument& document, AllocationRange range)
{
    return fresh(document.parts(), range);
}

std::optional<CommandValue> fieldValue(const TypedValue& value, std::string_view field, std::optional<std::int32_t> frame)
{
    std::optional<CommandValue> result;
    std::visit(
        [&](const auto& state)
        {
            if (frame)
            {
                if (field == "framedata")
                {
                    seriesField(state,
                                [&](const auto& series)
                                {
                                    const auto* element = series.find(*frame);
                                    result = element != nullptr ? CommandValue(*element) : CommandValue{};
                                });
                }
                return;
            }
            if constexpr (std::is_same_v<std::decay_t<decltype(state)>, VideoState>)
            {
                if (field == "paths")
                {
                    result = state.paths ? CommandValue(*state.paths) : CommandValue(osp::DecodeError::Missing);
                    return;
                }
            }
            scalarField(state, field, [&](const auto& read) { result = fromRead(read); });
        },
        value);
    return result;
}

bool setFieldValue(TypedValue& value, std::string_view field, std::optional<std::int32_t> frame, const CommandValue& written)
{
    return std::visit(
        [&](auto& state)
        {
            bool done = false;
            if (frame)
            {
                if (field == "framedata")
                {
                    seriesField(state,
                                [&](auto& series)
                                {
                                    using Element = typename std::decay_t<decltype(series)>::Element;
                                    if (std::holds_alternative<std::monostate>(written))
                                    {
                                        series.erase(*frame);
                                        done = true;
                                    }
                                    else if (const auto* element = std::get_if<Element>(&written))
                                    {
                                        series.set(*frame, *element);
                                        done = true;
                                    }
                                });
                }
                return done;
            }
            if constexpr (std::is_same_v<std::decay_t<decltype(state)>, VideoState>)
            {
                if (field == "paths")
                {
                    if (const auto* paths = std::get_if<std::vector<std::string>>(&written))
                    {
                        state.paths = *paths;
                        return true;
                    }
                    const auto* error = std::get_if<osp::DecodeError>(&written);
                    if (error != nullptr && *error == osp::DecodeError::Missing)
                    {
                        state.paths.reset();
                        return true;
                    }
                    return false;
                }
            }
            scalarField(state, field, [&](auto& read) { done = toRead(read, written); });
            return done;
        },
        value);
}

std::optional<Insert> createTrack(const TrackerDocument& document, Family family, std::string name)
{
    const auto id = freshIdentity(document, AllocationRange::Counter);
    if (!id || (family != Family::PointMass && family != Family::Vector))
    {
        return std::nullopt;
    }
    auto value = family == Family::PointMass ? TypedValue(PointMassState{.name = std::move(name), .mass = 1.0})
                                             : TypedValue(VectorState{.name = std::move(name)});
    // After the last track of its family: the track list shows point masses, then vectors.
    const auto order = document.trackOrder();
    std::size_t trackPosition = family == Family::PointMass ? 0 : order.size();
    for (std::size_t index = order.size(); index > 0; --index)
    {
        if (document.object(order[index - 1])->family() == family)
        {
            trackPosition = index;
            break;
        }
    }
    return Insert{.object = {.id = *id,
                             .range = AllocationRange::Counter,
                             .role = Role::Track,
                             .className = std::string(family == Family::PointMass ? tracker::POINT_MASS_CLASS : tracker::VECTOR_CLASS),
                             .binding = std::nullopt,
                             .state = ObjectState::Live,
                             .value = std::make_shared<const TypedValue>(std::move(value))},
                  .position = document.objects().size(),
                  .trackPosition = trackPosition};
}

std::optional<Insert> createSingleton(const TrackerDocument& document, Role role, TypedValue value)
{
    const auto id = freshIdentity(document, AllocationRange::Countdown);
    const auto kind = creatable(familyOf(value));
    if (!id || !kind || kind->role != role || role == Role::Track)
    {
        return std::nullopt;
    }
    return Insert{.object = {.id = *id,
                             .range = AllocationRange::Countdown,
                             .role = role,
                             .className = std::string(kind->className),
                             .binding = std::nullopt,
                             .state = ObjectState::Live,
                             .value = std::make_shared<const TypedValue>(std::move(value))},
                  .position = document.objects().size()};
}

std::expected<Command, CommandError>
setMark(const TrackerDocument& document, ObjectId pointMass, std::int32_t frame, std::optional<Mark> mark)
{
    const auto* object = document.object(pointMass);
    if (object == nullptr || object->state != ObjectState::Live || object->family() != Family::PointMass)
    {
        return refuse(CommandErrorCode::UnknownObject, "The command's object is not a live point mass.");
    }
    const auto& state = std::get<PointMassState>(*object->value);
    const auto* current = state.marks.find(frame);
    SetField marks{.object = pointMass,
                   .field = "framedata",
                   .frame = frame,
                   .before = current != nullptr ? CommandValue(FrameElement<Mark>{.value = current->value}) : CommandValue{},
                   .after = mark ? CommandValue(FrameElement<Mark>{.value = *mark}) : CommandValue{}};
    auto after = state.marks;
    if (mark)
    {
        after.set(frame, {.value = *mark, .node = current != nullptr ? current->node : std::nullopt});
    }
    else
    {
        after.erase(frame);
    }
    const auto keysBefore = fromRead(state.keyFrames);
    const auto keysAfter = fromRead(keyFramesFor(document, pointMass, after));
    if (sameValue(keysBefore, keysAfter))
    {
        return marks;
    }
    return Batch{
        .commands = {std::move(marks),
                     SetField{.object = pointMass, .field = "keyFrames", .frame = std::nullopt, .before = keysBefore, .after = keysAfter}}};
}

} // namespace motrack::model
