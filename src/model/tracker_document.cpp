#include "model/tracker_document.hpp"

#include "model/dependencies/name_binding.hpp"
#include "model/descriptors.hpp"
#include "model/tracker_loaders.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <ranges>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace motrack::model
{
namespace
{

/// A refusal of B1's import policy, with B1's message; caught at the import boundary.
class Refusal : public std::runtime_error
{
  public:
    using std::runtime_error::runtime_error;
};

[[noreturn]] void refuse(const std::string& message)
{
    throw Refusal(message);
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

/// How many leading slots Tracker's application loader removes from a track's `framedata` before the track reads it,
/// or none when it removes the whole array (#313; see legacy_project.cpp): TrackerIO's adjustFrameData drops the
/// first `shift` slots (when positive) of every track array of Java length above 1, so saved index n loads at frame
/// n - shift. An array no longer than the shift loads no frames, and one of length 1 is unchanged. An array whose last
/// index Java cannot read stays unshifted, as before #313.
template <typename Data> std::optional<std::int32_t> droppedSlots(const tracker::FrameArray<Data>& array, std::int32_t shift)
{
    const auto last = array.frames.empty() ? Read<std::int32_t>(0) : array.frames.back().element.index;
    if (shift <= 0 || !last || *last < 0)
    {
        return 0;
    }
    const auto length = std::int64_t{*last} + 1;
    if (length <= 1)
    {
        return 0;
    }
    return shift >= length ? std::nullopt : std::optional(shift);
}

/// A track's object elements as Java's array loader keeps them (reading revision 2): in index order, a later element
/// of an index replacing an earlier one, a null one included. Where Java cannot place the indices, every object element
/// in file order, as MoTrack read them before (#315 decides refusals).
template <typename Data>
std::vector<const tracker::Frame<Data>*> trackFrames(const osp::Source& source, const tracker::FrameArray<Data>& array)
{
    std::vector<const tracker::Frame<Data>*> result;
    if (const auto indexed = osp::PropertyView(source, array.property).indexedElements())
    {
        std::map<osp::NodeId, const tracker::Frame<Data>*> decoded;
        for (const auto& frame : array.frames)
        {
            decoded.emplace(frame.element.property, &frame);
        }
        for (const auto& element : indexed->elements | std::views::values)
        {
            if (const auto* frame = decoded.at(element.id()); frame->element.object)
            {
                result.push_back(frame);
            }
        }
        return result;
    }
    for (const auto& frame : array.frames)
    {
        if (frame.element.object)
        {
            result.push_back(&frame);
        }
    }
    return result;
}

/// The loaded frame of a saved track element, or none when Tracker drops its slot.
std::optional<std::int32_t> loadedFrame(const tracker::ArrayElement& element, std::optional<std::int32_t> dropped)
{
    const auto index = arrayIndex(element);
    return dropped && index >= *dropped ? std::optional(index - *dropped) : std::nullopt;
}

/// A track's frames by Tracker's loaded frame number; `decode` reads one kept frame's value.
template <typename T, typename Data, typename Decode>
FrameSeries<T> trackSeries(
    const osp::Source& source, const tracker::FrameArray<Data>& array, std::int32_t shift, Decode decode, std::string_view duplicate)
{
    Indexed<T> elements;
    const auto dropped = droppedSlots(array, shift);
    for (const auto* frame : trackFrames(source, array))
    {
        if (const auto number = loadedFrame(frame->element, dropped))
        {
            elements.emplace_back(*number, FrameElement<T>{.value = decode(frame->data), .node = frame->element.property});
        }
    }
    return series(std::move(elements), duplicate);
}

PointMassState pointMass(const osp::Source& source, const tracker::PointMassLoad& loaded, std::int32_t shift)
{
    PointMassState state{.name = loaded.name, .mass = loaded.mass, .marks = {}, .keyFrames = loaded.keyFrames};
    if (loaded.frames)
    {
        state.marks = trackSeries<Mark>(
            source,
            *loaded.frames,
            shift,
            [](const tracker::PointMassFrameData& data)
            {
                const auto x = coordinate(data.x);
                return Mark{.x = x, .y = coordinate(data.y)};
            },
            "Duplicate Tracker point mass frame.");
    }
    return state;
}

VectorState vector(const osp::Source& source, const tracker::VectorLoad& loaded, std::int32_t shift)
{
    VectorState state{.name = loaded.name, .samples = {}};
    if (loaded.frames)
    {
        state.samples = trackSeries<VectorSample>(
            source,
            *loaded.frames,
            shift,
            [](const tracker::VectorFrameData& data)
            {
                return VectorSample{.xTail = coordinate(data.xtail),
                                    .yTail = coordinate(data.ytail),
                                    .xComponent = coordinate(data.xcomponent),
                                    .yComponent = coordinate(data.ycomponent)};
            },
            "Duplicate Tracker vector frame.");
    }
    return state;
}

/// The coordinate system as B1 presents it, or nothing where B1 imports no calibration: another class, a reference
/// frame, no usable keys, or indices Java cannot place. Keys take Java's slots, so the last of duplicate keys wins
/// (#314), and never shift: TrackerPanel loads `coords` without the frame adjuster.
std::optional<CoordinatesState> coordinates(const osp::Source& source, const tracker::PanelLoad& panel)
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
    if (coords.frames)
    {
        const auto indexed = osp::PropertyView(source, coords.frames->property).indexedElements();
        if (!indexed)
        {
            return std::nullopt;
        }
        std::map<osp::NodeId, const tracker::Frame<tracker::CoordFrameData>*> decoded;
        for (const auto& frame : coords.frames->frames)
        {
            decoded.emplace(frame.element.property, &frame);
        }
        try
        {
            for (const auto& [index, element] : indexed->elements)
            {
                if (const auto* frame = decoded.at(element.id()); frame->element.object)
                {
                    const auto& data = frame->data;
                    const CoordKey key{.xOrigin = coordinate(data.xorigin),
                                       .yOrigin = coordinate(data.yorigin),
                                       .angle = coordinate(data.angle),
                                       .xScale = coordinate(data.xscale),
                                       .yScale = coordinate(data.yscale)};
                    state.keys.set(static_cast<std::int32_t>(index), {.value = key, .node = frame->element.property});
                }
            }
        }
        catch (const Refusal&)
        {
            return std::nullopt;
        }
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
    if (parts.readerRevision > CURRENT_READER_REVISION)
    {
        return invalid("A newer MoTrack read this document's Tracker source.");
    }
    if (parts.associations == Associations::Known && parts.readerRevision != CURRENT_READER_REVISION)
    {
        return invalid("Known associations follow the current reader.");
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
    if (parts.associations == Associations::Known)
    {
        // Known associations: every addressable baseline object (the panel, its singletons and each object element of
        // `tracks`) has exactly one object, at the place its role names.
        const auto panel = tracker::loadPanel(*parts.baseline);
        std::map<osp::NodeId, Role> addressable{{panel.object, Role::Panel}};
        if (panel.videoClip)
        {
            addressable.emplace(panel.videoClip->object, Role::Clip);
            if (panel.videoClip->video)
            {
                addressable.emplace(panel.videoClip->video->object, Role::Video);
            }
        }
        if (panel.clipControl)
        {
            addressable.emplace(panel.clipControl->object, Role::ClipControl);
        }
        if (panel.coords)
        {
            addressable.emplace(panel.coords->object, Role::Coordinates);
        }
        for (const auto& item : panel.trackItems)
        {
            if (item.object)
            {
                addressable.emplace(*item.object, Role::Track);
            }
        }
        for (const auto& object : parts.objects)
        {
            if (const auto place = object.binding ? addressable.find(*object.binding) : addressable.end();
                object.binding && (place == addressable.end() || place->second != object.role))
            {
                return invalid("Document source binding is not at its role's place in the baseline.");
            }
        }
        if (bound.size() != addressable.size())
        {
            return invalid("A known document binds every addressable baseline object exactly once.");
        }
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
            if (auto coords = coordinates(source, panel))
            {
                objects.push_back(slot(Role::Coordinates, panel.coords->className, panel.coords->object, std::move(*coords)));
            }
            else
            {
                objects.push_back(slot(Role::Coordinates, panel.coords->className, panel.coords->object, UnknownState{}));
            }
        }
        // Tracker's application loader shifts every track's frames by the clip's positive frame shift (#313).
        const auto shift = panel.videoClip ? std::max(clipFrameShift(panel.videoClip->frameShift), 0) : 0;
        for (const auto& item : panel.trackItems)
        {
            if (!item.object)
            {
                continue; // A null or non-object element is not an object.
            }
            const auto label = savedLabel(source, *item.object);
            if (item.className == tracker::POINT_MASS_CLASS)
            {
                objects.push_back(slot(
                    Role::Track, item.className, *item.object, pointMass(source, tracker::loadPointMass(source, *item.object), shift)));
            }
            else if (item.className == tracker::VECTOR_CLASS)
            {
                try
                {
                    objects.push_back(
                        slot(Role::Track, item.className, *item.object, vector(source, tracker::loadVector(source, *item.object), shift)));
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
        return invalid(refusal.what());
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
    return findSingleton(m_Parts.objects, role);
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

std::uint32_t TrackerDocument::readerRevision() const noexcept
{
    return m_Parts.readerRevision;
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
    return sameBaseline && a.associations == b.associations && a.readerRevision == b.readerRevision && a.objects == b.objects &&
           a.trackOrder == b.trackOrder && a.nextObjectId == b.nextObjectId && a.countdownLowWater == b.countdownLowWater &&
           a.media == b.media;
}

void TrackerDocument::swap(TrackerDocument& other) noexcept
{
    // The parts hold only vectors, shared pointers and scalars; the index is swapped by member, since MSVC's
    // unordered_map can allocate when moved.
    static_assert(std::is_nothrow_swappable_v<DocumentParts>);
    std::swap(m_Parts, other.m_Parts);
    m_Index.swap(other.m_Index);
    m_Graph.swap(other.m_Graph);
    m_ReferenceIssues.swap(other.m_ReferenceIssues);
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

const ObjectSlot* findSingleton(std::span<const ObjectSlot> objects, Role role)
{
    const ObjectSlot* unknown = nullptr;
    for (const auto& object : objects)
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

std::optional<double> loadedFrameDuration(const ObjectSlot* video, const ObjectSlot* control)
{
    if (video && video->className != tracker::IMAGE_VIDEO_CLASS)
    {
        return std::nullopt;
    }
    const auto* controlState = control ? std::get_if<ClipControlState>(control->value.get()) : nullptr;
    const auto controlDuration = controlState ? finiteValue(controlState->deltaT) : std::nullopt;
    auto duration = controlDuration;
    if (duration.value_or(0.0) <= 0.0)
    {
        const auto* videoState = video ? std::get_if<VideoState>(video->value.get()) : nullptr;
        duration = videoState ? finiteValue(videoState->deltaT) : std::nullopt;
    }
    if (duration.value_or(0.0) <= 0.0)
    {
        duration.reset();
    }
    if (!video && (!controlState || controlDuration == 0.0))
    {
        duration = tracker::DEFAULT_STEPPER_FRAME_DURATION_MS;
    }
    return duration;
}

ClipTimeline clipTimeline(std::span<const ObjectSlot> objects, const MediaExtension& media)
{
    ClipTimeline timeline{.media = !media.references.empty(), .sourceFrameCount = media.sourceFrameCount};
    if (const auto* clip = findSingleton(objects, Role::Clip);
        const auto* state = clip ? std::get_if<ClipState>(clip->value.get()) : nullptr)
    {
        timeline.firstFrame = loadedStartFrame(*state).value_or(timeline.firstFrame);
        timeline.stepSize = state->stepSize.value_or(timeline.stepSize);
        if (state->stepCount)
        {
            timeline.stepCount = *state->stepCount;
        }
        timeline.startTimeMs = finiteValue(state->startTime).value_or(0.0);
    }
    timeline.frameDurationMs = loadedFrameDuration(findSingleton(objects, Role::Video), findSingleton(objects, Role::ClipControl));
    return timeline;
}

ClipTimeline clipTimeline(const TrackerDocument& document)
{
    return clipTimeline(document.objects(), document.media());
}

std::optional<PointMassState> baselinePointMass(const TrackerDocument& document, ObjectId id)
{
    const auto* object = document.object(id);
    if (object == nullptr || !object->binding || document.associations() != Associations::Known || object->family() != Family::PointMass)
    {
        return std::nullopt;
    }
    // Tracker's application loader shifts every track's frames by the clip's positive frame shift (#313); no command
    // edits the shift, so the document's clip holds the baseline's.
    const auto* clip = findSingleton(document.objects(), Role::Clip);
    const auto* clipState = clip ? std::get_if<ClipState>(clip->value.get()) : nullptr;
    const auto& source = *document.baseline();
    try
    {
        return pointMass(source, tracker::loadPointMass(source, *object->binding), clipState ? trackFrameShift(*clipState) : 0);
    }
    catch (const Refusal&)
    {
        return std::nullopt; // Unreachable: importing the same baseline typed this object.
    }
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
