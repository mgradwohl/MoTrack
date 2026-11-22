#include "tracker_compose.hpp"

#include "legacy_view.hpp"
#include "measurements.hpp"
#include "model/codecs.hpp"
#include "model/dependencies/name_binding.hpp"
#include "model/descriptors.hpp"
#include "model/source_edits.hpp"
#include "osp/java_text.hpp"
#include "osp/value.hpp"
#include "tracker_import.hpp"
#include "tracker_rename.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <numbers>
#include <ranges>
#include <set>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace motrack
{
namespace
{

using osp::NodeId;
using osp::PropertyType;

void require(bool valid)
{
    if (!valid)
    {
        throw ProjectError("This Tracker export cannot preserve all edits or source associations.");
    }
}

[[nodiscard]] const osp::Node& node(const osp::Source& xml, NodeId id)
{
    return xml.node(id);
}

/// The value of an optional the export requires.
template <typename T> T required(const std::optional<T>& value)
{
    require(value.has_value());
    return value.value_or(T{});
}

/// XMLControlElement's lookup: the last property with this name, if any.
std::optional<NodeId> lastProperty(const osp::Source& xml, NodeId owner, std::string_view name)
{
    const auto& children = node(xml, owner).children;
    for (const auto child : std::views::reverse(children))
    {
        if (node(xml, child).name == name)
        {
            return child;
        }
    }
    return std::nullopt;
}

/// The only property with this name, for the explicitly audited schemas that refuse duplicates.
std::optional<NodeId> uniqueProperty(const osp::Source& xml, NodeId owner, std::string_view name)
{
    std::optional<NodeId> result;
    for (const auto child : node(xml, owner).children)
    {
        if (node(xml, child).name == name)
        {
            require(!result);
            result = child;
        }
    }
    return result;
}

/// The object of an object property, if it is not null.
std::optional<NodeId> objectOf(const osp::Source& xml, std::optional<NodeId> property)
{
    if (!property || node(xml, *property).type != PropertyType::Object || node(xml, *property).children.empty())
    {
        return std::nullopt;
    }
    return node(xml, *property).children.front();
}

std::string uniqueText(const osp::Source& xml, NodeId owner, std::string_view name)
{
    const auto property = uniqueProperty(xml, owner, name);
    return property ? node(xml, *property).content : std::string{};
}

std::optional<double> javaDouble(std::string_view text)
{
    const auto value = osp::parseJavaDouble(text);
    return value ? std::optional(*value) : std::nullopt;
}

std::optional<std::int32_t> javaInt(std::string_view text)
{
    const auto value = osp::parseJavaInt(text);
    return value ? std::optional(*value) : std::nullopt;
}

/// Sets an existing typed scalar (Tracker reads the last duplicate) or appends a new one in OSP's writer layout.
/// A value that already decodes to the intended one keeps its original text.
void patchDouble(SourceEdits& edits, NodeId object, std::string_view name, double value)
{
    if (const auto property = lastProperty(edits.source(), object, name))
    {
        require(node(edits.source(), *property).type == PropertyType::Double);
        if (const auto current = javaDouble(node(edits.source(), *property).content);
            !current || std::bit_cast<std::uint64_t>(*current) != std::bit_cast<std::uint64_t>(value))
            edits.setDouble(*property, value);
    }
    else
    {
        edits.append(object, osp::doubleProperty(std::string(name), value));
    }
}

void patchInt(SourceEdits& edits, NodeId object, std::string_view name, std::int32_t value)
{
    if (const auto property = lastProperty(edits.source(), object, name))
    {
        require(node(edits.source(), *property).type == PropertyType::Int);
        if (javaInt(node(edits.source(), *property).content) != value)
            edits.setInt(*property, value);
    }
    else
    {
        edits.append(object, osp::intProperty(std::string(name), value));
    }
}

void patchString(SourceEdits& edits, NodeId object, std::string_view name, const std::string& value)
{
    // OSP cannot represent "]]></property>" or a nested CDATA marker; MoTrack's labels exclude "]]>" altogether.
    require(value.find("]]>") == std::string::npos);
    if (const auto property = lastProperty(edits.source(), object, name))
    {
        require(node(edits.source(), *property).type == PropertyType::String);
        if (const auto current = osp::PropertyView(edits.source(), *property).stringValue(); !current || *current != value)
            edits.setString(*property, value);
    }
    else
    {
        edits.append(object, osp::stringProperty(std::string(name), value));
    }
}

// Admission checks and patches. Each check encodes a Tracker behavior and runs before its patch, which only edits;
// B1's composition and the descriptor composition run the same checks in the same order.

/// A unit label is a name codec value (admission).
void checkUnit(const std::string& unit)
{
    require(model::isTrackerUnitLabel(unit));
}

void patchUnit(SourceEdits& edits, std::string_view name, const std::string& unit)
{
    checkUnit(unit);
    patchString(edits, edits.source().root(), name, unit);
}

/// The panel's VideoClip object, audited as the only clip representation the writers patch.
NodeId videoClipObject(const osp::Source& xml)
{
    const auto property = lastProperty(xml, xml.root(), "videoclip");
    const auto object = required(objectOf(xml, property));
    require(node(xml, object).className == tracker::VIDEO_CLIP_CLASS);
    return object;
}

/// The clip range Tracker can retain (admission); returns the VideoClip object the range is written to.
NodeId checkClipRange(const osp::Source& xml, const Clip& clip, LinkedMedia media)
{
    if (clip.frameDurationMs)
    {
        auto timing = clip;
        // Audited media use the same constant interval arithmetic. This private
        // numeric check does not change the saved source inventory or enable timing edits.
        if (media != LinkedMedia::None)
            timing.mediaReferences.clear();
        require(validConstantClipTiming(timing));
    }
    const auto count = clip.stepCount.value_or(0);
    require(clip.firstFrame >= 0 && clip.stepSize > 0 && count > 0);
    const auto end = static_cast<std::int64_t>(clip.firstFrame) + static_cast<std::int64_t>(count - 1) * clip.stepSize;
    // Tracker 6.3.5 clamps media-free clips at frame 500000 and loads a temporary frame count
    // of start + count * stride. Reject values its Java int arithmetic cannot retain.
    require(end <= 500000 && end + clip.stepSize <= std::numeric_limits<int>::max());
    require(!clip.sourceFrameCount || end < *clip.sourceFrameCount);
    const auto object = videoClipObject(xml);
    require(!lastProperty(xml, object, "frameshift"));
    if (media != LinkedMedia::None)
    {
        // The media schema, complete frame inventory/index and optional saved count were
        // checked before patching. Trimming changes only the clip, never the source media.
        const auto video = required(objectOf(xml, lastProperty(xml, object, "video")));
        require(node(xml, video).className == (media == LinkedMedia::Images ? tracker::IMAGE_VIDEO_CLASS : tracker::XUGGLE_VIDEO_CLASS));
    }
    else
        require(!lastProperty(xml, object, "video") && !lastProperty(xml, object, "video_framecount"));
    return object;
}

/// Tracker's clip range is one batch: its three fields are written together, absent ones included.
void patchClipRange(SourceEdits& edits, NodeId clip, std::int32_t start, std::int32_t step, std::int32_t count)
{
    patchInt(edits, clip, "startframe", start);
    patchInt(edits, clip, "stepsize", step);
    patchInt(edits, clip, "stepcount", count);
}

void patchClipRange(SourceEdits& edits, const Clip& clip, LinkedMedia media)
{
    const auto object = checkClipRange(edits.source(), clip, media);
    patchClipRange(edits, object, clip.firstFrame, clip.stepSize, clip.stepCount.value_or(0));
}

struct TimingObjects
{
    NodeId clip{};
    NodeId control{};
};

/// Constant clip timing of a media-free experiment whose clip and clip control hold only audited fields (admission).
TimingObjects checkClipTiming(const osp::Source& xml, const LegacyProject& project)
{
    const auto& clip = project.clip;
    require(validConstantClipTiming(clip));
    // Data tracks can supply their own time source; models require independent trajectory checks.
    require(supportsConstantClipTimingEdits(project));
    const auto clipObject = videoClipObject(xml);
    const std::set<std::string_view> clipFields{"startframe", "stepsize", "stepcount", "starttime", "readout", "playallsteps"};
    std::set<std::string> seen;
    for (const auto field : node(xml, clipObject).children)
    {
        const auto& item = node(xml, field);
        const auto name = required(item.name);
        require(clipFields.contains(name) && seen.insert(name).second);
        if (name == "starttime")
        {
            const auto value = javaDouble(item.content);
            require(item.type == PropertyType::Double && value && std::isfinite(*value));
        }
        else if (name == "readout")
        {
            require(item.type == PropertyType::String);
        }
        else if (name == "playallsteps")
        {
            require(item.type == PropertyType::Boolean && (item.content == "true" || item.content == "false"));
        }
        else
        {
            const auto value = javaInt(item.content);
            require(item.type == PropertyType::Int && value && *value >= (name == "startframe" ? 0 : 1));
        }
    }
    const auto control = required(objectOf(xml, lastProperty(xml, xml.root(), "clipcontrol")));
    require(node(xml, control).className == tracker::STEPPER_CLIP_CONTROL_CLASS);
    const std::set<std::string_view> controlFields{"rate", "delta_t", "frame", "looping"};
    seen.clear();
    for (const auto field : node(xml, control).children)
    {
        const auto& item = node(xml, field);
        const auto name = required(item.name);
        require(controlFields.contains(name) && seen.insert(name).second);
        if (name == "looping")
        {
            require(item.type == PropertyType::Boolean && (item.content == "true" || item.content == "false"));
        }
        else if (name == "frame")
        {
            const auto frame = javaInt(item.content);
            require(item.type == PropertyType::Int && frame && *frame >= 0 && *frame <= 500000);
        }
        else
        {
            const auto number = javaDouble(item.content);
            require(item.type == PropertyType::Double && number && std::isfinite(*number) && *number > 0.0);
        }
    }
    require(seen.contains("rate") && seen.contains("delta_t"));
    return {.clip = clipObject, .control = control};
}

/// Clip timing is one batch too: the clip's start time and the clip control's frame duration.
void patchClipTiming(SourceEdits& edits, const TimingObjects& objects, double startTime, double frameDuration)
{
    patchDouble(edits, objects.clip, "starttime", startTime);
    patchDouble(edits, objects.control, "delta_t", frameDuration);
}

void patchClipTiming(SourceEdits& edits, const LegacyProject& project)
{
    const auto objects = checkClipTiming(edits.source(), project);
    patchClipTiming(edits, objects, project.clip.startTimeMs, required(project.clip.frameDurationMs));
}

std::optional<int> frameIndex(const std::optional<std::string>& name)
{
    if (!name || name->size() < 3 || name->front() != '[' || name->back() != ']')
    {
        return std::nullopt;
    }
    const auto index = osp::parseJavaInt(std::string_view(*name).substr(1, name->size() - 2));
    return index && *index >= 0 ? std::optional<int>(*index) : std::nullopt;
}

int requireFrameIndex(const osp::Source& xml, NodeId item)
{
    const auto index = frameIndex(node(xml, item).name);
    require(index.has_value());
    return index.value_or(0);
}

/// The audited object array, or none when the object does not save it.
std::optional<NodeId> frameArray(const osp::Source& xml, NodeId object, std::string_view name, std::string_view className)
{
    const auto array = lastProperty(xml, object, name);
    if (array)
    {
        require(node(xml, *array).type == PropertyType::Array && node(xml, *array).className == className);
    }
    return array;
}

std::set<int> readKeys(const osp::Source& xml, NodeId object, const std::vector<Mark>& original)
{
    std::set<int> result;
    if (const auto keys = lastProperty(xml, object, "keyFrames"))
    {
        const auto& container = node(xml, *keys);
        require(container.type == PropertyType::Array && container.className == "[I");
        if (!container.children.empty())
        {
            // OSP writes a nonempty int array as one compact string; a zero-length array has no element.
            const auto& values = node(xml, container.children.front());
            require(container.children.size() == 1 && values.name == "array" && values.type == PropertyType::String);
            const auto decoded = osp::parseCompactArray(values.content, "[I");
            require(decoded && decoded->kind == osp::CompactArray::Kind::Int && !decoded->ints.empty());
            for (const auto frame : decoded->ints)
            {
                require(frame >= 0);
                result.insert(frame);
            }
        }
    }
    // Tracker treats missing/empty key arrays as all existing marks being manual keys.
    if (result.empty())
    {
        for (const auto& mark : original)
        {
            result.insert(mark.frame);
        }
    }
    return result;
}

osp::PropertyValue keyFramesValue(const std::set<int>& keys)
{
    osp::CompactArray values{.kind = osp::CompactArray::Kind::Int};
    values.ints.assign(keys.begin(), keys.end());
    auto property = osp::primitiveArrayProperty("keyFrames", "[I", values);
    require(property.has_value());
    return property ? std::move(*property) : osp::PropertyValue{};
}

struct FrameChanges
{
    std::vector<int> updated;
    std::vector<int> removed;
};

/// A new `[frame]` element holding a FrameData object with the given properties, as OSP writes it.
osp::PropertyValue frameElement(int frame, std::string_view className, std::vector<osp::PropertyValue> properties)
{
    return osp::objectProperty(
        "[" + std::to_string(frame) + "]",
        osp::ObjectValue{.className = std::string(className), .version = std::nullopt, .properties = std::move(properties)});
}

/// The complete frame array of samples, as OSP writes a FrameData[] (no null elements are needed).
template <typename Sample, typename Fill>
osp::PropertyValue frameArrayValue(std::string_view name, std::string_view className, const std::vector<Sample>& samples, Fill fill)
{
    osp::PropertyValue array{.name = std::string(name), .typeName = "array", .className = "[L" + std::string(className) + ";"};
    int previous = -1;
    for (const auto& sample : samples)
    {
        require(sample.frame > previous);
        previous = sample.frame;
        array.elements.push_back(frameElement(sample.frame, className, fill(sample)));
    }
    return array;
}

template <typename Sample, typename Fill, typename Update>
FrameChanges patchFrameData(SourceEdits& edits,
                            NodeId object,
                            const std::vector<Sample>& original,
                            const std::vector<Sample>& current,
                            std::string_view className,
                            Fill fill,
                            Update update)
{
    const auto& xml = edits.source();
    std::map<int, const Sample*> marks;
    int previous = -1;
    for (const auto& mark : current)
    {
        require(mark.frame > previous);
        marks.emplace(mark.frame, &mark);
        previous = mark.frame;
    }
    FrameChanges changes;
    const auto arrayClass = "[L" + std::string(className) + ";";
    const auto data = frameArray(xml, object, "framedata", arrayClass);
    if (!data)
    {
        require(original.empty());
        for (const auto& [frame, mark] : marks)
        {
            changes.updated.push_back(frame);
        }
        edits.append(object, frameArrayValue("framedata", className, current, fill));
        return changes;
    }
    // Map indices to the element ordinals that insertions refer to.
    std::map<int, std::pair<NodeId, std::size_t>> entries;
    int lastEntry = -1;
    const auto& children = node(xml, *data).children;
    for (std::size_t ordinal = 0; ordinal < children.size(); ++ordinal)
    {
        const auto frame = requireFrameIndex(xml, children[ordinal]);
        require(frame > lastEntry && entries.emplace(frame, std::pair(children[ordinal], ordinal)).second);
        lastEntry = frame;
    }
    for (const auto& mark : original)
    {
        const auto found = marks.find(mark.frame);
        if (found == marks.end())
        {
            edits.remove(entries.at(mark.frame).first);
            entries.erase(mark.frame);
            changes.removed.push_back(mark.frame);
        }
        else if (*found->second == mark)
        {
            marks.erase(found); // Keep untouched frame objects and their unknown properties.
        }
    }
    for (const auto& [frame, mark] : marks)
    {
        if (const auto entry = entries.find(frame); entry != entries.end())
        {
            const auto [item, ordinal] = entry->second;
            require(node(xml, item).type == PropertyType::Object);
            if (const auto step = objectOf(xml, item))
            {
                require(node(xml, *step).className == className);
                update(edits, *step, *mark);
            }
            else
            {
                // A null placeholder becomes the sample's object in the same array position.
                edits.remove(item);
                edits.insert(*data, ordinal, frameElement(frame, className, fill(*mark)));
            }
        }
        else
        {
            const auto next = entries.upper_bound(frame);
            const auto position = next == entries.end() ? children.size() : next->second.second;
            edits.insert(*data, position, frameElement(frame, className, fill(*mark)));
        }
        changes.updated.push_back(frame);
    }
    return changes;
}

std::vector<osp::PropertyValue> markProperties(const Mark& mark)
{
    require(std::isfinite(mark.x) && std::isfinite(mark.y));
    return {osp::doubleProperty("x", mark.x), osp::doubleProperty("y", mark.y)};
}

/// A point mass's marks through the frame-array patcher.
FrameChanges patchMarkFrames(SourceEdits& edits, NodeId object, const std::vector<Mark>& original, const std::vector<Mark>& current)
{
    return patchFrameData(edits,
                          object,
                          original,
                          current,
                          tracker::POINT_MASS_FRAME_CLASS,
                          markProperties,
                          [](SourceEdits& document, NodeId step, const Mark& mark)
                          {
                              require(std::isfinite(mark.x) && std::isfinite(mark.y));
                              patchDouble(document, step, "x", mark.x);
                              patchDouble(document, step, "y", mark.y);
                          });
}

/// An empty key list makes Tracker promote every remaining automatic mark to a manual key (admission). Until that
/// distinction is in the editable model, edits that cannot retain it are refused.
void checkKeyFrames(const std::set<int>& keys, bool marked)
{
    require(!keys.empty() || !marked);
}

void patchKeyFrames(SourceEdits& edits, NodeId object, const std::set<int>& keys);

/// Mark edits keep the key frames with the marks they change (admission): in the key frames Tracker loads, every changed
/// or new mark is a manual key and no removed one stays a key, as B1 computes them. Otherwise Tracker would reload a
/// manual mark as an automatic one, or keep a key without its mark.
void checkKeyFramesFollowMarks(const std::set<int>& keys, const FrameChanges& changes, const std::string& track)
{
    const auto unkeyed = std::ranges::any_of(changes.updated, [&](int frame) { return !keys.contains(frame); });
    const auto stale = std::ranges::any_of(changes.removed, [&](int frame) { return keys.contains(frame); });
    if (unkeyed || stale)
    {
        throw ProjectError("Tracker export cannot keep the key frames of '" + track +
                           "': " + (unkeyed ? "a changed or new mark is not a key frame." : "a removed mark is still a key frame."));
    }
}

void patchMarks(SourceEdits& edits, NodeId object, const std::vector<Mark>& original, const std::vector<Mark>& current)
{
    const auto& xml = edits.source();
    auto keys = readKeys(xml, object, original);
    const auto oldKeys = keys;
    const auto changes = patchMarkFrames(edits, object, original, current);
    for (const auto frame : changes.removed)
    {
        keys.erase(frame);
    }
    keys.insert(changes.updated.begin(), changes.updated.end());
    checkKeyFrames(keys, !current.empty());
    if (keys == oldKeys)
    {
        return;
    }
    patchKeyFrames(edits, object, keys);
}

/// Writes a point mass's `keyFrames` as OSP writes an int[]: one compact string, or no element when it is empty.
void patchKeyFrames(SourceEdits& edits, NodeId object, const std::set<int>& keys)
{
    const auto& xml = edits.source();
    const auto container = lastProperty(xml, object, "keyFrames");
    if (!container)
    {
        edits.append(object, keyFramesValue(keys));
        return;
    }
    const auto& elements = node(xml, *container).children;
    if (keys.empty())
    {
        // OSP cannot decode the compact string "{}" as a zero-length int array; an empty array has no element.
        for (const auto element : elements)
        {
            edits.remove(element);
        }
        return;
    }
    osp::CompactArray values{.kind = osp::CompactArray::Kind::Int};
    values.ints.assign(keys.begin(), keys.end());
    const auto text = osp::formatCompactArray(values);
    if (elements.empty())
    {
        edits.append(*container, osp::stringProperty("array", text));
    }
    else
    {
        edits.setString(elements.front(), text);
    }
}

std::array<double, 6> trackerInverse(const CalibrationKey& key)
{
    // ImageCoordSystem normalizes sine/cosine, builds this image affine matrix, then uses
    // Java 17 AffineTransform.createInverse(). Algebraically simplified translations can
    // hide overflowing intermediate products; check the same diagonal/shear/general cases.
    const auto angle = key.angleDegrees * std::numbers::pi / 180.0;
    const auto rawCos = std::cos(angle);
    const auto rawSin = std::sin(angle);
    const auto norm = std::sqrt(rawCos * rawCos + rawSin * rawSin);
    const auto cos = rawCos / norm;
    const auto sin = rawSin / norm;
    const auto a = key.scaleX * cos;
    const auto b = -key.scaleY * sin;
    const auto c = -key.scaleX * sin;
    const auto d = -key.scaleY * cos;
    std::array<double, 6> inverse{};
    if (b == 0 && c == 0)
    {
        require(a != 0 && d != 0);
        inverse = {1 / a, 0, 0, 1 / d, -key.originX / a, -key.originY / d};
    }
    else if (a == 0 && d == 0)
    {
        require(b != 0 && c != 0);
        inverse = {0, 1 / c, 1 / b, 0, -key.originY / b, -key.originX / c};
    }
    else
    {
        // Keep multiplication and subtraction separate, as Java's strict floating point does.
        const auto diagonal = a * d;
        const auto shear = b * c;
        const auto determinant = diagonal - shear;
        require(std::isfinite(determinant) && std::abs(determinant) > std::numeric_limits<double>::denorm_min());
        const auto cx = c * key.originY;
        const auto dx = d * key.originX;
        const auto by = b * key.originX;
        const auto ay = a * key.originY;
        inverse = {d / determinant, -b / determinant, -c / determinant, a / determinant, (cx - dx) / determinant, (by - ay) / determinant};
    }
    const std::array expected{cos / key.scaleX,
                              -sin / key.scaleX,
                              -sin / key.scaleY,
                              -cos / key.scaleY,
                              -cos * key.originX / key.scaleX + sin * key.originY / key.scaleY,
                              sin * key.originX / key.scaleX + cos * key.originY / key.scaleY};
    for (std::size_t index = 0; index < inverse.size(); ++index)
    {
        const auto tolerance = std::max(1e-10, 1e-12 * std::max(std::abs(inverse[index]), std::abs(expected[index])));
        require(std::isfinite(inverse[index]) && std::isfinite(expected[index]) && std::abs(inverse[index] - expected[index]) <= tolerance);
    }
    return inverse;
}

/// Every object node of the class in the document, in file order.
std::vector<NodeId> objectsOfClass(const osp::Source& xml, std::string_view className)
{
    std::vector<NodeId> result;
    for (std::size_t index = 0; index < xml.nodeCount(); ++index)
    {
        const auto id = static_cast<NodeId>(index);
        if (node(xml, id).kind == osp::NodeKind::Object && node(xml, id).className == className)
        {
            result.push_back(id);
        }
    }
    return result;
}

void requireBoundedMassFrames(const osp::Source& xml)
{
    std::size_t frameSlots = 0;
    const auto arrayClass = "[L" + std::string(tracker::POINT_MASS_FRAME_CLASS) + ";";
    for (std::size_t index = 0; index < xml.nodeCount(); ++index)
    {
        const auto& data = node(xml, static_cast<NodeId>(index));
        if (data.kind != osp::NodeKind::Property || data.type != PropertyType::Array || data.className != arrayClass)
            continue;
        int lastFrame = -1;
        for (const auto item : data.children)
        {
            const auto frame = requireFrameIndex(xml, item);
            require(frame <= 500000);
            lastFrame = std::max(lastFrame, frame);
        }
        // Include inherited arrays on retained objects such as CenterOfMass, even when those
        // objects are not natively editable. Reimport and null placeholders cannot reset the budget.
        frameSlots += static_cast<std::size_t>(lastFrame + 1);
        require(frameSlots <= 1000000);
    }
}

/// The only `[n]` cell of a string-array row, or none.
std::optional<std::string> cell(const osp::Source& xml, NodeId row, int index)
{
    const auto property = uniqueProperty(xml, row, "[" + std::to_string(index) + "]");
    return property ? std::optional(node(xml, *property).content) : std::nullopt;
}

void requireStableTableColumns(const osp::Source& xml, const LegacyProject& project, bool movie)
{
    std::unordered_set<std::string> massNames;
    for (const auto& mass : project.pointMasses)
        massNames.insert(mass.name);
    const auto masses = objectsOfClass(xml, tracker::POINT_MASS_CLASS);
    for (const auto object : masses)
    {
        if (uniqueProperty(xml, object, "number_formats"))
        {
            // This newly supported metadata requires the audited format grammar even for a
            // mass-only edit, which otherwise permits opaque source content to pass through.
            if (movie)
                validateTrackerMovieEditSchema(xml);
            else
                validateTrackerEditSchema(xml);
            break;
        }
    }
    const auto defaultPattern = [](const std::string& variable, const std::string& pattern)
    {
        return ((variable == "frame" || variable == "step") && pattern == "0") || (variable == "t" && pattern == "0.000") ||
               pattern.empty();
    };
    for (const auto object : masses)
    {
        const auto owner = uniqueText(xml, object, "name");
        const auto formats = uniqueProperty(xml, object, "number_formats");
        if (!formats)
            continue;
        const auto& cells = node(xml, *formats).children;
        for (std::size_t index = 0; index < cells.size(); index += 2)
        {
            require(index + 1 < cells.size());
            if (!defaultPattern(node(xml, cells[index]).content, node(xml, cells[index + 1]).content))
                throw ProjectError("Tracker cannot retain custom number formats for '" + owner + "' (MoTrack issue #64).");
        }
    }
    for (const auto object : objectsOfClass(xml, "org.opensourcephysics.cabrillo.tracker.TableTView"))
    {
        if (const auto columns = uniqueProperty(xml, object, "track_columns"))
        {
            for (const auto row : node(xml, *columns).children)
            {
                const auto owner = cell(xml, row, 0).value_or(std::string{});
                if (!massNames.contains(owner) || cell(xml, row, 1) != "t")
                    continue;
                const auto second = cell(xml, row, 2).value_or(std::string{});
                const auto third = cell(xml, row, 3);
                const bool subset = second == "x" && !third;
                const bool defaults = !cell(xml, row, 4) && ((second == "x" && third == "y") || (second == "y" && third == "x"));
                // Pinned 6.3.5 can serialize t,x as null columns, or omit a default-column view's
                // custom formats/order. Preserve these source rows natively until a safe reference
                // workflow has evidence; do not let a changed standard save imply fidelity.
                const bool reordered = second == "y" && third == "x" && cell(xml, row, 4) == "frame" && !cell(xml, row, 5);
                if (subset || defaults || reordered)
                    throw ProjectError("Tracker cannot safely resave the table columns for '" + owner + "' (MoTrack issue #64).");
            }
        }
        if (const auto formats = uniqueProperty(xml, object, "column_formats"))
        {
            for (const auto group : node(xml, *formats).children)
            {
                for (const auto row : node(xml, group).children)
                {
                    const auto owner = cell(xml, row, 0).value_or(std::string{});
                    if (!massNames.contains(owner))
                        continue;
                    if (!defaultPattern(cell(xml, row, 1).value_or(std::string{}), cell(xml, row, 2).value_or(std::string{})))
                        throw ProjectError("Tracker cannot retain custom table formats for '" + owner + "' (MoTrack issue #64).");
                }
            }
        }
    }
}

void requireTrackerCoordinates(const LegacyProject& project, const Calibration& calibration)
{
    require(!calibration.keyframes.empty() && calibration.keyframes.front().frame == 0);
    std::vector<std::array<double, 6>> inverses;
    int previous = -1;
    for (const auto& key : calibration.keyframes)
    {
        require(key.frame > previous && key.frame <= 500000 && std::isfinite(key.originX) && std::isfinite(key.originY) &&
                std::isfinite(key.angleDegrees) && std::abs(key.angleDegrees) <= 180 && std::isfinite(key.scaleX) &&
                std::isfinite(key.scaleY) && key.scaleX != 0 && key.scaleY != 0);
        const auto determinant = key.scaleX * key.scaleY;
        require(std::isfinite(determinant) && determinant != 0);
        previous = key.frame;
    }
    auto componentCalibration = calibration;
    for (auto& key : componentCalibration.keyframes)
    {
        // Fixed origin/angle use the last loaded key, while fixed scale uses the first.
        // Compare Java's inverse with that effective calibration, not each raw source record.
        const auto effective = calibrationAt(calibration, key.frame);
        if (!effective)
        {
            require(false);
            return;
        }
        inverses.push_back(trackerInverse(*effective));
        key.originX = 0;
        key.originY = 0;
    }
    const auto check = [&](const Mark& point, bool component = false, const Mark* reconstructed = nullptr)
    {
        const auto next = std::ranges::upper_bound(calibration.keyframes, point.frame, {}, &CalibrationKey::frame);
        require(next != calibration.keyframes.begin());
        const auto& inverse = inverses[static_cast<std::size_t>(next - calibration.keyframes.begin() - 1)];
        const auto native = worldPosition(component ? componentCalibration : calibration, point);
        if (!native)
        {
            require(false);
            return;
        }
        // AffineTransform evaluates products, their sum, then translation. Test coordinates,
        // not just coefficients: cancellation can be large relative to a near-zero result.
        const auto& image = reconstructed ? *reconstructed : point;
        const auto xx = inverse[0] * image.x;
        const auto xy = inverse[2] * image.y;
        const auto yx = inverse[1] * image.x;
        const auto yy = inverse[3] * image.y;
        const std::array actual{(xx + xy) + (component ? 0 : inverse[4]), (yx + yy) + (component ? 0 : inverse[5])};
        if (component)
        {
            // Vector.refreshData transforms the components separately and squares them for magnitude.
            require(std::isfinite(actual[0] * actual[0] + actual[1] * actual[1]));
        }
        const std::array expected{native->x, native->y};
        for (std::size_t axis = 0; axis < actual.size(); ++axis)
        {
            const auto tolerance = std::max(1e-10, 1e-12 * std::max(std::abs(actual[axis]), std::abs(expected[axis])));
            require(std::isfinite(actual[axis]) && std::abs(actual[axis] - expected[axis]) <= tolerance);
        }
    };
    for (const auto& key : calibration.keyframes)
    {
        check({.frame = key.frame, .x = key.originX, .y = key.originY});
        check({.frame = key.frame, .x = 13.5, .y = 17.25}); // Independent corpus coordinate probe.
    }
    for (const auto& mass : project.pointMasses)
    {
        for (const auto& mark : mass.marks)
        {
            check(mark);
        }
    }
    for (const auto& vector : project.vectors)
    {
        for (const auto& sample : vector.samples)
        {
            check({.frame = sample.frame, .x = sample.tailX, .y = sample.tailY});
            check({.frame = sample.frame, .x = sample.tailX + sample.componentX, .y = sample.tailY + sample.componentY});
            const Mark reconstructed{.frame = sample.frame,
                                     .x = (sample.tailX + sample.componentX) - sample.tailX,
                                     .y = (sample.tailY + sample.componentY) - sample.tailY};
            check({.frame = sample.frame, .x = sample.componentX, .y = sample.componentY}, true, &reconstructed);
        }
    }
}

void requireBoundedCoordinateUpdates(const osp::Source& xml, const Calibration& current, const LegacyProject& project)
{
    require(!current.keyframes.empty());
    int lastFrame = current.keyframes.back().frame;
    require(lastFrame >= 0 && lastFrame <= 500000);
    if (const auto object = objectOf(xml, lastProperty(xml, xml.root(), "coords")))
    {
        if (const auto data = lastProperty(xml, *object, "framedata"))
        {
            for (const auto item : node(xml, *data).children)
            {
                const auto frame = requireFrameIndex(xml, item);
                require(frame <= 500000); // Includes null entries in OSP's dense array allocation.
                lastFrame = std::max(lastFrame, frame);
            }
        }
    }
    if (!(current.fixedOrigin && current.fixedAngle && current.fixedScale) || current.keyframes.size() > 1)
    {
        const auto count = project.clip.stepCount.value_or(1);
        require(project.clip.firstFrame >= 0 && project.clip.stepSize > 0 && count > 0);
        const auto lastClipFrame =
            static_cast<std::int64_t>(project.clip.firstFrame) + static_cast<std::int64_t>(count - 1) * project.clip.stepSize;
        require(lastClipFrame <= 500000);
        lastFrame = std::max(lastFrame, static_cast<int>(lastClipFrame));
        for (const auto& mass : project.pointMasses)
        {
            for (const auto& mark : mass.marks)
            {
                require(mark.frame <= 500000);
                lastFrame = std::max(lastFrame, mark.frame);
            }
        }
        for (const auto& vector : project.vectors)
        {
            for (const auto& sample : vector.samples)
            {
                require(sample.frame >= 0 && sample.frame <= 500000);
                lastFrame = std::max(lastFrame, sample.frame);
            }
        }
        // Retained null vector entries are absent from native samples, but still size OSP arrays.
        for (const auto vector : objectsOfClass(xml, tracker::VECTOR_CLASS))
        {
            const auto data = uniqueProperty(xml, vector, "framedata");
            if (!data)
                continue;
            for (const auto item : node(xml, *data).children)
            {
                const auto frame = requireFrameIndex(xml, item);
                require(frame <= 500000);
                lastFrame = std::max(lastFrame, frame);
            }
        }
        // OSP updates the remaining dense transform range for each loaded key. A bound on
        // the final index alone still permits quadratic work on many closely spaced keys.
        std::size_t frameUpdates = 0;
        for (const auto& key : current.keyframes)
        {
            require(key.frame >= 0 && key.frame <= lastFrame);
            // Fixed origin/angle setters update the entire dense range for each loaded key.
            const auto firstAffected = current.fixedOrigin || current.fixedAngle ? 0 : key.frame;
            frameUpdates += static_cast<std::size_t>(lastFrame - firstAffected) + 1;
            require(frameUpdates <= 1000000);
        }
    }
}

std::vector<osp::PropertyValue> keyProperties(const CalibrationKey& key)
{
    return {osp::doubleProperty("xorigin", key.originX),
            osp::doubleProperty("yorigin", key.originY),
            osp::doubleProperty("angle", key.angleDegrees),
            osp::doubleProperty("xscale", key.scaleX),
            osp::doubleProperty("yscale", key.scaleY)};
}

/// A calibration edit Tracker reproduces (admission): the same fixed flags, keys added or edited but not removed, and
/// exact coordinate inversion. Returns the coordinate system the keys are written to.
NodeId checkCalibration(const osp::Source& xml, const Calibration& original, const Calibration& current, const LegacyProject& project)
{
    const bool fixed = original.fixedOrigin && original.fixedAngle && original.fixedScale && current.fixedOrigin && current.fixedAngle &&
                       current.fixedScale && original.keyframes.size() == 1 && current.keyframes.size() == 1 &&
                       original.keyframes.front().frame == 0 && current.keyframes.front().frame == 0;
    const bool keyed = !(original.fixedOrigin && original.fixedAngle && original.fixedScale) &&
                       original.fixedOrigin == current.fixedOrigin && original.fixedAngle == current.fixedAngle &&
                       original.fixedScale == current.fixedScale && !original.keyframes.empty() && original.keyframes.front().frame == 0;
    require(fixed || keyed); // Changing fixed flags needs its own reversible command and reference cases.
    requireTrackerCoordinates(project, current);
    // Existing commands add or edit calibration keys. Key removal needs its own reversible command/oracle.
    for (const auto& key : original.keyframes)
    {
        require(std::ranges::binary_search(current.keyframes, key.frame, {}, &CalibrationKey::frame));
    }
    const auto object = required(objectOf(xml, lastProperty(xml, xml.root(), "coords")));
    require(node(xml, object).className == tracker::COORD_SYSTEM_CLASS);
    if (const auto locked = lastProperty(xml, object, "locked"))
    {
        require(node(xml, *locked).type == PropertyType::Boolean && osp::javaTrim(node(xml, *locked).content) == "false");
    }
    return object;
}

/// Calibration keys through the frame-array patcher.
void patchCalibrationKeys(SourceEdits& edits,
                          NodeId object,
                          const std::vector<CalibrationKey>& original,
                          const std::vector<CalibrationKey>& current)
{
    patchFrameData(edits,
                   object,
                   original,
                   current,
                   tracker::COORD_FRAME_CLASS,
                   keyProperties,
                   [](SourceEdits& document, NodeId frame, const CalibrationKey& key)
                   {
                       patchDouble(document, frame, "xorigin", key.originX);
                       patchDouble(document, frame, "yorigin", key.originY);
                       patchDouble(document, frame, "angle", key.angleDegrees);
                       patchDouble(document, frame, "xscale", key.scaleX);
                       patchDouble(document, frame, "yscale", key.scaleY);
                   });
}

void patchCalibration(SourceEdits& edits, const Calibration& original, const Calibration& current, const LegacyProject& project)
{
    const auto object = checkCalibration(edits.source(), original, current, project);
    patchCalibrationKeys(edits, object, original.keyframes, current.keyframes);
}

void sortTracks(LegacyProject& project)
{
    std::ranges::sort(project.pointMasses, {}, &PointMass::id);
    std::ranges::sort(project.vectors, {}, &VectorTrack::id);
}

/// Tracker's loader subtracts a positive clip frame shift from every track's saved `framedata` index (#313), so the
/// imported frames are not the saved indices the frame writers patch. Until the writer maps frames back to saved
/// indices, refuse changed or new samples rather than write them at other frames; other edits keep the indices.
void requireUnchangedShiftedSamples(const LegacyProject& expected, const LegacyProject& baseline)
{
    const auto refuse = []
    {
        throw ProjectError("Tracker export cannot yet save changed marks or vectors in a project whose video clip has a frame shift "
                           "(MoTrack issue #328).");
    };
    for (const auto& track : expected.pointMasses)
    {
        const auto original = std::ranges::find(baseline.pointMasses, track.id, &PointMass::id);
        if (original == baseline.pointMasses.end() ? !track.marks.empty() : original->marks != track.marks)
            refuse();
    }
    for (const auto& track : expected.vectors)
    {
        const auto original = std::ranges::find(baseline.vectors, track.id, &VectorTrack::id);
        if (original == baseline.vectors.end() ? !track.samples.empty() : original->samples != track.samples)
            refuse();
    }
}

bool retainsVectorComponent(double tail, double component)
{
    // Tracker stores endpoints internally, then reconstructs components by subtraction on save.
    // Use the independent corpus tolerance in image-pixel units, including cancellation error.
    const auto tip = tail + component;
    const auto reconstructed = tip - tail;
    const auto tolerance = std::max(1e-10, 1e-12 * std::max(std::abs(component), std::abs(reconstructed)));
    return std::isfinite(tip) && std::isfinite(reconstructed) && std::abs(reconstructed - component) <= tolerance;
}

std::vector<osp::PropertyValue> vectorProperties(const VectorSample& sample)
{
    require(std::isfinite(sample.tailX) && std::isfinite(sample.tailY) && std::isfinite(sample.componentX) &&
            std::isfinite(sample.componentY) && retainsVectorComponent(sample.tailX, sample.componentX) &&
            retainsVectorComponent(sample.tailY, sample.componentY));
    return {osp::doubleProperty("xtail", sample.tailX),
            osp::doubleProperty("ytail", sample.tailY),
            osp::doubleProperty("xcomponent", sample.componentX),
            osp::doubleProperty("ycomponent", sample.componentY)};
}

void requireStableVectors(const osp::Source& xml, const LegacyProject& project, const std::unordered_map<TrackId, NodeId>& objects)
{
    std::vector<const VectorSample*> samples;
    std::size_t frameSlots = 0;
    for (const auto& vector : project.vectors)
    {
        // OSP allocates a dense Java array through the highest XML index, even for null entries.
        // Bound individual indices and the sum across tracks before any destination is opened.
        int lastFrame = -1;
        const auto object = objects.at(vector.id);
        if (const auto data = uniqueProperty(xml, object, "framedata"))
        {
            for (const auto item : node(xml, *data).children)
            {
                const auto frame = requireFrameIndex(xml, item);
                require(frame <= 500000);
                lastFrame = std::max(lastFrame, frame);
            }
        }
        for (const auto& sample : vector.samples)
        {
            require(sample.frame >= 0 && sample.frame <= 500000);
            lastFrame = std::max(lastFrame, sample.frame);
            require(samples.size() < 4096); // Bound the conservative all-visible-tip comparison below.
            samples.push_back(&sample);
        }
        frameSlots += static_cast<std::size_t>(lastFrame + 1);
        require(frameSlots <= 500001);
        if (!vector.samples.empty())
        {
            const auto footprint = uniqueText(xml, object, "footprint");
            require(footprint == "Footprint.Arrow" || footprint == "Footprint.BoldArrow");
        }
    }
    if (samples.empty())
    {
        return;
    }
    // Until dependencies and display scaling are modeled, only audit ordinary independent arrows
    // with no other tracks that could supply visible vector snap targets.
    require(project.pointMasses.empty() &&
            std::ranges::all_of(project.unsupportedTrackTypes, [](const auto& type) { return type == tracker::COORD_AXES_CLASS; }));
    if (!project.calibration || project.calibration->keyframes.empty())
    {
        require(false);
        return;
    }
    requireTrackerCoordinates(project, *project.calibration);
    // Bound sample/origin comparisons as well as the all-visible-tip comparisons. Fixed origin
    // uses the last loaded key; varying origins are piecewise constant between source keys.
    std::vector<CalibrationKey> origins;
    if (project.calibration->fixedOrigin)
        origins.push_back(project.calibration->keyframes.back());
    else
    {
        require(project.calibration->keyframes.size() <= 1000000 / samples.size());
        origins = project.calibration->keyframes;
    }
    for (const auto* sample : samples)
    {
        require(std::isfinite(sample->tailX) && std::isfinite(sample->tailY) && std::isfinite(sample->componentX) &&
                std::isfinite(sample->componentY) && retainsVectorComponent(sample->tailX, sample->componentX) &&
                retainsVectorComponent(sample->tailY, sample->componentY));
        // Tracker 6.3.5 snaps brand-new steps on first draw. A trail step can first draw at
        // another playhead, whose origin is the shared snap point. Checking its own frame's
        // origin alone would allow that draw to alter the stored tail and subsequent resave.
        for (const auto& origin : origins)
            require(std::hypot(sample->tailX - origin.originX, sample->tailY - origin.originY) > 8);
        for (const auto* target : samples)
        {
            if (target != sample)
            {
                require(std::hypot(sample->tailX - (target->tailX + target->componentX),
                                   sample->tailY - (target->tailY + target->componentY)) > 8);
            }
        }
    }
}

/// TTrack's appearance as Tracker saves it after the name: color, footprint, visible and trail (`locked` only when true).
/// Native appearance is not yet persisted; these are the audited interchange defaults of a new track.
std::vector<osp::PropertyValue> appearanceDefaults(TrackKind kind)
{
    const bool vector = kind == TrackKind::Vector;
    std::vector<osp::PropertyValue> properties;
    osp::ObjectValue color{.className = "java.awt.Color"};
    for (const auto& [channel, value] :
         std::array<std::pair<const char*, int>, 4>{{{"red", 255}, {"green", 0}, {"blue", vector ? 255 : 0}, {"alpha", 255}}})
        color.properties.push_back(osp::intProperty(channel, value));
    properties.push_back(osp::objectProperty("color", std::move(color)));
    properties.push_back(osp::stringProperty("footprint", vector ? "Footprint.BoldArrow" : "Footprint.Diamond"));
    properties.push_back(osp::booleanProperty("visible", true));
    properties.push_back(osp::booleanProperty("trail", true));
    return properties;
}

/// A new track's name OSP can write (admission).
void checkCreatedName(const std::string& name)
{
    require(name.find("]]>") == std::string::npos);
}

/// A new independent track as Tracker's PointMass/Vector loaders save it: PointMass writes `mass` before
/// TTrack's name and appearance, then its frame arrays.
osp::PropertyValue newTrack(TrackKind kind, const std::string& name, double mass, std::vector<osp::PropertyValue> arrays)
{
    const bool vector = kind == TrackKind::Vector;
    checkCreatedName(name);
    osp::ObjectValue object{.className = std::string(vector ? tracker::VECTOR_CLASS : tracker::POINT_MASS_CLASS)};
    if (!vector)
        object.properties.push_back(osp::doubleProperty("mass", mass));
    object.properties.push_back(osp::stringProperty("name", name));
    for (auto& property : appearanceDefaults(kind))
        object.properties.push_back(std::move(property));
    for (auto& array : arrays)
        object.properties.push_back(std::move(array));
    return osp::objectProperty("item", std::move(object));
}

/// A created point mass Tracker loads as written (admission), counting its dense frame slots across created tracks.
void checkCreatedPointMass(const PointMass& track, std::size_t& createdFrameSlots)
{
    // New native tracks have no source metadata to copy. Use the audited independent-mass defaults.
    require(std::ranges::all_of(track.marks, [](const auto& mark) { return mark.frame >= 0 && mark.frame <= 500000; }));
    require(std::isfinite(track.mass) && track.mass >= MIN_POINT_MASS);
    if (!track.marks.empty())
    {
        const auto last = std::ranges::max(track.marks, {}, &Mark::frame).frame;
        createdFrameSlots += static_cast<std::size_t>(last) + 1;
        require(createdFrameSlots <= 1000000);
        int previous = -1;
        for (const auto& mark : track.marks)
        {
            require(mark.frame > previous);
            previous = mark.frame;
            static_cast<void>(markProperties(mark));
        }
    }
    checkCreatedName(track.name);
}

/// A created vector: only beside other vectors and coordinate axes, with samples Tracker reconstructs (admission).
void checkCreatedVector(const LegacyProject& expected, const VectorTrack& track)
{
    require(expected.pointMasses.empty() &&
            std::ranges::all_of(expected.unsupportedTrackTypes, [](const auto& type) { return type == tracker::COORD_AXES_CLASS; }));
    int previous = -1;
    for (const auto& sample : track.samples)
    {
        require(sample.frame > previous);
        previous = sample.frame;
        static_cast<void>(vectorProperties(sample));
    }
    checkCreatedName(track.name);
}

void remapIds(LegacyProject& project, const std::unordered_map<TrackId, TrackId>& mapping, TrackId nextId)
{
    for (auto& track : project.pointMasses)
    {
        track.id = mapping.at(track.id);
    }
    for (auto& track : project.vectors)
    {
        track.id = mapping.at(track.id);
    }
    project.nextTrackId = nextId;
    sortTracks(project);
}

/// An audited linked image sequence (admission): every frame and sample within the sequence, unchanged numeric timing
/// and a consistent saved inventory. Returns its ImageVideo.
NodeId checkLinkedImages(const osp::Source& xml, const LegacyProject& project, const LegacyProject& baseline)
{
    validateTrackerEditSchema(xml);
    require(std::ranges::all_of(project.unsupportedTrackTypes, [](const auto& type) { return type == tracker::COORD_AXES_CLASS; }));
    const auto count = project.clip.mediaReferences.size();
    require(count > 0 && count <= 10000 && count == baseline.clip.mediaReferences.size());
    auto expectedClip = project.clip;
    expectedClip.mediaReferences = baseline.clip.mediaReferences;
    expectedClip.firstFrame = baseline.clip.firstFrame;
    expectedClip.stepSize = baseline.clip.stepSize;
    expectedClip.stepCount = baseline.clip.stepCount;
    require(expectedClip == baseline.clip); // Numeric timing changes need separate media acceptance.
    const auto& clip = project.clip;
    const auto steps = clip.stepCount.value_or(0);
    require(clip.firstFrame >= 0 && clip.stepSize > 0 && steps > 0 &&
            static_cast<std::int64_t>(clip.firstFrame) + static_cast<std::int64_t>(steps - 1) * clip.stepSize <
                static_cast<std::int64_t>(count));
    for (const auto& mass : project.pointMasses)
        for (const auto& mark : mass.marks)
            require(mark.frame >= 0 && std::cmp_less(mark.frame, count));
    for (const auto& vector : project.vectors)
        for (const auto& sample : vector.samples)
            require(sample.frame >= 0 && std::cmp_less(sample.frame, count));
    const auto clipObject = videoClipObject(xml);
    const auto video = required(objectOf(xml, uniqueProperty(xml, clipObject, "video")));
    require(node(xml, video).className == tracker::IMAGE_VIDEO_CLASS);
    if (const auto frameCount = uniqueProperty(xml, clipObject, "video_framecount"))
    {
        const auto saved = required(javaInt(node(xml, *frameCount).content));
        require(saved >= 0 && std::cmp_equal(saved, count));
    }
    const auto duration = required(javaDouble(uniqueText(xml, video, "delta_t")));
    require(std::isfinite(duration) && duration > 0.0 && clip.frameDurationMs == duration);
    require(uniqueText(xml, video, "path") == baseline.clip.mediaReferences.front());
    return video;
}

/// The image sequence's `paths` and `path` as the destination spells them.
void patchLinkedImagePaths(SourceEdits& edits, NodeId video, const std::vector<std::string>& references)
{
    const auto& xml = edits.source();
    const auto paths = required(uniqueProperty(xml, video, "paths"));
    for (std::size_t index = 0; index < references.size(); ++index)
    {
        const auto entry = required(uniqueProperty(xml, paths, "[" + std::to_string(index) + "]"));
        require(node(xml, entry).type == PropertyType::String && references[index].find("]]>") == std::string::npos);
        if (const auto current = osp::PropertyView(xml, entry).stringValue(); !current || *current != references[index])
            edits.setString(entry, references[index]);
    }
    patchString(edits, video, "path", references.front());
}

bool patchLinkedImagePaths(SourceEdits& edits, const LegacyProject& project, LegacyProject& baseline)
{
    const auto video = checkLinkedImages(edits.source(), project, baseline);
    if (project.clip.mediaReferences == baseline.clip.mediaReferences)
        return false;
    patchLinkedImagePaths(edits, video, project.clip.mediaReferences);
    baseline.clip.mediaReferences = project.clip.mediaReferences;
    return true;
}

/// An audited constant-rate linked movie (admission): unchanged timing and edits other than the audited ones, decoded
/// frame times that match the clip control's frame duration, and every frame and sample within the movie. Returns its
/// XuggleVideo.
NodeId
checkLinkedMovie(const osp::Source& xml, const LegacyProject& project, const LegacyProject& baseline, std::span<const double> frameTimes)
{
    validateTrackerMovieEditSchema(xml);
    require(project.clip.mediaReferences.size() == 1 && baseline.clip.mediaReferences.size() == 1 &&
            std::ranges::all_of(project.unsupportedTrackTypes, [](const auto& type) { return type == tracker::COORD_AXES_CLASS; }));
    // Movie edits retain original timing. Clip ranges and marks keep source frame numbers.
    // Existing calibration uses the shared coordinate writer and its numerical checks below.
    auto unchanged = project;
    if (project.calibration != baseline.calibration)
    {
        require(project.calibration && std::ranges::all_of(project.calibration->keyframes,
                                                           [frameTimes](const auto& key)
                                                           { return key.frame >= 0 && std::cmp_less(key.frame, frameTimes.size()); }));
        unchanged.calibration = baseline.calibration;
    }
    unchanged.lengthUnit = baseline.lengthUnit;
    unchanged.clip.mediaReferences = baseline.clip.mediaReferences;
    unchanged.clip.firstFrame = baseline.clip.firstFrame;
    unchanged.clip.stepSize = baseline.clip.stepSize;
    unchanged.clip.stepCount = baseline.clip.stepCount;
    // The shared identity/membership writers below audit source bindings, names, relationships,
    // new-track defaults and deletion cleanup before the final full-project equality check.
    unchanged.pointMasses = baseline.pointMasses;
    unchanged.vectors = baseline.vectors;
    require(unchanged == baseline);
    const auto clip = videoClipObject(xml);
    const auto video = required(objectOf(xml, uniqueProperty(xml, clip, "video")));
    const auto control = required(objectOf(xml, uniqueProperty(xml, xml.root(), "clipcontrol")));
    require(node(xml, video).className == tracker::XUGGLE_VIDEO_CLASS &&
            node(xml, control).className == tracker::VIDEO_CLIP_CONTROL_CLASS &&
            uniqueText(xml, video, "path") == baseline.clip.mediaReferences.front());
    const auto duration = required(javaDouble(uniqueText(xml, control, "delta_t")));
    require(std::isfinite(duration) && duration > 0.0 && !frameTimes.empty() && frameTimes.size() <= 2000000);
    for (std::size_t frame = 0; frame < frameTimes.size(); ++frame)
    {
        const auto expected = static_cast<double>(frame) * duration / 1000.0;
        require(std::isfinite(expected) && std::isfinite(frameTimes[frame]) && std::abs(frameTimes[frame] - expected) <= 1e-6);
    }
    if (const auto count = uniqueProperty(xml, clip, "video_framecount"))
    {
        const auto saved = required(javaInt(node(xml, *count).content));
        require(saved >= 0 && std::cmp_equal(saved, frameTimes.size()));
    }
    const auto steps = project.clip.stepCount.value_or(0);
    const auto last = static_cast<std::int64_t>(project.clip.firstFrame) + (static_cast<std::int64_t>(steps) - 1) * project.clip.stepSize;
    require(project.clip.firstFrame >= 0 && project.clip.stepSize > 0 && steps > 0 && last >= 0 && std::cmp_less(last, frameTimes.size()));
    for (const auto& mass : project.pointMasses)
        for (const auto& mark : mass.marks)
            require(mark.frame >= 0 && std::cmp_less(mark.frame, frameTimes.size()));
    for (const auto& vector : project.vectors)
        for (const auto& sample : vector.samples)
            require(sample.frame >= 0 && std::cmp_less(sample.frame, frameTimes.size()));
    return video;
}

bool patchLinkedMoviePath(SourceEdits& edits, const LegacyProject& project, LegacyProject& baseline, std::span<const double> frameTimes)
{
    const auto video = checkLinkedMovie(edits.source(), project, baseline, frameTimes);
    if (project.clip.mediaReferences == baseline.clip.mediaReferences)
        return false;
    patchString(edits, video, "path", project.clip.mediaReferences.front());
    baseline.clip.mediaReferences = project.clip.mediaReferences;
    return true;
}

/// An independent vector's samples through the frame-array patcher.
void patchVectorFrames(SourceEdits& edits,
                       NodeId object,
                       const std::vector<VectorSample>& original,
                       const std::vector<VectorSample>& current)
{
    patchFrameData(edits,
                   object,
                   original,
                   current,
                   tracker::VECTOR_FRAME_CLASS,
                   vectorProperties,
                   [](SourceEdits& document, NodeId step, const VectorSample& sample)
                   {
                       static_cast<void>(vectorProperties(sample)); // Same finite/reconstruction audit as new samples.
                       patchDouble(document, step, "xtail", sample.tailX);
                       patchDouble(document, step, "ytail", sample.tailY);
                       patchDouble(document, step, "xcomponent", sample.componentX);
                       patchDouble(document, step, "ycomponent", sample.componentY);
                   });
}

/// Renders through motrack_osp: untouched bytes are copied verbatim and O8 is checked on the output.
SourceEdits::Rendered renderEdits(const SourceEdits& edits)
{
    auto rendered = edits.render();
    if (!rendered)
    {
        throw ProjectError("This Tracker export cannot preserve all edits or source associations (" + rendered.error() + ").");
    }
    return std::move(*rendered);
}

/// Admission checks on the composed output's views and tracks Tracker cannot resave, before its tracks are matched.
void checkEmittedViews(const osp::Source& emitted, const LegacyProject& expected, LinkedMedia media)
{
    // Guards below audit the emitted document, including appended metadata and patched samples.
    requireStableTableColumns(emitted, expected, media == LinkedMedia::Movie);
    // Pinned Tracker 6.3.5 never initializes saved VectorSum members and loses their names on resave.
    // Do not let clearing all independent samples bypass the nonempty-vector safeguards below.
    require(std::ranges::find(expected.unsupportedTrackTypes, "org.opensourcephysics.cabrillo.tracker.VectorSum") ==
            expected.unsupportedTrackTypes.end());
}

/// Admission checks on the composed output's tracks, budgets and schema, given each live track's emitted object.
void checkEmittedTracks(const osp::Source& emitted,
                        const LegacyProject& expected,
                        LinkedMedia media,
                        bool auditFinalSchema,
                        const std::unordered_map<TrackId, NodeId>& emittedObjects,
                        std::size_t bytes)
{
    requireStableVectors(emitted, expected, emittedObjects);
    // Count the emitted state after all removals/patches. Calibration still loads when the last
    // vector is deleted, and an unchanged calibration cannot exempt another edit from its budget.
    if (expected.calibration)
        requireBoundedCoordinateUpdates(emitted, *expected.calibration, expected);
    requireBoundedMassFrames(emitted);
    if (auditFinalSchema)
    {
        // Include appended metadata and patched samples in the reimport/edit budget.
        if (media == LinkedMedia::Movie)
            validateTrackerMovieEditSchema(emitted);
        else
            validateTrackerEditSchema(emitted);
    }
    require(bytes <= MAX_TRACKER_XML_BYTES);
}

} // namespace

std::string detail::composeLegacyExport(const LegacyProject& project, const TrackerSource& source, const ExportMedia& exportMedia)
{
    const auto media = exportMedia.kind;
    const auto frameTimes = exportMedia.frameTimes;
    require(media != LinkedMedia::None || project.clip.mediaReferences.empty());
    auto imported = detail::validatedTrackerSource(source, project);
    if (!source.bindings)
    {
        throw ProjectError("Tracker export requires original object associations.");
    }
    auto& baseline = imported.project;
    std::unordered_map<std::size_t, TrackId> sourceIds;
    for (const auto& binding : *source.bindings)
    {
        sourceIds.emplace(binding.sourceIndex, binding.trackId);
    }
    std::unordered_map<TrackId, TrackId> mapping;
    for (const auto& binding : imported.bindings)
    {
        mapping.emplace(binding.trackId, sourceIds.at(binding.sourceIndex));
    }
    remapIds(baseline, mapping, project.nextTrackId);
    auto expected = project;
    sortTracks(expected);
    if (imported.trackFrameShift > 0)
        requireUnchangedShiftedSamples(expected, baseline);
    const bool membershipChanged = !std::ranges::equal(expected.pointMasses, baseline.pointMasses, {}, &PointMass::id, &PointMass::id) ||
                                   !std::ranges::equal(expected.vectors, baseline.vectors, {}, &VectorTrack::id, &VectorTrack::id);
    std::unordered_set<TrackId> liveIds;
    for (const auto& track : expected.pointMasses)
        liveIds.insert(track.id);
    for (const auto& track : expected.vectors)
        liveIds.insert(track.id);
    // Reparse assigns compact IDs in surviving XML order, not the original/native ID order.
    mapping.clear();
    TrackId nextParsedId = 1;

    SourceEdits edits(imported.source);
    const auto& xml = edits.source();
    const auto tracks = imported.panel.tracks;
    std::unordered_map<TrackId, NodeId> objects;
    for (const auto& item : imported.panel.trackItems)
    {
        if (const auto found = sourceIds.find(item.ordinal); found != sourceIds.end())
        {
            require(item.object.has_value());
            objects.emplace(found->second, item.object.value_or(NodeId{}));
            if (liveIds.contains(found->second))
                mapping.emplace(nextParsedId++, found->second);
        }
    }
    bool changed = media == LinkedMedia::Images  ? patchLinkedImagePaths(edits, expected, baseline)
                   : media == LinkedMedia::Movie ? patchLinkedMoviePath(edits, expected, baseline, frameTimes)
                                                 : false;
    bool auditFinalSchema = media != LinkedMedia::None;
    bool massCoordinatesChecked = false;
    if (membershipChanged)
    {
        validateTrackerTrackMembership(xml, expected, source, media == LinkedMedia::Movie);
        auditFinalSchema = true;
        require(expected.calibration || !lastProperty(xml, xml.root(), "coords"));
        if (expected.calibration)
            requireTrackerCoordinates(expected, *expected.calibration);
        massCoordinatesChecked = true;
        const auto collection = required(tracks);
        require(node(xml, collection).type == PropertyType::Collection && node(xml, collection).className == "java.util.ArrayList");
        std::size_t createdFrameSlots = 0;
        for (const auto& current : expected.pointMasses)
        {
            if (objects.contains(current.id))
                continue;
            checkCreatedPointMass(current, createdFrameSlots);
            std::vector<osp::PropertyValue> arrays;
            if (!current.marks.empty())
            {
                arrays.push_back(frameArrayValue("framedata", tracker::POINT_MASS_FRAME_CLASS, current.marks, markProperties));
                std::set<int> keys;
                for (const auto& mark : current.marks)
                    keys.insert(mark.frame);
                arrays.push_back(keyFramesValue(keys));
            }
            edits.append(collection, newTrack(TrackKind::PointMass, current.name, current.mass, std::move(arrays)));
            mapping.emplace(nextParsedId++, current.id);
            baseline.pointMasses.push_back(current);
        }
        for (const auto& current : expected.vectors)
        {
            if (objects.contains(current.id))
                continue;
            checkCreatedVector(expected, current);
            std::vector<osp::PropertyValue> arrays;
            if (!current.samples.empty())
                arrays.push_back(frameArrayValue("framedata", tracker::VECTOR_FRAME_CLASS, current.samples, vectorProperties));
            edits.append(collection, newTrack(TrackKind::Vector, current.name, 1.0, std::move(arrays)));
            mapping.emplace(nextParsedId++, current.id);
            baseline.vectors.push_back(current);
        }
        std::erase_if(baseline.pointMasses, [&](const auto& track) { return !liveIds.contains(track.id); });
        std::erase_if(baseline.vectors, [&](const auto& track) { return !liveIds.contains(track.id); });
        sortTracks(baseline);
        changed = true;
    }
    const bool renamed = patchTrackerNames(edits, expected, baseline, source, media == LinkedMedia::Movie);
    changed = renamed || changed;
    auditFinalSchema = auditFinalSchema || renamed;
    // Name patching uses original source ordinals, so remove nodes only after its reference updates.
    if (membershipChanged)
    {
        patchTrackerDeletedReferences(edits, expected, source);
        for (const auto& [id, object] : objects)
        {
            if (!liveIds.contains(id))
            {
                const auto item = node(xml, object).parent;
                require(item.has_value());
                edits.remove(item.value_or(NodeId{}));
            }
        }
    }
    if (expected.calibration != baseline.calibration)
    {
        if (!expected.calibration || !baseline.calibration)
        {
            require(false); // Creating/removing coordinate systems is not covered by this writer slice.
        }
        else
        {
            patchCalibration(edits, *baseline.calibration, *expected.calibration, expected);
            massCoordinatesChecked = true;
            baseline.calibration = expected.calibration;
            changed = true;
        }
    }
    if (expected.lengthUnit != baseline.lengthUnit)
    {
        // A unit is a label; any requested rescaling belongs to the reversible calibration edit.
        patchUnit(edits, "length_unit", expected.lengthUnit);
        baseline.lengthUnit = expected.lengthUnit;
        changed = true;
    }
    if (expected.massUnit != baseline.massUnit)
    {
        patchUnit(edits, "mass_unit", expected.massUnit);
        baseline.massUnit = expected.massUnit;
        changed = true;
    }
    if (expected.timeUnit != baseline.timeUnit)
    {
        patchUnit(edits, "time_unit", expected.timeUnit);
        baseline.timeUnit = expected.timeUnit;
        changed = true;
    }
    if (expected.clip.firstFrame != baseline.clip.firstFrame || expected.clip.stepSize != baseline.clip.stepSize ||
        expected.clip.stepCount != baseline.clip.stepCount)
    {
        patchClipRange(edits, expected.clip, media);
        baseline.clip.firstFrame = expected.clip.firstFrame;
        baseline.clip.stepSize = expected.clip.stepSize;
        baseline.clip.stepCount = expected.clip.stepCount;
        changed = true;
    }
    if (expected.clip.frameDurationMs != baseline.clip.frameDurationMs || expected.clip.startTimeMs != baseline.clip.startTimeMs)
    {
        patchClipTiming(edits, expected);
        baseline.clip.frameDurationMs = expected.clip.frameDurationMs;
        baseline.clip.startTimeMs = expected.clip.startTimeMs;
        changed = true;
    }
    for (std::size_t index = 0; index < expected.pointMasses.size(); ++index)
    {
        const auto& current = expected.pointMasses[index];
        auto& original = baseline.pointMasses[index];
        require(current.id == original.id && std::isfinite(current.mass) && current.mass >= MIN_POINT_MASS);
        if (current.mass != original.mass)
        {
            patchDouble(edits, objects.at(current.id), "mass", current.mass);
            original.mass = current.mass;
            changed = true;
        }
        if (current.marks != original.marks)
        {
            if (!massCoordinatesChecked)
            {
                require(expected.calibration || !lastProperty(xml, xml.root(), "coords"));
                if (expected.calibration)
                    requireTrackerCoordinates(expected, *expected.calibration);
                massCoordinatesChecked = true;
            }
            patchMarks(edits, objects.at(current.id), original.marks, current.marks);
            original.marks = current.marks;
            changed = true;
        }
    }
    for (std::size_t index = 0; index < expected.vectors.size(); ++index)
    {
        const auto& current = expected.vectors[index];
        auto& original = baseline.vectors[index];
        require(current.id == original.id);
        if (current.samples != original.samples)
        {
            patchVectorFrames(edits, objects.at(current.id), original.samples, current.samples);
            original.samples = current.samples;
            changed = true;
        }
    }
    // Never produce a superficially valid file that silently omits an unsupported edit.
    require(baseline == expected);
    if (!changed)
    {
        return source.xml;
    }
    // Render through motrack_osp: untouched bytes are copied verbatim and O8 is checked on the output.
    auto rendered = renderEdits(edits);
    auto output = detail::importTracker(rendered.source);
    const auto& emitted = *output.source;
    checkEmittedViews(emitted, expected, media);
    require(output.bindings.size() == mapping.size());
    std::unordered_map<TrackId, NodeId> emittedObjects;
    for (const auto& binding : output.bindings)
    {
        const auto& item = output.panel.trackItems.at(binding.sourceIndex);
        emittedObjects.emplace(mapping.at(binding.trackId), item.object.value_or(NodeId{}));
    }
    checkEmittedTracks(emitted, expected, media, auditFinalSchema, emittedObjects, rendered.xml.size());
    auto reparsed = std::move(output.project);
    remapIds(reparsed, mapping, project.nextTrackId);
    require(reparsed == expected);
    return std::move(rendered.xml);
}

// The descriptor composition (docs/tracker-document.md, "From state to SourceEdits").

namespace
{

using model::ObjectSlot;
using model::Role;

template <typename State> const State* stateOf(const ObjectSlot* object)
{
    return object ? std::get_if<State>(object->value.get()) : nullptr;
}

template <typename T> bool sameValues(const model::FrameSeries<T>& left, const model::FrameSeries<T>& right)
{
    if (left.sharesChunksWith(right))
    {
        return true;
    }
    if (left.size() != right.size())
    {
        return false;
    }
    const auto a = left.entries();
    const auto b = right.entries();
    return std::ranges::equal(a, b, [](const auto& x, const auto& y) { return x.first == y.first && x.second.value == y.second.value; });
}

// The frame-array patcher works on the reduced samples of a frame-keyed field.

std::vector<Mark> reducedMarks(const model::FrameSeries<model::Mark>& series)
{
    std::vector<Mark> result;
    result.reserve(series.size());
    series.forEach([&](std::int32_t frame, const model::FrameElement<model::Mark>& mark)
                   { result.push_back({.frame = frame, .x = mark.value.x, .y = mark.value.y}); });
    return result;
}

std::vector<VectorSample> reducedSamples(const model::FrameSeries<model::VectorSample>& series)
{
    std::vector<VectorSample> result;
    result.reserve(series.size());
    series.forEach(
        [&](std::int32_t frame, const model::FrameElement<model::VectorSample>& sample)
        {
            result.push_back({.frame = frame,
                              .tailX = sample.value.xTail,
                              .tailY = sample.value.yTail,
                              .componentX = sample.value.xComponent,
                              .componentY = sample.value.yComponent});
        });
    return result;
}

std::vector<CalibrationKey> reducedKeys(const model::FrameSeries<model::CoordKey>& series)
{
    std::vector<CalibrationKey> result;
    result.reserve(series.size());
    series.forEach(
        [&](std::int32_t frame, const model::FrameElement<model::CoordKey>& key)
        {
            result.push_back({.frame = frame,
                              .originX = key.value.xOrigin,
                              .originY = key.value.yOrigin,
                              .angleDegrees = key.value.angle,
                              .scaleX = key.value.xScale,
                              .scaleY = key.value.yScale});
        });
    return result;
}

/// The same accessor result; doubles by their bits, so that a saved NaN reads as the same value again.
template <typename T> bool same(const model::Read<T>& left, const model::Read<T>& right)
{
    if constexpr (std::is_same_v<T, double>)
    {
        if (left && right)
        {
            return std::bit_cast<std::uint64_t>(*left) == std::bit_cast<std::uint64_t>(*right);
        }
    }
    return left == right;
}

bool same(const model::FieldValue& left, const model::FieldValue& right)
{
    const auto* a = std::get_if<double>(&left);
    const auto* b = std::get_if<double>(&right);
    return a && b ? std::bit_cast<std::uint64_t>(*a) == std::bit_cast<std::uint64_t>(*b) : left == right;
}

/// Equal typed states as an export reproduces them: frame elements by value, since reimported frames have new nodes, and
/// the panel without its selected track, a view reference that B1's audited cleanup rewrites and that the view checks
/// cover instead.
bool sameExportedState(const model::TypedValue& left, const model::TypedValue& right)
{
    if (left.index() != right.index())
    {
        return false;
    }
    return std::visit(
        [&](const auto& state)
        {
            using State = std::decay_t<decltype(state)>;
            const auto& other = std::get<State>(right);
            if constexpr (std::is_same_v<State, model::PanelState>)
            {
                auto copy = state;
                copy.selectedTrack = other.selectedTrack;
                return copy == other;
            }
            else if constexpr (std::is_same_v<State, model::ClipState>)
            {
                return state.startFrame == other.startFrame && state.stepSize == other.stepSize && state.stepCount == other.stepCount &&
                       state.frameCount == other.frameCount && state.frameShift == other.frameShift &&
                       same(state.startTime, other.startTime);
            }
            else if constexpr (std::is_same_v<State, model::VideoState>)
            {
                return state.path == other.path && state.paths == other.paths && same(state.deltaT, other.deltaT);
            }
            else if constexpr (std::is_same_v<State, model::ClipControlState>)
            {
                return same(state.rate, other.rate) && same(state.deltaT, other.deltaT) && state.frame == other.frame &&
                       state.looping == other.looping;
            }
            else if constexpr (std::is_same_v<State, model::CoordinatesState>)
            {
                return state.fixedOrigin == other.fixedOrigin && state.fixedAngle == other.fixedAngle &&
                       state.fixedScale == other.fixedScale && state.locked == other.locked && sameValues(state.keys, other.keys);
            }
            else if constexpr (std::is_same_v<State, model::PointMassState>)
            {
                return state.name == other.name && same(state.mass, other.mass) && state.keyFrames == other.keyFrames &&
                       sameValues(state.marks, other.marks);
            }
            else if constexpr (std::is_same_v<State, model::VectorState>)
            {
                return state.name == other.name && sameValues(state.samples, other.samples);
            }
            else
            {
                return state == other;
            }
        },
        left);
}

/// A field's value as Tracker's loader leaves it: the saved value, or the descriptor's loader default (#214) when the file
/// saves none. A batch writes it in full.
template <typename T> T loadedOrDefault(const model::Read<T>& value, model::Family family, std::string_view id)
{
    if (value)
    {
        return *value;
    }
    const auto* field = model::findField(family, id);
    require(field != nullptr && !field->loaderDefault.empty() && value.error() == osp::DecodeError::Missing);
    const auto text = field ? field->loaderDefault : std::string_view{};
    if constexpr (std::is_same_v<T, double>)
    {
        return required(javaDouble(text));
    }
    else
    {
        return required(javaInt(text));
    }
}

/// A typed field's value differs between the state and its baseline reading.
bool differs(const model::TypedValue& now, const model::TypedValue& before, const model::FieldDescriptor& field)
{
    if (field.kind == model::FieldKind::Scalar)
    {
        return !same(model::scalarValue(now, field), model::scalarValue(before, field));
    }
    if (const auto* coords = std::get_if<model::CoordinatesState>(&now))
    {
        return !sameValues(coords->keys, std::get<model::CoordinatesState>(before).keys);
    }
    if (const auto* mass = std::get_if<model::PointMassState>(&now))
    {
        return !sameValues(mass->marks, std::get<model::PointMassState>(before).marks);
    }
    return !sameValues(std::get<model::VectorState>(now).samples, std::get<model::VectorState>(before).samples);
}

/// A created track as its family's saver writes it (PointMass: `mass`, then TTrack's `name` and appearance, then
/// `framedata` and `keyFrames`; Vector: the same without `mass` and `keyFrames`), checked against Tracker's saver by
/// oracle O7. Tracker's saver writes no frame arrays for a track without frames.
osp::PropertyValue createdTrack(const ObjectSlot& object)
{
    auto properties = model::saverProperties(*object.value);
    const auto kind = object.family() == model::Family::PointMass ? TrackKind::PointMass : TrackKind::Vector;
    const auto* mass = std::get_if<model::PointMassState>(object.value.get());
    const bool framed = mass ? !mass->marks.empty() : !std::get<model::VectorState>(*object.value).samples.empty();
    if (!framed)
    {
        std::erase_if(properties,
                      [](const osp::PropertyValue& property) { return property.name == "framedata" || property.name == "keyFrames"; });
    }
    const auto name = std::ranges::find(properties, std::optional<std::string>("name"), &osp::PropertyValue::name);
    require(name != properties.end());
    auto appearance = appearanceDefaults(kind);
    properties.insert(std::next(name), std::make_move_iterator(appearance.begin()), std::make_move_iterator(appearance.end()));
    return osp::objectProperty(
        "item", osp::ObjectValue{.className = object.className, .version = std::nullopt, .properties = std::move(properties)});
}

std::string loadedTrackName(const model::TypedValue& value)
{
    if (const auto* mass = std::get_if<model::PointMassState>(&value))
    {
        return model::loadedName(mass->name);
    }
    return model::loadedName(std::get<model::VectorState>(value).name);
}

bool typedTrack(const ObjectSlot& object)
{
    return object.role == Role::Track && (object.family() == model::Family::PointMass || object.family() == model::Family::Vector);
}

/// B1's reading of the baseline with the document's track identities, sorted as B1 sorts the expected project.
LegacyProject boundBaseline(const detail::ImportedTracker& imported, const TrackerSource& source, TrackId nextTrackId)
{
    require(source.bindings.has_value());
    std::unordered_map<std::size_t, TrackId> sourceIds;
    for (const auto& binding : source.bindings.value_or(std::vector<TrackSourceBinding>{}))
    {
        sourceIds.emplace(binding.sourceIndex, binding.trackId);
    }
    std::unordered_map<TrackId, TrackId> mapping;
    for (const auto& binding : imported.bindings)
    {
        mapping.emplace(binding.trackId, sourceIds.at(binding.sourceIndex));
    }
    auto baseline = imported.project;
    remapIds(baseline, mapping, nextTrackId);
    return baseline;
}

template <typename Track> const Track& reducedTrack(const std::vector<Track>& tracks, model::ObjectId id)
{
    const auto found = std::ranges::find(tracks, id.value, &Track::id);
    require(found != tracks.end());
    return *found;
}

/// The reference descriptor a graph dependency of this owner class came from.
const model::ReferenceDescriptor* referenceField(std::string_view ownerClass, const model::Dependency& dependency)
{
    for (const auto& field : model::referenceDescriptors())
    {
        if (field.kind == dependency.kind && std::ranges::contains(field.ownerClasses, ownerClass) && dependency.slot >= field.slotBase &&
            dependency.slot - field.slotBase < field.maxSlots)
        {
            return &field;
        }
    }
    return nullptr;
}

std::string_view renderStatusName(model::RenderStatus status)
{
    switch (status)
    {
    case model::RenderStatus::Faithful:
        return "is faithful";
    case model::RenderStatus::Ambiguous:
        return "would be ambiguous";
    case model::RenderStatus::Unclassified:
        return "could bind a track of an unverified class";
    case model::RenderStatus::Rebinds:
        return "would bind another track";
    case model::RenderStatus::Lost:
        return "would bind nothing";
    case model::RenderStatus::Dangling:
        return "refers to a removed track";
    case model::RenderStatus::Unnamed:
        break;
    }
    return "refers to a track without a name";
}

/// The broad rule of B2.3: after a label change, a creation or a removal, every saved reference must render faithfully,
/// so that Tracker binds each name it reloads to the same object or to the same unresolved candidates.
void checkFaithfulReferences(const model::TrackerDocument& document)
{
    const auto& graph = document.graph();
    auto index = model::NameIndex::fromGraph(graph);
    for (const auto owner : graph.objects())
    {
        const auto* info = graph.object(owner);
        for (const auto facet : {model::Facet::Definition, model::Facet::Geometry, model::Facet::Data})
        {
            for (const auto& dependency : graph.dependencies({.object = owner, .facet = facet}))
            {
                const auto* field = referenceField(info->className, dependency);
                require(field != nullptr);
                const auto rendered = model::renderName(graph, dependency, index, field->filter);
                require(rendered.hasValue());
                if (rendered->status != model::RenderStatus::Faithful)
                {
                    const auto label = info->label.value_or(info->className);
                    throw ProjectError("Tracker export cannot keep the reference to '" + dependency.savedName + "' in " +
                                       std::string(field->id) + " of '" + label + "': it " +
                                       std::string(renderStatusName(rendered->status)) + ".");
                }
            }
        }
    }
}

/// What the export intends each object to read after a reload: its typed state, with the values a batch writes in full
/// (Tracker's loader defaults of a clip range or timing field the file did not save) and the media references the
/// destination spells.
class IntendedState
{
  public:
    void set(const ObjectSlot& object, model::TypedValue value)
    {
        m_Values.insert_or_assign(object.id, std::move(value));
    }

    [[nodiscard]] const model::TypedValue& value(const ObjectSlot& object) const
    {
        const auto found = m_Values.find(object.id);
        return found == m_Values.end() ? *object.value : found->second;
    }

    template <typename State, typename Change> void update(const ObjectSlot& object, Change change)
    {
        auto state = std::get<State>(value(object));
        change(state);
        set(object, std::move(state));
    }

  private:
    std::unordered_map<model::ObjectId, model::TypedValue> m_Values;
};

/// The live elements of `tracks` in export order: bound objects in baseline order, then created point masses and created
/// vectors, each in ascending identity.
std::vector<const ObjectSlot*> exportedTracks(const model::TrackerDocument& document)
{
    std::vector<const ObjectSlot*> bound;
    std::vector<const ObjectSlot*> masses;
    std::vector<const ObjectSlot*> vectors;
    for (const auto& object : document.objects())
    {
        if (object.role != Role::Track || object.state != model::ObjectState::Live)
        {
            continue;
        }
        if (object.binding)
        {
            bound.push_back(&object);
        }
        else if (object.family() == model::Family::PointMass)
        {
            masses.push_back(&object);
        }
        else if (object.family() == model::Family::Vector)
        {
            vectors.push_back(&object);
        }
    }
    // Node identities follow the order of the source, in which the reader built them.
    std::ranges::sort(bound, {}, [](const ObjectSlot* object) { return std::to_underlying(object->binding.value_or(NodeId{})); });
    std::ranges::sort(masses, {}, &ObjectSlot::id);
    std::ranges::sort(vectors, {}, &ObjectSlot::id);
    bound.insert(bound.end(), masses.begin(), masses.end());
    bound.insert(bound.end(), vectors.begin(), vectors.end());
    return bound;
}

/// The reimport check: the typed load of the output, matched with the intended objects through the export order (bound
/// objects in baseline order, then created point masses and created vectors in ascending identity), reads every intended
/// state, and the output's media references are the destination's.
void checkReimport(const model::TrackerDocument& document,
                   const IntendedState& intended,
                   const model::TrackerDocument& output,
                   std::span<const ObjectSlot* const> exportOrder)
{
    // An intended edit that was never composed, or composed as another value, rereads differently.
    const auto reread = [](bool same)
    {
        if (!same)
        {
            throw ProjectError("This Tracker export cannot preserve all edits or source associations (its output rereads "
                               "differently from the intended state).");
        }
    };
    reread(output.media().references == document.media().references);
    for (const auto role : {Role::Panel, Role::Clip, Role::Video, Role::ClipControl, Role::Coordinates})
    {
        const auto* expected = document.singleton(role);
        const auto* actual = output.singleton(role);
        reread((expected == nullptr) == (actual == nullptr));
        if (expected && actual)
        {
            reread(expected->className == actual->className && sameExportedState(intended.value(*expected), *actual->value));
        }
    }
    std::vector<const ObjectSlot*> tracks;
    for (const auto& object : output.objects())
    {
        if (object.role == Role::Track && object.state == model::ObjectState::Live)
        {
            tracks.push_back(&object);
        }
    }
    reread(tracks.size() == exportOrder.size());
    for (std::size_t position = 0; position < tracks.size(); ++position)
    {
        reread(tracks[position]->className == exportOrder[position]->className &&
               sameExportedState(intended.value(*exportOrder[position]), *tracks[position]->value));
    }
}

} // namespace

std::vector<model::ObjectId> exportOrder(const model::TrackerDocument& document)
{
    std::vector<model::ObjectId> ids;
    for (const auto* object : exportedTracks(document))
    {
        ids.push_back(object->id);
    }
    return ids;
}

std::string composeTrackerExport(const model::TrackerDocument& document, const ExportMedia& exportMedia)
{
    const auto media = exportMedia.kind;
    const auto movie = media == LinkedMedia::Movie;
    const auto& baselineSource = document.baseline();
    if (!baselineSource)
    {
        throw ProjectError("Tracker export requires its original source.");
    }
    const auto& xml = *baselineSource;
    // Admission on the legacy view and the baseline, in B1's order: the view is what B1's checks encode Tracker's
    // behavior on, and the source view carries the bindings its reference index validates.
    const auto view = legacyProject(document);
    const auto source = trackerSourceView(document).value_or(TrackerSource{});
    require(media != LinkedMedia::None || view.clip.mediaReferences.empty());
    auto imported = detail::validatedTrackerSource(source, view);
    if (document.associations() != model::Associations::Known || !source.bindings)
    {
        throw ProjectError("Tracker export requires original object associations.");
    }
    const auto baseline = boundBaseline(imported, source, view.nextTrackId);
    auto expected = view;
    sortTracks(expected);
    if (imported.trackFrameShift > 0)
        requireUnchangedShiftedSamples(expected, baseline);

    // The baseline's typed reading, by bound node: what every bound object read before any edit.
    const auto reading = model::TrackerDocument::importBaseline(baselineSource);
    if (!reading)
    {
        throw ProjectError(reading.error().message);
    }
    std::map<NodeId, const ObjectSlot*> before;
    for (const auto& object : reading->objects())
    {
        if (object.binding)
        {
            before.emplace(*object.binding, &object);
        }
    }
    const auto baselineOf = [&](const ObjectSlot& object) -> const ObjectSlot*
    {
        if (!object.binding)
        {
            return nullptr;
        }
        const auto found = before.find(*object.binding);
        return found == before.end() ? nullptr : found->second;
    };

    std::vector<const ObjectSlot*> createdMasses;
    std::vector<const ObjectSlot*> createdVectors;
    std::vector<const ObjectSlot*> removedTracks;
    std::vector<const ObjectSlot*> liveMasses;
    std::vector<const ObjectSlot*> liveVectors;
    std::set<TrackId> liveIds;
    for (const auto& object : document.objects())
    {
        if (!typedTrack(object))
        {
            continue;
        }
        if (object.state == model::ObjectState::Removed)
        {
            if (object.binding)
            {
                removedTracks.push_back(&object);
            }
            continue;
        }
        liveIds.insert(object.id.value);
        const bool pointMass = object.family() == model::Family::PointMass;
        (pointMass ? liveMasses : liveVectors).push_back(&object);
        if (!object.binding)
        {
            (pointMass ? createdMasses : createdVectors).push_back(&object);
        }
    }
    const auto byIdentity = [](const ObjectSlot* left, const ObjectSlot* right)
    {
        return left->id < right->id;
    };
    std::ranges::sort(createdMasses, byIdentity);
    std::ranges::sort(createdVectors, byIdentity);
    std::ranges::sort(liveMasses, byIdentity);
    std::ranges::sort(liveVectors, byIdentity);
    const bool membershipChanged = !createdMasses.empty() || !createdVectors.empty() || !removedTracks.empty();
    const auto panel = tracker::loadPanel(xml);

    SourceEdits edits(baselineSource);
    IntendedState intended;
    bool changed = false;

    // Media references for the destination: the media adapters' path patches.
    if (media != LinkedMedia::None)
    {
        const auto video =
            movie ? checkLinkedMovie(xml, expected, baseline, exportMedia.frameTimes) : checkLinkedImages(xml, expected, baseline);
        const auto& references = document.media().references;
        if (references != reading->media().references)
        {
            if (movie)
            {
                patchString(edits, video, "path", references.front());
            }
            else
            {
                patchLinkedImagePaths(edits, video, references);
            }
            const auto* object = document.singleton(Role::Video);
            require(object && object->binding == video);
            intended.update<model::VideoState>(*object,
                                               [&](model::VideoState& state)
                                               {
                                                   state.path = references.front();
                                                   if (state.paths)
                                                   {
                                                       state.paths = references;
                                                   }
                                               });
            changed = true;
        }
    }
    bool auditFinalSchema = media != LinkedMedia::None;
    bool massCoordinatesChecked = false;

    // Created tracks: appended with their family's saver.
    if (membershipChanged)
    {
        validateTrackerTrackMembership(xml, expected, source, movie);
        auditFinalSchema = true;
        require(expected.calibration || !lastProperty(xml, xml.root(), "coords"));
        if (expected.calibration)
            requireTrackerCoordinates(expected, *expected.calibration);
        massCoordinatesChecked = true;
        const auto collection = required(panel.tracks);
        require(node(xml, collection).type == PropertyType::Collection && node(xml, collection).className == "java.util.ArrayList");
        std::size_t createdFrameSlots = 0;
        for (const auto* object : createdMasses)
        {
            checkCreatedPointMass(reducedTrack(expected.pointMasses, object->id), createdFrameSlots);
            edits.append(collection, createdTrack(*object));
        }
        for (const auto* object : createdVectors)
        {
            checkCreatedVector(expected, reducedTrack(expected.vectors, object->id));
            edits.append(collection, createdTrack(*object));
        }
        changed = true;
    }

    // Renamed tracks and every reference that names them.
    std::map<TrackId, std::string> renamed;
    for (const auto& object : document.objects())
    {
        if (typedTrack(object) && object.state == model::ObjectState::Live)
        {
            if (const auto* original = baselineOf(object); original && loadedTrackName(*object.value) != loadedTrackName(*original->value))
            {
                renamed.emplace(object.id.value, loadedTrackName(*object.value));
            }
        }
    }
    const auto names = planTrackerNames(xml, renamed, liveIds, source, movie);
    if (names)
    {
        applyTrackerNames(edits, *names);
        changed = true;
        auditFinalSchema = true;
    }

    // Removed tracks: their elements go after the name and view-reference patches, so ordinal paths stay valid.
    if (membershipChanged)
    {
        patchTrackerDeletedReferences(edits, expected, source);
        for (const auto* object : removedTracks)
        {
            const auto element = node(xml, *object->binding).parent;
            require(element.has_value());
            edits.remove(element.value_or(NodeId{}));
        }
    }

    // Calibration keys through the frame-array patcher.
    const auto* coords = document.singleton(Role::Coordinates);
    const auto* coordsState = stateOf<model::CoordinatesState>(coords);
    const auto* coordsBefore = coords ? baselineOf(*coords) : nullptr;
    const auto* coordsReading = stateOf<model::CoordinatesState>(coordsBefore);
    if (coordsState && (!coordsReading || !sameExportedState(*coords->value, *coordsBefore->value)))
    {
        if (!coordsReading || !expected.calibration || !baseline.calibration)
        {
            require(false); // Creating/removing coordinate systems is not covered by this writer slice.
        }
        else
        {
            const auto object = checkCalibration(xml, *baseline.calibration, *expected.calibration, expected);
            require(object == coords->binding);
            patchCalibrationKeys(edits, object, reducedKeys(coordsReading->keys), reducedKeys(coordsState->keys));
            massCoordinatesChecked = true;
            changed = true;
        }
    }

    // The panel's unit labels, in B1's order: the descriptor's typed setter, or an append when the file did not save one.
    const auto* panelObject = document.singleton(Role::Panel);
    const auto* panelBefore = panelObject ? baselineOf(*panelObject) : nullptr;
    require(panelObject && panelBefore && panelObject->family() == model::Family::Panel);
    for (const auto id : {std::string_view("length_unit"), std::string_view("mass_unit"), std::string_view("time_unit")})
    {
        const auto& field = *model::findField(model::Family::Panel, id);
        const auto value = model::scalarValue(*panelObject->value, field);
        if (value != model::scalarValue(*panelBefore->value, field))
        {
            const auto* label = std::get_if<std::string>(&value);
            require(label != nullptr);
            patchUnit(edits, field.property, label ? *label : std::string{});
            changed = true;
        }
    }

    // The clip range is one batch: when one of its fields differs, all three are written with Tracker's loaded values.
    const auto* clip = document.singleton(Role::Clip);
    const auto* clipState = stateOf<model::ClipState>(clip);
    const auto* clipBefore = clip ? baselineOf(*clip) : nullptr;
    const auto clipReading = clipBefore ? std::get<model::ClipState>(*clipBefore->value) : model::ClipState{};
    if (clipState && (!same(clipState->startFrame, clipReading.startFrame) || !same(clipState->stepSize, clipReading.stepSize) ||
                      !same(clipState->stepCount, clipReading.stepCount)))
    {
        const auto object = checkClipRange(xml, expected.clip, media);
        require(object == clip->binding);
        const auto start = loadedOrDefault(clipState->startFrame, model::Family::Clip, "startframe");
        const auto step = loadedOrDefault(clipState->stepSize, model::Family::Clip, "stepsize");
        const auto count = loadedOrDefault(clipState->stepCount, model::Family::Clip, "stepcount");
        patchClipRange(edits, object, start, step, count);
        intended.update<model::ClipState>(*clip,
                                          [&](model::ClipState& state)
                                          {
                                              state.startFrame = start;
                                              state.stepSize = step;
                                              state.stepCount = count;
                                          });
        changed = true;
    }

    // Clip timing is one batch too: the clip's start time and the clip control's frame duration.
    const auto* control = document.singleton(Role::ClipControl);
    const auto* controlState = stateOf<model::ClipControlState>(control);
    const auto* controlBefore = control ? baselineOf(*control) : nullptr;
    const auto* controlReading = stateOf<model::ClipControlState>(controlBefore);
    if ((clipState && !same(clipState->startTime, clipReading.startTime)) ||
        (controlState && (!controlReading || !same(controlState->deltaT, controlReading->deltaT))))
    {
        const auto objects = checkClipTiming(xml, expected);
        require(clip && control && objects.clip == clip->binding && objects.control == control->binding && controlState);
        const auto startTime =
            loadedOrDefault(clipState ? clipState->startTime : model::missing<double>(), model::Family::Clip, "starttime");
        const auto frameDuration = required(controlState ? model::finiteValue(controlState->deltaT) : std::nullopt);
        patchClipTiming(edits, objects, startTime, frameDuration);
        intended.update<model::ClipState>(*clip, [&](model::ClipState& state) { state.startTime = startTime; });
        intended.update<model::ClipControlState>(*control, [&](model::ClipControlState& state) { state.deltaT = frameDuration; });
        changed = true;
    }

    // Point masses in identity order: the mass through its typed setter, marks through the frame-array patcher and the
    // key frames with them.
    for (const auto* object : liveMasses)
    {
        const auto& current = reducedTrack(expected.pointMasses, object->id);
        require(std::isfinite(current.mass) && current.mass >= MIN_POINT_MASS);
        const auto* original = baselineOf(*object);
        if (!original)
        {
            continue;
        }
        const auto& state = std::get<model::PointMassState>(*object->value);
        const auto& reading = std::get<model::PointMassState>(*original->value);
        const auto element = *object->binding;
        if (!same(state.mass, reading.mass))
        {
            require(state.mass.has_value());
            patchDouble(edits, element, model::findField(model::Family::PointMass, "mass")->property, state.mass.value_or(0.0));
            changed = true;
        }
        const bool keysChanged = state.keyFrames != reading.keyFrames;
        if (!sameValues(state.marks, reading.marks) || keysChanged)
        {
            if (!massCoordinatesChecked)
            {
                require(expected.calibration || !lastProperty(xml, xml.root(), "coords"));
                if (expected.calibration)
                    requireTrackerCoordinates(expected, *expected.calibration);
                massCoordinatesChecked = true;
            }
            const auto originalMarks = reducedMarks(reading.marks);
            auto keys = readKeys(xml, element, originalMarks);
            if (keysChanged)
            {
                require(state.keyFrames.has_value());
                keys = state.keyFrames ? std::set<int>(state.keyFrames->begin(), state.keyFrames->end()) : std::set<int>{};
            }
            const auto changes = patchMarkFrames(edits, element, originalMarks, reducedMarks(state.marks));
            checkKeyFrames(keys, !state.marks.empty());
            // Tracker's loaded key frames: the saved ones, or every remaining mark when none is saved.
            const auto loadedKeys = model::loadedKeyFrames(state);
            checkKeyFramesFollowMarks(std::set<int>(loadedKeys.begin(), loadedKeys.end()), changes, model::loadedName(state.name));
            if (keysChanged)
            {
                patchKeyFrames(edits, element, keys);
            }
            changed = true;
        }
    }

    // Vectors in identity order: samples through the frame-array patcher.
    for (const auto* object : liveVectors)
    {
        const auto* original = baselineOf(*object);
        if (!original)
        {
            continue;
        }
        const auto& state = std::get<model::VectorState>(*object->value);
        const auto& reading = std::get<model::VectorState>(*original->value);
        if (!sameValues(state.samples, reading.samples))
        {
            patchVectorFrames(edits, *object->binding, reducedSamples(reading.samples), reducedSamples(state.samples));
            changed = true;
        }
    }

    // Never produce a superficially valid file that silently omits an edit: every other difference is one no command
    // makes, or one no descriptor composes (a created singleton, a removed or changed untyped object).
    for (const auto& object : document.objects())
    {
        if (object.state == model::ObjectState::Removed)
        {
            require(typedTrack(object) && object.binding.has_value());
            continue;
        }
        const auto* original = baselineOf(object);
        if (!original)
        {
            require(typedTrack(object) && !object.binding);
            continue;
        }
        require(original->className == object.className && original->family() == object.family());
        if (object.family() == model::Family::Unknown)
        {
            require(*original->value == *object.value);
            continue;
        }
        for (const auto& field : model::fieldDescriptors(object.family()))
        {
            require(field.commandEdits || !differs(*object.value, *original->value, field));
        }
    }
    if (!changed)
    {
        return std::string(xml.bytes());
    }

    // Admission on the composed output, before anything is written.
    auto rendered = renderEdits(edits);
    const auto output = model::TrackerDocument::importBaseline(rendered.source);
    if (!output)
    {
        throw ProjectError(output.error().message);
    }
    const auto& emitted = *rendered.source;
    checkEmittedViews(emitted, expected, media);
    const auto exportOrder = exportedTracks(document);
    std::vector<const ObjectSlot*> emittedTracks;
    for (const auto& object : output->objects())
    {
        if (typedTrack(object) && object.state == model::ObjectState::Live)
        {
            emittedTracks.push_back(&object);
        }
    }
    std::vector<const ObjectSlot*> intendedTracks;
    std::ranges::copy_if(exportOrder, std::back_inserter(intendedTracks), [](const ObjectSlot* object) { return typedTrack(*object); });
    require(emittedTracks.size() == intendedTracks.size());
    std::unordered_map<TrackId, NodeId> emittedObjects;
    for (std::size_t position = 0; position < emittedTracks.size(); ++position)
    {
        emittedObjects.emplace(intendedTracks[position]->id.value, emittedTracks[position]->binding.value_or(NodeId{}));
    }
    checkEmittedTracks(emitted, expected, media, auditFinalSchema, emittedObjects, rendered.xml.size());
    checkReimport(document, intended, *output, exportOrder);
    if (membershipChanged || names)
    {
        checkFaithfulReferences(document);
    }
    return std::move(rendered.xml);
}

namespace detail
{

void requireFaithfulReferences(const model::TrackerDocument& document)
{
    checkFaithfulReferences(document);
}

bool CompositionComparison::sameDecision() const noexcept
{
    return legacy.index() == descriptor.index();
}

bool CompositionComparison::equivalent() const noexcept
{
    if (!sameDecision())
    {
        return false;
    }
    const auto* exported = std::get_if<std::string>(&legacy);
    const auto* other = std::get_if<std::string>(&descriptor);
    return !exported || (other && *exported == *other);
}

bool CompositionComparison::identical() const noexcept
{
    return equivalent() && legacy == descriptor;
}

std::string CompositionComparison::describe() const
{
    const auto* legacyXml = std::get_if<std::string>(&legacy);
    const auto* descriptorXml = std::get_if<std::string>(&descriptor);
    if (legacyXml && descriptorXml)
    {
        if (*legacyXml == *descriptorXml)
        {
            return "both export the same " + std::to_string(legacyXml->size()) + " bytes";
        }
        const auto [first, second] = std::ranges::mismatch(*legacyXml, *descriptorXml);
        return "the exports differ from byte " + std::to_string(first - legacyXml->begin()) + " (B1 " + std::to_string(legacyXml->size()) +
               " bytes, descriptors " + std::to_string(descriptorXml->size()) + ")";
    }
    if (legacyXml)
    {
        return "B1 exports, the descriptors refuse: " + std::get<CompositionRefusal>(descriptor).message;
    }
    const auto& refusal = std::get<CompositionRefusal>(legacy).message;
    if (descriptorXml)
    {
        return "B1 refuses (" + refusal + "), the descriptors export";
    }
    const auto& other = std::get<CompositionRefusal>(descriptor).message;
    return refusal == other ? "both refuse: " + refusal : "both refuse, B1: " + refusal + " Descriptors: " + other;
}

CompositionComparison compareWithDescriptorComposition(const LegacyProject& project,
                                                       const TrackerSource& source,
                                                       const ExportMedia& media,
                                                       CompositionOutcome legacy)
{
    CompositionComparison comparison{.legacy = std::move(legacy), .descriptor = CompositionRefusal{}};
    try
    {
        // The typed document a B2.4a session would hold for this reduced project and source.
        const auto document = typedDocumentFromReduced(project, source);
        comparison.descriptor = composeTrackerExport(document, media);
    }
    catch (const ProjectError& error)
    {
        comparison.descriptor = CompositionRefusal{.message = error.what()};
    }
    return comparison;
}

} // namespace detail
} // namespace motrack
