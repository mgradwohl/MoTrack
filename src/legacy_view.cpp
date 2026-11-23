#include "legacy_view.hpp"

#include "model/tracker_loaders.hpp"
#include "tracker_import.hpp"

#include <algorithm>
#include <functional>
#include <ranges>
#include <set>
#include <unordered_map>
#include <utility>

namespace motrack
{
static_assert(model::CURRENT_READER_REVISION == std::to_underlying(CURRENT_TRACKER_READING),
              "The typed document and the legacy import read Tracker sources the same way.");

namespace
{

using model::ObjectSlot;
using model::Role;

template <typename State> const State* stateOf(const ObjectSlot* object)
{
    return object ? std::get_if<State>(object->value.get()) : nullptr;
}

/// Whether the file saves the property, readable or not: B1 keeps an unreadable `length_unit` as an empty label.
bool saved(const model::Read<std::string>& value)
{
    return value || value.error() != osp::DecodeError::Missing;
}

constexpr std::string_view DEFAULT_MASS_UNIT = "kg";
constexpr std::string_view DEFAULT_TIME_UNIT = "s";

/// TrackerPanel keeps its preferred length unit (SI "m") unless the file saves one (#214), and assigns a saved mass unit
/// as it is, an empty one included (#315); B1 shows either unreadable label empty.
std::string savedLabel(const model::Read<std::string>& value, std::string_view fallback)
{
    return saved(value) ? model::loadedName(value) : std::string(fallback);
}

/// VideoPanel.setTimeUnit keeps the default unless the file saves a non-empty time unit.
std::string timeLabel(const model::Read<std::string>& value)
{
    auto label = model::loadedName(value);
    return label.empty() ? std::string(DEFAULT_TIME_UNIT) : label;
}

/// The unit labels B1 shows for a panel. A bound panel shows Tracker's reading of its saved labels. An unbound one
/// holds MoTrack's labels (an empty document, or native-only objects without associations), which no Tracker loader
/// reads, so each shows as stored, an empty time unit included; only a missing one takes the default.
struct UnitLabels
{
    std::string length;
    std::string mass;
    std::string time;
};

UnitLabels unitLabels(const model::PanelState& panel, bool bound)
{
    if (!bound)
    {
        const auto stored = [](const model::Read<std::string>& value, std::string_view fallback)
        {
            return value ? *value : std::string(fallback);
        };
        return {.length = stored(panel.lengthUnit, tracker::DEFAULT_LENGTH_UNIT),
                .mass = stored(panel.massUnit, DEFAULT_MASS_UNIT),
                .time = stored(panel.timeUnit, DEFAULT_TIME_UNIT)};
    }
    return {.length = savedLabel(panel.lengthUnit, tracker::DEFAULT_LENGTH_UNIT),
            .mass = savedLabel(panel.massUnit, DEFAULT_MASS_UNIT),
            .time = timeLabel(panel.timeUnit)};
}

/// B1's frame duration: an image sequence or media-free clip takes its clip control's positive `delta_t`, else the
/// video's; without media, Tracker's stepper keeps 100 ms unless the clip control replaces it with a non-zero value
/// (#214). Movies have none here; their decoder times them.
std::optional<double> frameDuration(const ObjectSlot* video, const ObjectSlot* control)
{
    if (video && video->className != tracker::IMAGE_VIDEO_CLASS)
    {
        return std::nullopt;
    }
    const auto* controlState = stateOf<model::ClipControlState>(control);
    const auto controlDuration = controlState ? model::finiteValue(controlState->deltaT) : std::nullopt;
    auto duration = controlDuration;
    if (duration.value_or(0.0) <= 0.0)
    {
        const auto* videoState = stateOf<model::VideoState>(video);
        duration = videoState ? model::finiteValue(videoState->deltaT) : std::nullopt;
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

Calibration calibration(const model::CoordinatesState& coords)
{
    Calibration result{.keyframes = {},
                       .fixedOrigin = coords.fixedOrigin.value_or(false),
                       .fixedAngle = coords.fixedAngle.value_or(false),
                       .fixedScale = coords.fixedScale.value_or(false)};
    coords.keys.forEach(
        [&](std::int32_t frame, const model::FrameElement<model::CoordKey>& key)
        {
            result.keyframes.push_back({.frame = frame,
                                        .originX = key.value.xOrigin,
                                        .originY = key.value.yOrigin,
                                        .angleDegrees = key.value.angle,
                                        .scaleX = key.value.xScale,
                                        .scaleY = key.value.yScale});
        });
    return result;
}

PointMass pointMass(const model::PointMassState& state, model::ObjectId id)
{
    PointMass track{.name = model::loadedName(state.name), .marks = {}, .mass = model::loadedMass(state), .id = id.value};
    state.marks.forEach([&](std::int32_t frame, const model::FrameElement<model::Mark>& mark)
                        { track.marks.push_back({.frame = frame, .x = mark.value.x, .y = mark.value.y}); });
    return track;
}

VectorTrack vectorTrack(const model::VectorState& state, model::ObjectId id)
{
    VectorTrack track{.name = model::loadedName(state.name), .samples = {}, .id = id.value};
    state.samples.forEach(
        [&](std::int32_t frame, const model::FrameElement<model::VectorSample>& sample)
        {
            track.samples.push_back({.frame = frame,
                                     .tailX = sample.value.xTail,
                                     .tailY = sample.value.yTail,
                                     .componentX = sample.value.xComponent,
                                     .componentY = sample.value.yComponent});
        });
    return track;
}

/// The ordinal of a bound track among all direct children of the baseline's `tracks` property.
std::size_t trackOrdinal(const osp::Source& source, osp::NodeId object)
{
    const auto element = source.node(object).parent;
    if (!element)
    {
        throw ProjectError("A bound track is not an element of the Tracker tracks.");
    }
    const auto tracks = source.node(*element).parent;
    if (!tracks)
    {
        throw ProjectError("A bound track is not an element of the Tracker tracks.");
    }
    const auto& siblings = source.node(*tracks).children;
    return static_cast<std::size_t>(std::ranges::find(siblings, *element) - siblings.begin());
}

// Conversion from the reduced model.

ObjectSlot unbound(Role role,
                   std::string_view className,
                   model::TypedValue value,
                   model::AllocationRange range = model::AllocationRange::Countdown,
                   model::ObjectId id = {})
{
    return {.id = id,
            .range = range,
            .role = role,
            .className = std::string(className),
            .binding = std::nullopt,
            .state = model::ObjectState::Live,
            .value = std::make_shared<const model::TypedValue>(std::move(value))};
}

/// The elements of `items` keyed by frame, keeping each retained frame's node and every element equal to the
/// baseline's, so an unchanged chunk stays shared. The reduced model stores samples in strictly ascending frame order
/// (the native reader requires it); anything else is refused rather than merged.
template <typename T, typename Items, typename Convert>
model::FrameSeries<T> overlay(const model::FrameSeries<T>& baseline, const Items& items, Convert convert)
{
    if (std::ranges::adjacent_find(items, std::greater_equal<>{}, [](const auto& item) { return item.frame; }) != std::ranges::end(items))
    {
        throw ProjectError("The native project's samples are not in frame order.");
    }
    auto result = baseline;
    for (const auto& [frame, element] : baseline.entries())
    {
        if (!std::ranges::binary_search(items, frame, {}, [](const auto& item) { return item.frame; }))
        {
            result.erase(frame);
        }
    }
    for (const auto& item : items)
    {
        const auto value = convert(item);
        const auto* current = result.find(item.frame);
        if (!current || current->value != value)
        {
            result.set(item.frame, {.value = value, .node = current ? current->node : std::nullopt});
        }
    }
    return result;
}

model::Mark mark(const Mark& item)
{
    return {.x = item.x, .y = item.y};
}

model::VectorSample sample(const VectorSample& item)
{
    return {.xTail = item.tailX, .yTail = item.tailY, .xComponent = item.componentX, .yComponent = item.componentY};
}

model::CoordKey key(const CalibrationKey& item)
{
    return {.xOrigin = item.originX, .yOrigin = item.originY, .angle = item.angleDegrees, .xScale = item.scaleX, .yScale = item.scaleY};
}

template <typename T> void setIfChanged(model::Read<T>& field, const T& reading, const T& stored)
{
    if (reading != stored)
    {
        field = stored;
    }
}

void setIfChanged(model::Read<std::int32_t>& field, const std::optional<int>& reading, const std::optional<int>& stored)
{
    if (reading != stored)
    {
        field = stored ? model::Read<std::int32_t>(*stored) : model::missing<std::int32_t>();
    }
}

/// A bound point mass: the stored values of the fields that differ from the baseline's reading. Its key frames follow
/// B1's mark export (tracker_writer.cpp, patchMarks): the baseline's keys as PointMass.Loader reads them, without the
/// removed marks and with every changed or inserted one, so the typed state holds the keys B1 writes.
model::PointMassState pointMassState(const PointMass& track, const model::PointMassState& baseline)
{
    auto state = baseline;
    setIfChanged(state.name, model::loadedName(baseline.name), track.name);
    if (model::loadedMass(baseline) != track.mass)
    {
        state.mass = track.mass;
    }
    state.marks = overlay(baseline.marks, track.marks, mark);
    const auto baselineKeys = model::loadedKeyFrames(baseline);
    std::set<std::int32_t> keys(baselineKeys.begin(), baselineKeys.end());
    baseline.marks.forEach(
        [&](std::int32_t frame, const model::FrameElement<model::Mark>& /*element*/)
        {
            if (!state.marks.find(frame))
            {
                keys.erase(frame);
            }
        });
    state.marks.forEach(
        [&](std::int32_t frame, const model::FrameElement<model::Mark>& element)
        {
            if (const auto* original = baseline.marks.find(frame); !original || original->value != element.value)
            {
                keys.insert(frame);
            }
        });
    if (!std::ranges::equal(keys, baselineKeys))
    {
        state.keyFrames = std::vector<std::int32_t>(keys.begin(), keys.end());
    }
    return state;
}

model::VectorState vectorState(const VectorTrack& track, const model::VectorState& baseline)
{
    auto state = baseline;
    setIfChanged(state.name, model::loadedName(baseline.name), track.name);
    state.samples = overlay(baseline.samples, track.samples, sample);
    return state;
}

model::CoordinatesState coordinatesState(const Calibration& stored, const model::CoordinatesState& baseline)
{
    auto state = baseline;
    setIfChanged(state.fixedOrigin, baseline.fixedOrigin.value_or(false), stored.fixedOrigin);
    setIfChanged(state.fixedAngle, baseline.fixedAngle.value_or(false), stored.fixedAngle);
    setIfChanged(state.fixedScale, baseline.fixedScale.value_or(false), stored.fixedScale);
    state.keys = overlay(baseline.keys, stored.keyframes, key);
    return state;
}

// Objects MoTrack created, or the native-only objects of a document without associations, set every field they save.

/// A created point mass: every mark is a key frame, as B1 writes a new track's `keyFrames`; without marks it saves none.
model::PointMassState createdPointMass(const PointMass& track)
{
    model::PointMassState state{
        .name = track.name, .mass = track.mass, .marks = overlay(model::FrameSeries<model::Mark>{}, track.marks, mark)};
    if (!track.marks.empty())
    {
        std::vector<std::int32_t> keys;
        keys.reserve(track.marks.size());
        for (const auto& item : track.marks)
        {
            keys.push_back(item.frame);
        }
        state.keyFrames = std::move(keys);
    }
    return state;
}

model::VectorState createdVector(const VectorTrack& track)
{
    return {.name = track.name, .samples = overlay(model::FrameSeries<model::VectorSample>{}, track.samples, sample)};
}

model::CoordinatesState createdCoordinates(const Calibration& stored)
{
    return {.fixedOrigin = stored.fixedOrigin,
            .fixedAngle = stored.fixedAngle,
            .fixedScale = stored.fixedScale,
            .keys = overlay(model::FrameSeries<model::CoordKey>{}, stored.keyframes, key)};
}

/// Equal sources, with bindings compared as sets: a native file may store them in any order.
bool sameSource(const std::optional<TrackerSource>& left, const std::optional<TrackerSource>& right)
{
    if (left.has_value() != right.has_value() ||
        (left && (left->xml != right->xml || left->bindings.has_value() != right->bindings.has_value())))
    {
        return false;
    }
    if (!left || !left->bindings)
    {
        return true;
    }
    auto a = *left->bindings;
    auto b = *right->bindings;
    std::ranges::sort(a, {}, &TrackSourceBinding::sourceIndex);
    std::ranges::sort(b, {}, &TrackSourceBinding::sourceIndex);
    return a == b;
}

ObjectSlot* findRole(std::vector<ObjectSlot>& objects, Role role, model::Family family)
{
    const auto found =
        std::ranges::find_if(objects,
                             [&](const ObjectSlot& object)
                             { return object.role == role && object.state == model::ObjectState::Live && object.family() == family; });
    return found == objects.end() ? nullptr : &*found;
}

template <typename State> void replaceValue(ObjectSlot& object, State state)
{
    object.value = std::make_shared<const model::TypedValue>(std::move(state));
}

/// Applies the stored panel, clip, timing, calibration and media values to the singletons.
void overlaySingletons(model::DocumentParts& parts, const LegacyProject& project)
{
    auto& objects = parts.objects;
    auto* panelObject = findRole(objects, Role::Panel, model::Family::Panel);
    auto panel = std::get<model::PanelState>(*panelObject->value);
    setIfChanged(panel.semanticVersion, model::loadedName(panel.semanticVersion), project.version);
    setIfChanged(panel.description, model::loadedName(panel.description), project.description);
    const auto labels = unitLabels(panel, panelObject->binding.has_value());
    setIfChanged(panel.lengthUnit, labels.length, project.lengthUnit);
    setIfChanged(panel.massUnit, labels.mass, project.massUnit);
    setIfChanged(panel.timeUnit, labels.time, project.timeUnit);
    replaceValue(*panelObject, std::move(panel));

    const auto& stored = project.clip;
    auto* clipObject = findRole(objects, Role::Clip, model::Family::Clip);
    const model::ClipState clipReading = clipObject ? std::get<model::ClipState>(*clipObject->value) : model::ClipState{};
    auto clip = clipReading;
    if (model::loadedStartFrame(clipReading).value_or(0) != stored.firstFrame)
    {
        // The saved start that loads at the stored first frame (VideoClip.Loader subtracts the frame shift).
        const auto saved = std::int64_t{stored.firstFrame} + model::clipFrameShift(clipReading.frameShift);
        clip.startFrame = std::in_range<std::int32_t>(saved) ? model::Read<std::int32_t>(static_cast<std::int32_t>(saved))
                                                             : model::Read<std::int32_t>(stored.firstFrame);
    }
    setIfChanged(clip.stepSize, clipReading.stepSize.value_or(1), stored.stepSize);
    setIfChanged(clip.stepCount, clipReading.stepCount ? std::optional<int>(*clipReading.stepCount) : std::nullopt, stored.stepCount);
    setIfChanged(clip.startTime, model::finiteValue(clipReading.startTime).value_or(0.0), stored.startTimeMs);
    if (clipObject)
    {
        replaceValue(*clipObject, clip);
    }
    else if (clip != clipReading)
    {
        objects.push_back(unbound(Role::Clip, tracker::VIDEO_CLIP_CLASS, clip));
    }

    const auto* video = findRole(objects, Role::Video, model::Family::Video);
    auto* control = findRole(objects, Role::ClipControl, model::Family::ClipControl);
    if (frameDuration(video, control) != stored.frameDurationMs)
    {
        auto state = control ? std::get<model::ClipControlState>(*control->value) : model::ClipControlState{};
        state.deltaT = stored.frameDurationMs ? model::Read<double>(*stored.frameDurationMs) : model::missing<double>();
        if (control)
        {
            replaceValue(*control, state);
        }
        else
        {
            objects.push_back(unbound(Role::ClipControl, tracker::STEPPER_CLIP_CONTROL_CLASS, state));
        }
    }

    auto* coords = findRole(objects, Role::Coordinates, model::Family::Coordinates);
    const auto* coordsState = stateOf<model::CoordinatesState>(coords);
    const auto reading = coordsState ? std::optional(calibration(*coordsState)) : std::nullopt;
    if (reading != project.calibration && project.calibration)
    {
        if (coords)
        {
            replaceValue(*coords, coordinatesState(*project.calibration, *coordsState));
        }
        else
        {
            // As calibrating a document whose coordinate system B1 cannot present: a native-only object that standard
            // export refuses.
            objects.push_back(unbound(Role::Coordinates, tracker::COORD_SYSTEM_CLASS, createdCoordinates(*project.calibration)));
        }
    }
    parts.media = {.references = stored.mediaReferences, .sourceFrameCount = stored.sourceFrameCount};
}

/// Native-only typed objects for every stored value, and class-only placeholders for the unsupported tracks.
void nativeObjects(model::DocumentParts& parts, const LegacyProject& project)
{
    parts.objects.push_back(unbound(Role::Panel,
                                    tracker::TRACKER_PANEL_CLASS,
                                    model::PanelState{.semanticVersion = project.version,
                                                      .description = project.description,
                                                      .lengthUnit = project.lengthUnit,
                                                      .massUnit = project.massUnit,
                                                      .timeUnit = project.timeUnit}));
    const auto& clip = project.clip;
    parts.objects.push_back(
        unbound(Role::Clip,
                tracker::VIDEO_CLIP_CLASS,
                model::ClipState{.startFrame = clip.firstFrame,
                                 .stepSize = clip.stepSize,
                                 .stepCount = clip.stepCount ? model::Read<std::int32_t>(*clip.stepCount) : model::missing<std::int32_t>(),
                                 .startTime = clip.startTimeMs}));
    parts.objects.push_back(unbound(
        Role::ClipControl,
        tracker::STEPPER_CLIP_CONTROL_CLASS,
        model::ClipControlState{.deltaT = clip.frameDurationMs ? model::Read<double>(*clip.frameDurationMs) : model::missing<double>()}));
    if (project.calibration)
    {
        parts.objects.push_back(unbound(Role::Coordinates, tracker::COORD_SYSTEM_CLASS, createdCoordinates(*project.calibration)));
    }
    for (const auto& track : project.pointMasses)
    {
        parts.objects.push_back(unbound(
            Role::Track, tracker::POINT_MASS_CLASS, createdPointMass(track), model::AllocationRange::Counter, model::ObjectId{track.id}));
    }
    for (const auto& track : project.vectors)
    {
        parts.objects.push_back(
            unbound(Role::Track, tracker::VECTOR_CLASS, createdVector(track), model::AllocationRange::Counter, model::ObjectId{track.id}));
    }
    for (const auto& type : project.unsupportedTrackTypes)
    {
        parts.objects.push_back(unbound(Role::Track, type, model::UnknownState{}));
    }
    parts.media = {.references = clip.mediaReferences, .sourceFrameCount = clip.sourceFrameCount};
}

/// The baseline's typed import with the stored identities and values (native versions 4–6 with known bindings).
model::DocumentParts boundObjects(const LegacyProject& project, const std::string& xml, const std::vector<TrackSourceBinding>& bindings)
{
    auto parts = importTypedTracker(xml).parts();
    const auto& baseline = *parts.baseline;
    std::unordered_map<std::size_t, std::size_t> byOrdinal;
    for (std::size_t index = 0; index < parts.objects.size(); ++index)
    {
        const auto& object = parts.objects[index];
        if (object.role == Role::Track && object.range == model::AllocationRange::Counter && object.binding)
        {
            byOrdinal.emplace(trackOrdinal(baseline, *object.binding), index);
        }
    }
    std::unordered_map<TrackId, const PointMass*> masses;
    for (const auto& track : project.pointMasses)
    {
        masses.emplace(track.id, &track);
    }
    std::unordered_map<TrackId, const VectorTrack*> vectors;
    for (const auto& track : project.vectors)
    {
        vectors.emplace(track.id, &track);
    }
    std::unordered_map<TrackId, bool> bound;
    for (const auto& binding : bindings)
    {
        const auto found = byOrdinal.find(binding.sourceIndex);
        if (found == byOrdinal.end())
        {
            throw ProjectError("Incomplete Tracker source associations.");
        }
        auto& object = parts.objects[found->second];
        object.id = model::ObjectId{binding.trackId};
        bound.emplace(binding.trackId, true);
        if (const auto* mass = std::get_if<model::PointMassState>(object.value.get()))
        {
            if (const auto live = masses.find(binding.trackId); live != masses.end())
            {
                replaceValue(object, pointMassState(*live->second, *mass));
            }
            else
            {
                object.state = model::ObjectState::Removed;
            }
        }
        else if (const auto* vector = std::get_if<model::VectorState>(object.value.get()))
        {
            if (const auto live = vectors.find(binding.trackId); live != vectors.end())
            {
                replaceValue(object, vectorState(*live->second, *vector));
            }
            else
            {
                object.state = model::ObjectState::Removed;
            }
        }
    }
    for (const auto& track : project.pointMasses)
    {
        if (!bound.contains(track.id))
        {
            parts.objects.push_back(unbound(Role::Track,
                                            tracker::POINT_MASS_CLASS,
                                            createdPointMass(track),
                                            model::AllocationRange::Counter,
                                            model::ObjectId{track.id}));
        }
    }
    for (const auto& track : project.vectors)
    {
        if (!bound.contains(track.id))
        {
            parts.objects.push_back(unbound(
                Role::Track, tracker::VECTOR_CLASS, createdVector(track), model::AllocationRange::Counter, model::ObjectId{track.id}));
        }
    }
    // Countdown identities are derived again on each open until native v7 stores them (B2.5).
    for (auto& object : parts.objects)
    {
        if (object.range == model::AllocationRange::Countdown)
        {
            object.id = {};
        }
    }
    parts.countdownLowWater = model::COUNTDOWN_START;
    overlaySingletons(parts, project);
    return parts;
}

} // namespace

LegacyProject legacyProject(const model::TrackerDocument& document)
{
    const auto& panelObject = *document.singleton(Role::Panel);
    const auto& panel = std::get<model::PanelState>(*panelObject.value);
    auto labels = unitLabels(panel, panelObject.binding.has_value());
    LegacyProject project{.version = model::loadedName(panel.semanticVersion),
                          .description = model::loadedName(panel.description),
                          .clip = {},
                          .calibration = std::nullopt,
                          .lengthUnit = std::move(labels.length),
                          .massUnit = std::move(labels.mass),
                          .timeUnit = std::move(labels.time),
                          .pointMasses = {},
                          .vectors = {},
                          .unsupportedTrackTypes = document.unknownTrackClasses(),
                          .nextTrackId = document.nextObjectId()};
    auto& clip = project.clip;
    if (const auto* state = stateOf<model::ClipState>(document.singleton(Role::Clip)))
    {
        clip.firstFrame = model::loadedStartFrame(*state).value_or(clip.firstFrame);
        clip.stepSize = state->stepSize.value_or(clip.stepSize);
        if (state->stepCount)
        {
            clip.stepCount = *state->stepCount;
        }
        clip.startTimeMs = model::finiteValue(state->startTime).value_or(0.0);
    }
    clip.mediaReferences = document.media().references;
    clip.sourceFrameCount = document.media().sourceFrameCount;
    clip.frameDurationMs = frameDuration(document.singleton(Role::Video), document.singleton(Role::ClipControl));
    if (const auto* coords = stateOf<model::CoordinatesState>(document.singleton(Role::Coordinates)))
    {
        project.calibration = calibration(*coords);
    }
    for (const auto id : document.trackOrder())
    {
        const auto& object = *document.object(id);
        if (const auto* mass = std::get_if<model::PointMassState>(object.value.get()))
        {
            project.pointMasses.push_back(pointMass(*mass, id));
        }
        else if (const auto* vector = std::get_if<model::VectorState>(object.value.get()))
        {
            project.vectors.push_back(vectorTrack(*vector, id));
        }
    }
    return project;
}

std::optional<TrackerSource> trackerSourceView(const model::TrackerDocument& document)
{
    const auto& baseline = document.baseline();
    if (!baseline)
    {
        return std::nullopt;
    }
    TrackerSource source{.xml = std::string(baseline->bytes()), .bindings = std::nullopt};
    if (document.associations() != model::Associations::Known)
    {
        return source;
    }
    auto& bindings = source.bindings.emplace();
    for (const auto& object : document.objects())
    {
        const auto family = object.family();
        if (object.role == Role::Track && object.binding && (family == model::Family::PointMass || family == model::Family::Vector))
        {
            bindings.push_back({.trackId = object.id.value,
                                .sourceIndex = trackOrdinal(*baseline, *object.binding),
                                .kind = family == model::Family::PointMass ? TrackKind::PointMass : TrackKind::Vector});
        }
    }
    return source;
}

model::TrackerDocument importTypedTracker(std::string_view xml)
{
    auto source = detail::parseTrackerXml(xml);
    // The typed import refuses these too, in English; give B1's translated message (#315).
    if (const auto panel = tracker::loadPanel(*source); panel.className == tracker::TRACKER_PANEL_CLASS)
    {
        if (const auto problem = tracker::findUnloadable(*source, panel))
        {
            throw ProjectError(detail::unloadableMessage(*problem));
        }
    }
    auto document = model::TrackerDocument::importBaseline(std::move(source));
    if (!document)
    {
        throw ProjectError(document.error().message);
    }
    return std::move(*document);
}

model::TrackerDocument typedDocumentFromReduced(const LegacyProject& project,
                                                const std::optional<TrackerSource>& source,
                                                bool associationsDropped,
                                                TrackerReading reading)
{
    const auto convert = [&](bool bound)
    {
        model::DocumentParts parts;
        if (bound)
        {
            parts = boundObjects(project, source->xml, *source->bindings);
        }
        else
        {
            if (source)
            {
                parts.baseline = detail::parseTrackerXml(source->xml);
                parts.associations = associationsDropped || source->bindings ? model::Associations::Stale : model::Associations::Unknown;
            }
            nativeObjects(parts, project);
        }
        parts.readerRevision = std::to_underlying(reading);
        parts.trackOrder.clear();
        for (const auto& track : project.pointMasses)
        {
            parts.trackOrder.push_back(model::ObjectId{track.id});
        }
        for (const auto& track : project.vectors)
        {
            parts.trackOrder.push_back(model::ObjectId{track.id});
        }
        parts.nextObjectId = project.nextTrackId;
        auto document = model::TrackerDocument::assemble(std::move(parts));
        if (!document)
        {
            throw ProjectError("The native project cannot be read as a typed document: " + document.error().message);
        }
        auto expectedSource = source;
        if (expectedSource && !bound)
        {
            expectedSource->bindings.reset();
        }
        if (legacyProject(*document) != project || !sameSource(trackerSourceView(*document), expectedSource))
        {
            throw ProjectError("The native project's stored values cannot be represented as a typed document.");
        }
        return std::move(*document);
    };
    // Bindings bind typed objects to this build's reading of the baseline, so only values of that reading keep them.
    std::string boundFailure;
    if (source && source->bindings && !associationsDropped && reading == CURRENT_TRACKER_READING)
    {
        try
        {
            return convert(true);
        }
        catch (const ProjectError& error)
        {
            // The bindings match the current reader's point masses and vectors, but the stored model does not match
            // its typed import of the rest, as when MoTrack's former DOM reader saw an unknown track that OSP's reader
            // does not. Every stored value is kept, native-only, and the associations are stale (§3).
            boundFailure = error.what();
        }
    }
    try
    {
        return convert(false);
    }
    catch (const ProjectError& error)
    {
        if (boundFailure.empty())
        {
            throw;
        }
        throw ProjectError(std::string(error.what()) + " With its source associations: " + boundFailure);
    }
}

bool keepsRecordedAssociations(const model::TrackerDocument& document,
                               const std::optional<TrackerSource>& source,
                               bool associationsDropped,
                               TrackerReading reading)
{
    const bool recorded = source && source->bindings && !associationsDropped && reading == CURRENT_TRACKER_READING;
    return !recorded || document.associations() == model::Associations::Known;
}

std::shared_ptr<const LegacyProject> LegacyViewCache::view(const model::TrackerDocument& document, DocumentStamp stamp)
{
    if (!m_View || m_Stamp != stamp)
    {
        m_View = std::make_shared<const LegacyProject>(legacyProject(document));
        m_Stamp = stamp;
    }
    return m_View;
}

void LegacyViewCache::prime(DocumentStamp stamp, std::shared_ptr<const LegacyProject> view)
{
    m_Stamp = stamp;
    m_View = std::move(view);
}

void LegacyViewCache::clear() noexcept
{
    m_Stamp.reset();
    m_View.reset();
}

std::optional<DocumentStamp> LegacyViewCache::stamp() const noexcept
{
    return m_Stamp;
}

} // namespace motrack
