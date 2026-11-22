#include "tracker_loaders.hpp"

#include <ranges>

namespace motrack::tracker
{

std::optional<osp::NodeId> childObject(const osp::Source& source, osp::NodeId object, std::string_view name)
{
    return osp::ObjectView(source, object).child(name).transform([](const osp::ObjectView& child) { return child.id(); });
}

std::optional<osp::NodeId> propertyNode(const osp::Source& source, osp::NodeId object, std::string_view name)
{
    return osp::ObjectView(source, object).property(name).transform([](const osp::PropertyView& property) { return property.id(); });
}

std::vector<ArrayElement> arrayElements(const osp::Source& source, osp::NodeId array)
{
    std::vector<ArrayElement> result;
    for (const auto& element : osp::PropertyView(source, array).elements())
    {
        result.push_back({.property = element.id(),
                          .index = element.elementIndex(),
                          .object = element.object().transform([](const osp::ObjectView& object) { return object.id(); })});
    }
    return result;
}

namespace
{

/// `getObject(name)` of an object array: the last property with the name, when it is an array.
std::optional<osp::NodeId> arrayNode(const osp::Source& source, osp::NodeId object, std::string_view name)
{
    const auto property = osp::ObjectView(source, object).property(name);
    if (!property || property->type() != osp::PropertyType::Array)
    {
        return std::nullopt;
    }
    return property->id();
}

template <typename Data, typename Decode>
std::optional<FrameArray<Data>> loadFrames(const osp::Source& source, osp::NodeId object, Decode decode)
{
    const auto array = arrayNode(source, object, "framedata");
    if (!array)
    {
        return std::nullopt;
    }
    FrameArray<Data> result{.property = *array, .className = std::string(source.node(*array).className), .frames = {}};
    for (auto& element : arrayElements(source, *array))
    {
        Frame<Data> frame{.element = element, .data = {}};
        if (element.object)
        {
            frame.data = decode(osp::ObjectView(source, *element.object));
        }
        result.frames.push_back(std::move(frame));
    }
    return result;
}

/// `(int[]) getObject(name)` (XMLControlElement.arrayValue): empty, a compact "array" string or `[n]` ints.
Read<std::vector<std::int32_t>> intArray(const osp::PropertyView& property)
{
    if (property.type() != osp::PropertyType::Array || property.className() != "[I")
    {
        return osp::makeUnexpected(osp::DecodeError::WrongType);
    }
    const auto elements = property.elements();
    if (elements.empty())
    {
        return std::vector<std::int32_t>{};
    }
    if (elements.front().name() == "array")
    {
        auto compact = property.compactArray();
        if (!compact)
        {
            return osp::makeUnexpected(compact.error());
        }
        return std::move(compact->ints);
    }
    const auto slots = property.indexedElements();
    if (!slots)
    {
        return osp::makeUnexpected(slots.error());
    }
    // Java allocates the whole array; refuse an extent no Tracker key list reaches instead of allocating it.
    constexpr std::size_t MAX_KEY_FRAMES = 1'000'000;
    if (slots->length > MAX_KEY_FRAMES)
    {
        return osp::makeUnexpected(osp::DecodeError::OutOfRange);
    }
    std::vector<std::int32_t> values(slots->length, 0); // Java leaves an absent int element at its default.
    for (const auto& [index, element] : slots->elements)
    {
        const auto value = element.intValue();
        if (!value)
        {
            return osp::makeUnexpected(value.error());
        }
        values[index] = *value;
    }
    return values;
}

VideoLoad loadVideo(const osp::Source& source, osp::NodeId object)
{
    const osp::ObjectView view(source, object);
    VideoLoad video{.object = object,
                    .className = std::string(view.className()),
                    .path = view.getString("path"),
                    .deltaT = view.getDouble("delta_t"),
                    .paths = std::nullopt,
                    .pathElements = {}};
    if (const auto paths = view.property("paths"); paths && paths->type() == osp::PropertyType::Array)
    {
        video.paths = paths->id();
        for (const auto& element : paths->elements())
        {
            video.pathElements.push_back({.element = {.property = element.id(), .index = element.elementIndex(), .object = std::nullopt},
                                          .value = element.stringValue()});
        }
    }
    return video;
}

VideoClipLoad loadVideoClip(const osp::Source& source, osp::NodeId object)
{
    const osp::ObjectView view(source, object);
    VideoClipLoad clip{.object = object,
                       .className = std::string(view.className()),
                       .startFrame = view.getInt("startframe"),
                       .stepSize = view.getInt("stepsize"),
                       .stepCount = view.getInt("stepcount"),
                       .frameCount = view.getInt("video_framecount"),
                       .frameShift = view.getInt("frameshift"),
                       .startTime = view.getDouble("starttime"),
                       .video = std::nullopt};
    if (const auto video = childObject(source, object, "video"))
    {
        clip.video = loadVideo(source, *video);
    }
    return clip;
}

ClipControlLoad loadClipControl(const osp::Source& source, osp::NodeId object)
{
    const osp::ObjectView view(source, object);
    return {.object = object,
            .className = std::string(view.className()),
            .rate = view.getDouble("rate"),
            .deltaT = view.getDouble("delta_t"),
            .frame = view.getInt("frame"),
            .looping = view.getBoolean("looping")};
}

} // namespace

CoordSystemLoad loadCoordSystem(const osp::Source& source, osp::NodeId object)
{
    const osp::ObjectView view(source, object);
    return {.object = object,
            .className = std::string(view.className()),
            .fixedOrigin = view.getBoolean("fixedorigin"),
            .fixedAngle = view.getBoolean("fixedangle"),
            .fixedScale = view.getBoolean("fixedscale"),
            .locked = view.getBoolean("locked"),
            .frames = loadFrames<CoordFrameData>(source,
                                                 object,
                                                 [](const osp::ObjectView& frame)
                                                 {
                                                     return CoordFrameData{.xorigin = frame.getDouble("xorigin"),
                                                                           .yorigin = frame.getDouble("yorigin"),
                                                                           .angle = frame.getDouble("angle"),
                                                                           .xscale = frame.getDouble("xscale"),
                                                                           .yscale = frame.getDouble("yscale")};
                                                 })};
}

PointMassLoad loadPointMass(const osp::Source& source, osp::NodeId object)
{
    const osp::ObjectView view(source, object);
    PointMassLoad track{
        .object = object,
        .name = view.getString("name"),
        .mass = view.getDouble("mass"),
        .frames = loadFrames<PointMassFrameData>(source,
                                                 object,
                                                 [](const osp::ObjectView& frame)
                                                 { return PointMassFrameData{.x = frame.getDouble("x"), .y = frame.getDouble("y")}; }),
        .keyFramesProperty = std::nullopt,
        .keyFrames = osp::makeUnexpected(osp::DecodeError::Missing)};
    if (const auto keys = view.property("keyFrames"))
    {
        track.keyFramesProperty = keys->id();
        track.keyFrames = intArray(*keys);
    }
    return track;
}

VectorLoad loadVector(const osp::Source& source, osp::NodeId object)
{
    const osp::ObjectView view(source, object);
    return {.object = object,
            .name = view.getString("name"),
            .frames = loadFrames<VectorFrameData>(source,
                                                  object,
                                                  [](const osp::ObjectView& frame)
                                                  {
                                                      return VectorFrameData{.xtail = frame.getDouble("xtail"),
                                                                             .ytail = frame.getDouble("ytail"),
                                                                             .xcomponent = frame.getDouble("xcomponent"),
                                                                             .ycomponent = frame.getDouble("ycomponent")};
                                                  })};
}

PanelLoad loadPanel(const osp::Source& source)
{
    const auto root = source.root();
    const osp::ObjectView view(source, root);
    PanelLoad panel{.object = root,
                    .className = std::string(view.className()),
                    .semanticVersion = view.getString("semantic_version"),
                    .description = view.getString("description"),
                    .lengthUnit = view.getString("length_unit"),
                    .massUnit = view.getString("mass_unit"),
                    .timeUnit = view.getString("time_unit"),
                    .referenceFrame = view.getString("referenceframe"),
                    .selectedTrack = view.getString("selectedtrack"),
                    .videoClip = std::nullopt,
                    .clipControl = std::nullopt,
                    .coords = std::nullopt,
                    .tracks = std::nullopt,
                    .trackItems = {}};
    if (const auto clip = childObject(source, root, "videoclip"))
    {
        panel.videoClip = loadVideoClip(source, *clip);
    }
    if (const auto control = childObject(source, root, "clipcontrol"))
    {
        panel.clipControl = loadClipControl(source, *control);
    }
    if (const auto coords = childObject(source, root, "coords"))
    {
        panel.coords = loadCoordSystem(source, *coords);
    }
    if (const auto tracks = view.property("tracks"))
    {
        panel.tracks = tracks->id();
        std::size_t ordinal = 0;
        for (const auto& element : tracks->elements())
        {
            const auto object = element.object();
            panel.trackItems.push_back({.ordinal = ordinal++,
                                        .property = element.id(),
                                        .object = object.transform([](const osp::ObjectView& item) { return item.id(); }),
                                        .className = object ? std::string(object->className()) : std::string{}});
        }
    }
    return panel;
}

} // namespace motrack::tracker
