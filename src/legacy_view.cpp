#include "legacy_view.hpp"

#include "model/tracker_loaders.hpp"
#include "tracker_import.hpp"

#include <algorithm>
#include <ranges>
#include <unordered_map>
#include <utility>

namespace motrack
{
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

std::string lengthUnit(const model::PanelState& panel)
{
    return saved(panel.lengthUnit) ? model::loadedName(panel.lengthUnit) : std::string(tracker::DEFAULT_LENGTH_UNIT);
}

/// B1 keeps its default mass and time labels unless the file saves a non-empty one (#315 tracks the empty mass unit).
std::string labelOr(const model::Read<std::string>& value, std::string_view fallback)
{
    auto label = model::loadedName(value);
    return label.empty() ? std::string(fallback) : label;
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
/// baseline's, so an unchanged chunk stays shared.
template <typename T, typename Items, typename Convert>
model::FrameSeries<T> overlay(const model::FrameSeries<T>& baseline, const Items& items, Convert convert)
{
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

/// A bound point mass: the stored values of the fields that differ from the baseline's reading.
model::PointMassState pointMassState(const PointMass& track, const model::PointMassState& baseline)
{
    auto state = baseline;
    setIfChanged(state.name, model::loadedName(baseline.name), track.name);
    if (model::loadedMass(baseline) != track.mass)
    {
        state.mass = track.mass;
    }
    state.marks = overlay(baseline.marks, track.marks, mark);
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

model::PointMassState createdPointMass(const PointMass& track)
{
    return {.name = track.name, .mass = track.mass, .marks = overlay(model::FrameSeries<model::Mark>{}, track.marks, mark)};
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
    setIfChanged(panel.lengthUnit, lengthUnit(panel), project.lengthUnit);
    setIfChanged(panel.massUnit, labelOr(panel.massUnit, "kg"), project.massUnit);
    setIfChanged(panel.timeUnit, labelOr(panel.timeUnit, "s"), project.timeUnit);
    replaceValue(*panelObject, std::move(panel));

    const auto& stored = project.clip;
    auto* clipObject = findRole(objects, Role::Clip, model::Family::Clip);
    const model::ClipState clipReading = clipObject ? std::get<model::ClipState>(*clipObject->value) : model::ClipState{};
    auto clip = clipReading;
    setIfChanged(clip.startFrame, clipReading.startFrame.value_or(0), stored.firstFrame);
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
    const auto& panel = std::get<model::PanelState>(*document.singleton(Role::Panel)->value);
    LegacyProject project{.version = model::loadedName(panel.semanticVersion),
                          .description = model::loadedName(panel.description),
                          .clip = {},
                          .calibration = std::nullopt,
                          .lengthUnit = lengthUnit(panel),
                          .massUnit = labelOr(panel.massUnit, "kg"),
                          .timeUnit = labelOr(panel.timeUnit, "s"),
                          .pointMasses = {},
                          .vectors = {},
                          .unsupportedTrackTypes = document.unknownTrackClasses(),
                          .nextTrackId = document.nextObjectId()};
    auto& clip = project.clip;
    if (const auto* state = stateOf<model::ClipState>(document.singleton(Role::Clip)))
    {
        clip.firstFrame = state->startFrame.value_or(clip.firstFrame);
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
    auto document = model::TrackerDocument::importBaseline(detail::parseTrackerXml(xml));
    if (!document)
    {
        throw ProjectError(document.error().message);
    }
    return std::move(*document);
}

model::TrackerDocument
typedDocumentFromReduced(const LegacyProject& project, const std::optional<TrackerSource>& source, bool bindingsReset)
{
    model::DocumentParts parts;
    if (source && source->bindings)
    {
        parts = boundObjects(project, source->xml, *source->bindings);
    }
    else
    {
        if (source)
        {
            parts.baseline = detail::parseTrackerXml(source->xml);
            parts.associations = bindingsReset ? model::Associations::Stale : model::Associations::Unknown;
        }
        nativeObjects(parts, project);
    }
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
    if (expectedSource && bindingsReset)
    {
        expectedSource->bindings.reset();
    }
    if (legacyProject(*document) != project || !sameSource(trackerSourceView(*document), expectedSource))
    {
        throw ProjectError("The native project's stored values cannot be represented as a typed document.");
    }
    return std::move(*document);
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
