#include "model/tracker_document.hpp"

#include "model/dependencies/name_binding.hpp"
#include "model/descriptors.hpp"
#include "model/tracker_loaders.hpp"

#include <algorithm>
#include <cmath>
#include <ranges>
#include <set>
#include <unordered_set>
#include <utility>

namespace motrack::model
{
namespace
{

/// A refusal of B1's import policy, with B1's message; caught at the import boundary.
struct Refusal
{
    std::string message;
};

[[noreturn]] void refuse(std::string message)
{
    throw Refusal{std::move(message)};
}

/// Whether VideoClip.Loader applies a saved int (see legacy_project.cpp): Java ignores a missing or other-typed
/// property, which reads as Integer.MIN_VALUE, while MoTrack refuses unreadable int text instead of guessing.
bool savedInteger(const Read<std::int32_t>& value)
{
    return value || value.error() == osp::DecodeError::Malformed || value.error() == osp::DecodeError::OutOfRange;
}

void requireInteger(const Read<std::int32_t>& value, std::int32_t minimum)
{
    if (savedInteger(value) && (!value || *value < minimum))
    {
        refuse("Invalid project integer.");
    }
}

std::int32_t arrayIndex(const tracker::ArrayElement& element)
{
    if (!element.index || *element.index < 0)
    {
        refuse("Invalid Tracker array entry.");
    }
    return *element.index;
}

double coordinate(const Read<double>& value)
{
    if (!value || !std::isfinite(*value))
    {
        refuse("Invalid Tracker numeric property.");
    }
    return *value;
}

template <typename T> using Indexed = std::vector<std::pair<std::int32_t, FrameElement<T>>>;

/// Frames in file order, sorted by frame; a repeated frame is refused with `duplicate`.
template <typename T> FrameSeries<T> series(Indexed<T> frames, std::string_view duplicate)
{
    std::ranges::sort(frames, {}, &std::pair<std::int32_t, FrameElement<T>>::first);
    if (std::ranges::adjacent_find(frames, {}, &std::pair<std::int32_t, FrameElement<T>>::first) != frames.end())
    {
        refuse(std::string(duplicate));
    }
    FrameSeries<T> result;
    for (auto& [frame, element] : frames)
    {
        result.set(frame, std::move(element));
    }
    return result;
}

PointMassState pointMass(const tracker::PointMassLoad& loaded)
{
    PointMassState state{.name = loaded.name, .mass = loaded.mass, .marks = {}, .keyFrames = loaded.keyFrames};
    Indexed<Mark> marks;
    if (loaded.frames)
    {
        for (const auto& frame : loaded.frames->frames)
        {
            if (frame.element.object)
            {
                const auto number = arrayIndex(frame.element);
                const auto x = coordinate(frame.data.x);
                const auto y = coordinate(frame.data.y);
                marks.emplace_back(number, FrameElement<Mark>{.value = {.x = x, .y = y}, .node = frame.element.property});
            }
        }
    }
    state.marks = series(std::move(marks), "Duplicate Tracker point mass frame.");
    return state;
}

VectorState vector(const tracker::VectorLoad& loaded)
{
    VectorState state{.name = loaded.name, .samples = {}};
    Indexed<VectorSample> samples;
    if (loaded.frames)
    {
        for (const auto& frame : loaded.frames->frames)
        {
            if (frame.element.object)
            {
                const auto number = arrayIndex(frame.element);
                const VectorSample sample{.xTail = coordinate(frame.data.xtail),
                                          .yTail = coordinate(frame.data.ytail),
                                          .xComponent = coordinate(frame.data.xcomponent),
                                          .yComponent = coordinate(frame.data.ycomponent)};
                samples.emplace_back(number, FrameElement<VectorSample>{.value = sample, .node = frame.element.property});
            }
        }
    }
    state.samples = series(std::move(samples), "Duplicate Tracker vector frame.");
    return state;
}

/// The coordinate system as B1 presents it, or nothing where B1 imports no calibration: another class, a reference
/// frame, no usable keys, or a duplicate key.
std::optional<CoordinatesState> coordinates(const tracker::PanelLoad& panel)
{
    if (!panel.coords || panel.coords->className != tracker::COORD_SYSTEM_CLASS || !loadedName(panel.referenceFrame).empty())
    {
        return std::nullopt;
    }
    const auto& coords = *panel.coords;
    CoordinatesState state{.fixedOrigin = coords.fixedOrigin,
                           .fixedAngle = coords.fixedAngle,
                           .fixedScale = coords.fixedScale,
                           .locked = coords.locked,
                           .keys = {}};
    try
    {
        Indexed<CoordKey> keys;
        if (coords.frames)
        {
            for (const auto& frame : coords.frames->frames)
            {
                if (frame.element.object)
                {
                    const auto number = arrayIndex(frame.element);
                    const CoordKey key{.xOrigin = coordinate(frame.data.xorigin),
                                       .yOrigin = coordinate(frame.data.yorigin),
                                       .angle = coordinate(frame.data.angle),
                                       .xScale = coordinate(frame.data.xscale),
                                       .yScale = coordinate(frame.data.yscale)};
                    keys.emplace_back(number, FrameElement<CoordKey>{.value = key, .node = frame.element.property});
                }
            }
        }
        state.keys = series(std::move(keys), "Duplicate calibration key.");
    }
    catch (const Refusal&)
    {
        return std::nullopt;
    }
    if (state.keys.empty())
    {
        return std::nullopt;
    }
    return state;
}

/// ImageVideo `paths` in index order, refused as B1 refuses an image sequence it cannot present.
std::optional<std::vector<std::string>> videoPaths(const tracker::VideoLoad& video)
{
    std::vector<std::pair<std::int32_t, std::string>> indexed;
    for (const auto& entry : video.pathElements)
    {
        if (!entry.value)
        {
            refuse("Invalid Tracker image sequence.");
        }
        indexed.emplace_back(arrayIndex(entry.element), *entry.value);
    }
    std::ranges::sort(indexed, {}, &std::pair<std::int32_t, std::string>::first);
    std::vector<std::string> paths;
    for (auto& [index, path] : indexed)
    {
        if (static_cast<std::size_t>(index) != paths.size() || path.empty())
        {
            refuse("Invalid Tracker image sequence.");
        }
        paths.push_back(std::move(path));
    }
    if (!video.paths)
    {
        return std::nullopt;
    }
    return paths;
}

ObjectSlot slot(Role role, std::string className, osp::NodeId node, TypedValue value)
{
    return {.id = {},
            .range = AllocationRange::Countdown,
            .role = role,
            .className = std::move(className),
            .binding = node,
            .state = ObjectState::Live,
            .value = std::make_shared<const TypedValue>(std::move(value))};
}

/// Hands out countdown identities to the objects without one, in baseline order (unbound objects last, in slot
/// order), below the low-water mark and skipping every value another identity uses. Returns the new low-water mark.
std::uint64_t assignCountdown(std::vector<ObjectSlot>& objects, std::uint64_t lowWater)
{
    std::unordered_set<std::uint64_t> used;
    std::vector<std::size_t> pending;
    bool startHandedOut = false;
    for (std::size_t index = 0; index < objects.size(); ++index)
    {
        const auto& object = objects[index];
        if (object.id.isValid())
        {
            used.insert(object.id.value);
            startHandedOut = startHandedOut || (object.range == AllocationRange::Countdown && object.id.value == COUNTDOWN_START);
        }
        else if (object.range == AllocationRange::Countdown)
        {
            pending.push_back(index);
        }
    }
    std::ranges::stable_sort(pending,
                             [&](std::size_t left, std::size_t right)
                             {
                                 const auto& a = objects[left].binding;
                                 const auto& b = objects[right].binding;
                                 if (a && b)
                                 {
                                     return std::to_underlying(*a) < std::to_underlying(*b);
                                 }
                                 return a.has_value() && !b.has_value();
                             });
    // Continue below the low-water mark; the range's first value is free only while nothing has been handed out.
    auto next = lowWater < COUNTDOWN_START || startHandedOut ? lowWater - 1 : COUNTDOWN_START;
    for (const auto index : pending)
    {
        while (used.contains(next))
        {
            --next;
        }
        objects[index].range = AllocationRange::Countdown;
        objects[index].id = ObjectId{next};
        used.insert(next);
        lowWater = next--;
    }
    return lowWater;
}

std::optional<std::string> graphLabel(const ObjectSlot& object)
{
    if (const auto* mass = std::get_if<PointMassState>(object.value.get()))
    {
        return mass->name ? std::optional(*mass->name) : std::nullopt;
    }
    if (const auto* vector = std::get_if<VectorState>(object.value.get()))
    {
        return vector->name ? std::optional(*vector->name) : std::nullopt;
    }
    if (const auto* unknown = std::get_if<UnknownState>(object.value.get()))
    {
        return unknown->label;
    }
    return std::nullopt;
}

bool familyFitsRole(Role role, Family family)
{
    if (family == Family::Unknown)
    {
        return true;
    }
    switch (role)
    {
    case Role::Panel:
        return family == Family::Panel;
    case Role::Clip:
        return family == Family::Clip;
    case Role::Video:
        return family == Family::Video;
    case Role::ClipControl:
        return family == Family::ClipControl;
    case Role::Coordinates:
        return family == Family::Coordinates;
    case Role::Track:
        break;
    }
    return family == Family::PointMass || family == Family::Vector;
}

/// An unbound object; `assemble` gives it a countdown identity.
ObjectSlot nativeObject(Role role, std::string_view className, TypedValue value)
{
    return {.id = {},
            .range = AllocationRange::Countdown,
            .role = role,
            .className = std::string(className),
            .binding = std::nullopt,
            .state = ObjectState::Live,
            .value = std::make_shared<const TypedValue>(std::move(value))};
}

/// An empty project: a panel with LegacyProject's default labels, a clip of one step size and a clip control
/// without a frame duration (an empty project has none, unlike a Tracker panel's default stepper).
DocumentParts emptyParts()
{
    DocumentParts parts;
    parts.objects.push_back(nativeObject(Role::Panel,
                                         tracker::TRACKER_PANEL_CLASS,
                                         PanelState{.semanticVersion = std::string{},
                                                    .description = std::string{},
                                                    .lengthUnit = std::string{},
                                                    .massUnit = std::string("kg"),
                                                    .timeUnit = std::string("s")}));
    parts.objects.push_back(
        nativeObject(Role::Clip, tracker::VIDEO_CLIP_CLASS, ClipState{.startFrame = 0, .stepSize = 1, .startTime = 0.0}));
    parts.objects.push_back(nativeObject(Role::ClipControl, tracker::STEPPER_CLIP_CONTROL_CLASS, ClipControlState{}));
    return parts;
}

std::unexpected<DocumentError> invalid(std::string message)
{
    return std::unexpected(DocumentError{.message = std::move(message)});
}

/// Validates everything `assemble` accepts, before deriving anything.
std::expected<void, DocumentError> validate(const DocumentParts& parts)
{
    if (parts.nextObjectId == 0 || parts.countdownLowWater == 0)
    {
        return invalid("Invalid document identity counter.");
    }
    if ((parts.associations == Associations::None) == static_cast<bool>(parts.baseline))
    {
        return invalid("Document associations do not match its baseline.");
    }
    std::unordered_set<ObjectId> ids;
    std::set<std::pair<Role, bool>> singletons;
    std::set<osp::NodeId> bound;
    std::size_t panels = 0;
    for (const auto& object : parts.objects)
    {
        if (!object.value || !object.id.isValid() || !ids.insert(object.id).second)
        {
            return invalid("Invalid or duplicate document identity.");
        }
        if (object.range == AllocationRange::Counter ? object.id.value >= parts.nextObjectId : object.id.value < parts.countdownLowWater)
        {
            return invalid("Document identity outside its allocation range.");
        }
        if (object.role != Role::Track && object.range != AllocationRange::Countdown)
        {
            return invalid("Document singletons take countdown identities.");
        }
        const auto family = object.family();
        if (!familyFitsRole(object.role, family))
        {
            return invalid("Document object does not belong to its role.");
        }
        if (object.role != Role::Track && object.state == ObjectState::Live &&
            !singletons.emplace(object.role, family == Family::Unknown).second)
        {
            return invalid("Duplicate document singleton.");
        }
        if (object.role == Role::Panel && object.state == ObjectState::Live && family == Family::Panel)
        {
            ++panels;
        }
        if (object.binding)
        {
            if (parts.associations != Associations::Known || !parts.baseline->contains(*object.binding) ||
                !bound.insert(*object.binding).second)
            {
                return invalid("Invalid document source binding.");
            }
            const auto& node = parts.baseline->node(*object.binding);
            if (node.kind != osp::NodeKind::Object || node.className != object.className)
            {
                return invalid("Document source binding names another object.");
            }
        }
    }
    if (panels != 1)
    {
        return invalid("A document has exactly one panel.");
    }
    std::unordered_set<ObjectId> ordered;
    for (const auto id : parts.trackOrder)
    {
        const auto found = std::ranges::find(parts.objects, id, &ObjectSlot::id);
        if (found == parts.objects.end() || found->role != Role::Track || found->state != ObjectState::Live ||
            (found->family() != Family::PointMass && found->family() != Family::Vector) || !ordered.insert(id).second)
        {
            return invalid("Invalid document track order.");
        }
    }
    const auto typedTracks = std::ranges::count_if(parts.objects,
                                                   [](const ObjectSlot& object)
                                                   {
                                                       return object.role == Role::Track && object.state == ObjectState::Live &&
                                                              (object.family() == Family::PointMass || object.family() == Family::Vector);
                                                   });
    if (std::cmp_not_equal(typedTracks, parts.trackOrder.size()))
    {
        return invalid("Invalid document track order.");
    }
    return {};
}

} // namespace

std::string_view associationsName(Associations associations) noexcept
{
    switch (associations)
    {
    case Associations::None:
        return "none";
    case Associations::Known:
        return "known";
    case Associations::Unknown:
        return "unknown";
    case Associations::Stale:
        break;
    }
    return "stale";
}

std::string_view allocationRangeName(AllocationRange range) noexcept
{
    return range == AllocationRange::Counter ? "counter" : "countdown";
}

bool operator==(const ObjectSlot& left, const ObjectSlot& right)
{
    return left.id == right.id && left.range == right.range && left.role == right.role && left.className == right.className &&
           left.binding == right.binding && left.state == right.state &&
           (left.value == right.value || (left.value && right.value && *left.value == *right.value));
}

TrackerDocument::TrackerDocument() : TrackerDocument(assemble(emptyParts()).value())
{}

TrackerDocument::TrackerDocument(DocumentParts parts) : m_Parts(std::move(parts))
{}

std::expected<TrackerDocument, DocumentError> TrackerDocument::importBaseline(std::shared_ptr<const osp::Source> baseline)
{
    if (!baseline)
    {
        return invalid("Missing Tracker baseline.");
    }
    const auto& source = *baseline;
    const auto panel = tracker::loadPanel(source);
    if (panel.className != tracker::TRACKER_PANEL_CLASS)
    {
        return invalid("Expected a TrackerPanel project.");
    }
    DocumentParts parts{.baseline = std::move(baseline), .associations = Associations::Known};
    auto& objects = parts.objects;
    try
    {
        objects.push_back(slot(Role::Panel,
                               panel.className,
                               panel.object,
                               PanelState{.semanticVersion = panel.semanticVersion,
                                          .description = panel.description,
                                          .lengthUnit = panel.lengthUnit,
                                          .massUnit = panel.massUnit,
                                          .timeUnit = panel.timeUnit,
                                          .referenceFrame = panel.referenceFrame,
                                          .selectedTrack = panel.selectedTrack}));
        if (panel.videoClip)
        {
            const auto& clip = *panel.videoClip;
            requireInteger(clip.startFrame, 0);
            requireInteger(clip.stepSize, 1);
            requireInteger(clip.stepCount, 0);
            objects.push_back(slot(Role::Clip,
                                   clip.className,
                                   clip.object,
                                   ClipState{.startFrame = clip.startFrame,
                                             .stepSize = clip.stepSize,
                                             .stepCount = clip.stepCount,
                                             .frameCount = clip.frameCount,
                                             .frameShift = clip.frameShift,
                                             .startTime = clip.startTime}));
            if (clip.video)
            {
                const auto& video = *clip.video;
                auto paths = videoPaths(video);
                if (paths && !paths->empty())
                {
                    parts.media.references = *paths;
                }
                else if (const auto path = loadedName(video.path); !path.empty())
                {
                    parts.media.references = {path};
                }
                objects.push_back(slot(Role::Video,
                                       video.className,
                                       video.object,
                                       VideoState{.path = video.path, .paths = std::move(paths), .deltaT = video.deltaT}));
            }
        }
        if (panel.clipControl)
        {
            const auto& control = *panel.clipControl;
            objects.push_back(
                slot(Role::ClipControl,
                     control.className,
                     control.object,
                     ClipControlState{.rate = control.rate, .deltaT = control.deltaT, .frame = control.frame, .looping = control.looping}));
        }
        if (panel.coords)
        {
            if (auto coords = coordinates(panel))
            {
                objects.push_back(slot(Role::Coordinates, panel.coords->className, panel.coords->object, std::move(*coords)));
            }
            else
            {
                objects.push_back(slot(Role::Coordinates, panel.coords->className, panel.coords->object, UnknownState{}));
            }
        }
        for (const auto& item : panel.trackItems)
        {
            if (!item.object)
            {
                continue; // A null or non-object element is not an object.
            }
            const auto label = savedLabel(source, *item.object);
            if (item.className == tracker::POINT_MASS_CLASS)
            {
                objects.push_back(slot(Role::Track, item.className, *item.object, pointMass(tracker::loadPointMass(source, *item.object))));
            }
            else if (item.className == tracker::VECTOR_CLASS)
            {
                try
                {
                    objects.push_back(slot(Role::Track, item.className, *item.object, vector(tracker::loadVector(source, *item.object))));
                }
                catch (const Refusal&)
                {
                    objects.push_back(slot(Role::Track, item.className, *item.object, UnknownState{.label = label}));
                }
            }
            else
            {
                objects.push_back(slot(Role::Track, item.className, *item.object, UnknownState{.label = label}));
            }
        }
    }
    catch (const Refusal& refusal)
    {
        return invalid(refusal.message);
    }
    for (auto& object : objects)
    {
        if (object.role == Role::Track && object.family() != Family::Unknown)
        {
            object.range = AllocationRange::Counter;
            object.id = ObjectId{parts.nextObjectId++};
        }
    }
    // The track list shows point masses, then vectors, each in source order.
    for (const auto family : {Family::PointMass, Family::Vector})
    {
        for (const auto& object : objects)
        {
            if (object.role == Role::Track && object.family() == family)
            {
                parts.trackOrder.push_back(object.id);
            }
        }
    }
    return assemble(std::move(parts));
}

std::expected<TrackerDocument, DocumentError> TrackerDocument::assemble(DocumentParts parts)
{
    parts.countdownLowWater = assignCountdown(parts.objects, parts.countdownLowWater);
    if (auto valid = validate(parts); !valid)
    {
        return std::unexpected(std::move(valid.error()));
    }
    TrackerDocument document(std::move(parts));
    if (auto derived = document.derive(); !derived)
    {
        return std::unexpected(std::move(derived.error()));
    }
    return document;
}

std::expected<void, DocumentError> TrackerDocument::derive()
{
    m_Index.clear();
    m_ReferenceIssues.clear();
    auto graph = std::make_shared<DependencyGraph>();
    const auto& objects = m_Parts.objects;
    for (std::size_t index = 0; index < objects.size(); ++index)
    {
        const auto& object = objects[index];
        m_Index.emplace(object.id, index);
        const auto added = object.state == ObjectState::Live
                               ? graph->addObject(object.id, {.label = graphLabel(object), .className = object.className})
                               : graph->retireIdentity(object.id);
        if (!added)
        {
            return invalid(std::string(errorMessage(added.error().code)));
        }
    }
    if (m_Parts.associations == Associations::Known)
    {
        // Saved names bind against the baseline's original labels, so renames and removals never change their targets.
        const auto& source = *m_Parts.baseline;
        std::vector<NamedObject> named;
        for (const auto& object : objects)
        {
            if (object.role == Role::Track && object.binding)
            {
                named.push_back({.id = object.id, .label = savedLabel(source, *object.binding), .className = object.className});
            }
        }
        NameIndex names(named);
        for (const auto& object : objects)
        {
            if (object.state != ObjectState::Live || !object.binding)
            {
                continue;
            }
            for (const auto& field : referenceDescriptors())
            {
                if (!std::ranges::contains(field.ownerClasses, std::string_view(object.className)))
                {
                    continue;
                }
                const auto saved = readReferences(source, *object.binding, field);
                if (!saved)
                {
                    continue;
                }
                if (saved->malformed)
                {
                    m_ReferenceIssues.push_back({.owner = object.id, .field = std::string(field.id), .reason = "malformed"});
                    continue;
                }
                for (std::size_t slotIndex = 0; slotIndex < saved->names.size(); ++slotIndex)
                {
                    const auto& name = saved->names[slotIndex];
                    if (!name)
                    {
                        continue; // An explicitly empty slot has no dependency.
                    }
                    auto binding = names.bind(*name, field.filter);
                    if (!binding)
                    {
                        m_ReferenceIssues.push_back({.owner = object.id,
                                                     .field = std::string(field.id),
                                                     .reason = std::string(errorMessage(binding.error().code))});
                        continue;
                    }
                    const auto dependency = makeDependency(field.kind,
                                                           field.slotBase + static_cast<std::uint32_t>(slotIndex),
                                                           field.targetFacet,
                                                           *name,
                                                           *binding,
                                                           field.onMissing);
                    if (!dependency)
                    {
                        continue;
                    }
                    const auto added =
                        graph->addDependency({.object = object.id, .facet = field.ownerFacet}, *dependency, CyclePolicy::Record);
                    if (!added)
                    {
                        m_ReferenceIssues.push_back(
                            {.owner = object.id, .field = std::string(field.id), .reason = std::string(errorMessage(added.error().code))});
                    }
                }
            }
        }
    }
    m_Graph = std::move(graph);
    return {};
}

const std::shared_ptr<const osp::Source>& TrackerDocument::baseline() const noexcept
{
    return m_Parts.baseline;
}

Associations TrackerDocument::associations() const noexcept
{
    return m_Parts.associations;
}

std::span<const ObjectSlot> TrackerDocument::objects() const noexcept
{
    return m_Parts.objects;
}

const ObjectSlot* TrackerDocument::object(ObjectId id) const
{
    const auto found = m_Index.find(id);
    return found == m_Index.end() ? nullptr : &m_Parts.objects[found->second];
}

const ObjectSlot* TrackerDocument::singleton(Role role) const
{
    const ObjectSlot* unknown = nullptr;
    for (const auto& object : m_Parts.objects)
    {
        if (object.role == role && object.state == ObjectState::Live)
        {
            if (object.family() != Family::Unknown)
            {
                return &object;
            }
            unknown = &object;
        }
    }
    return unknown;
}

std::span<const ObjectId> TrackerDocument::trackOrder() const noexcept
{
    return m_Parts.trackOrder;
}

std::uint64_t TrackerDocument::nextObjectId() const noexcept
{
    return m_Parts.nextObjectId;
}

std::uint64_t TrackerDocument::countdownLowWater() const noexcept
{
    return m_Parts.countdownLowWater;
}

bool TrackerDocument::hasCounterCapacity() const noexcept
{
    return m_Parts.nextObjectId > 0 && m_Parts.nextObjectId < m_Parts.countdownLowWater;
}

const MediaExtension& TrackerDocument::media() const noexcept
{
    return m_Parts.media;
}

const DependencyGraph& TrackerDocument::graph() const noexcept
{
    return *m_Graph;
}

const std::shared_ptr<const DependencyGraph>& TrackerDocument::sharedGraph() const noexcept
{
    return m_Graph;
}

std::span<const ReferenceIssue> TrackerDocument::referenceIssues() const noexcept
{
    return m_ReferenceIssues;
}

std::vector<std::string> TrackerDocument::unknownTrackClasses() const
{
    std::vector<std::string> classes;
    for (const auto& object : m_Parts.objects)
    {
        if (object.role == Role::Track && object.state == ObjectState::Live && object.family() == Family::Unknown &&
            !object.className.empty())
        {
            classes.push_back(object.className);
        }
    }
    return classes;
}

const DocumentParts& TrackerDocument::parts() const noexcept
{
    return m_Parts;
}

bool operator==(const TrackerDocument& left, const TrackerDocument& right)
{
    const auto& a = left.m_Parts;
    const auto& b = right.m_Parts;
    const bool sameBaseline = a.baseline == b.baseline || (a.baseline && b.baseline && a.baseline->bytes() == b.baseline->bytes());
    return sameBaseline && a.associations == b.associations && a.objects == b.objects && a.trackOrder == b.trackOrder &&
           a.nextObjectId == b.nextObjectId && a.countdownLowWater == b.countdownLowWater && a.media == b.media;
}

std::vector<PathStep> baselinePath(const osp::Source& source, osp::NodeId node)
{
    std::vector<PathStep> path;
    auto current = node;
    while (const auto parent = source.node(current).parent)
    {
        const auto& siblings = source.node(*parent).children;
        const auto ordinal = static_cast<std::size_t>(std::ranges::find(siblings, current) - siblings.begin());
        path.push_back({.ordinal = ordinal, .name = source.node(current).name.value_or(std::string{})});
        current = *parent;
    }
    std::ranges::reverse(path);
    return path;
}

} // namespace motrack::model
