#include "tracker_writer.hpp"

#include "local_media_path.hpp"
#include "measurements.hpp"
#include "model/codecs.hpp"
#include "osp/java_text.hpp"
#include "osp/value.hpp"
#include "model/source_edits.hpp"
#include "tracker_archive.hpp"
#include "tracker_archive_writer.hpp"
#include "tracker_import.hpp"
#include "tracker_rename.hpp"
#include "workspace_document.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QString>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
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

void patchUnit(SourceEdits& edits, std::string_view name, const std::string& unit)
{
    require(model::isTrackerUnitLabel(unit));
    patchString(edits, edits.source().root(), name, unit);
}

enum class LinkedMedia : std::uint8_t
{
    None,
    Images,
    Movie
};

/// The panel's VideoClip object, audited as the only clip representation the writers patch.
NodeId videoClipObject(const osp::Source& xml)
{
    const auto property = lastProperty(xml, xml.root(), "videoclip");
    const auto object = required(objectOf(xml, property));
    require(node(xml, object).className == tracker::VIDEO_CLIP_CLASS);
    return object;
}

void patchClipRange(SourceEdits& edits, const Clip& clip, LinkedMedia media)
{
    const auto& xml = edits.source();
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
    patchInt(edits, object, "startframe", clip.firstFrame);
    patchInt(edits, object, "stepsize", clip.stepSize);
    patchInt(edits, object, "stepcount", count);
}

void patchClipTiming(SourceEdits& edits, const LegacyProject& project)
{
    const auto& xml = edits.source();
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
    patchDouble(edits, clipObject, "starttime", clip.startTimeMs);
    patchDouble(edits, control, "delta_t", required(clip.frameDurationMs));
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

void patchMarks(SourceEdits& edits, NodeId object, const std::vector<Mark>& original, const std::vector<Mark>& current)
{
    const auto& xml = edits.source();
    auto keys = readKeys(xml, object, original);
    const auto oldKeys = keys;
    const auto changes = patchFrameData(edits,
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
    for (const auto frame : changes.removed)
    {
        keys.erase(frame);
    }
    keys.insert(changes.updated.begin(), changes.updated.end());
    // An empty key list makes Tracker promote every remaining automatic mark to a manual key.
    // Until that distinction is in the editable model, reject edits that cannot retain it.
    require(!keys.empty() || current.empty());
    if (keys == oldKeys)
    {
        return;
    }
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

void patchCalibration(SourceEdits& edits, const Calibration& original, const Calibration& current, const LegacyProject& project)
{
    const auto& xml = edits.source();
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
    patchFrameData(edits,
                   object,
                   original.keyframes,
                   current.keyframes,
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

void sortTracks(LegacyProject& project)
{
    std::ranges::sort(project.pointMasses, {}, &PointMass::id);
    std::ranges::sort(project.vectors, {}, &VectorTrack::id);
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

/// A new independent track as Tracker's PointMass/Vector loaders save it: PointMass writes `mass` before
/// TTrack's name, color, footprint, visible and trail (`locked` only when true), then its frame arrays.
/// Native appearance is not yet persisted; these are the audited interchange defaults.
osp::PropertyValue newTrack(TrackKind kind, const std::string& name, double mass, std::vector<osp::PropertyValue> arrays)
{
    const bool vector = kind == TrackKind::Vector;
    require(name.find("]]>") == std::string::npos);
    osp::ObjectValue object{.className = std::string(vector ? tracker::VECTOR_CLASS : tracker::POINT_MASS_CLASS)};
    if (!vector)
        object.properties.push_back(osp::doubleProperty("mass", mass));
    object.properties.push_back(osp::stringProperty("name", name));
    osp::ObjectValue color{.className = "java.awt.Color"};
    for (const auto& [channel, value] :
         std::array<std::pair<const char*, int>, 4>{{{"red", 255}, {"green", 0}, {"blue", vector ? 255 : 0}, {"alpha", 255}}})
        color.properties.push_back(osp::intProperty(channel, value));
    object.properties.push_back(osp::objectProperty("color", std::move(color)));
    object.properties.push_back(osp::stringProperty("footprint", vector ? "Footprint.BoldArrow" : "Footprint.Diamond"));
    object.properties.push_back(osp::booleanProperty("visible", true));
    object.properties.push_back(osp::booleanProperty("trail", true));
    for (auto& array : arrays)
        object.properties.push_back(std::move(array));
    return osp::objectProperty("item", std::move(object));
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

bool patchLinkedImagePaths(SourceEdits& edits, const LegacyProject& project, LegacyProject& baseline)
{
    const auto& xml = edits.source();
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
    if (project.clip.mediaReferences == baseline.clip.mediaReferences)
        return false;
    const auto paths = required(uniqueProperty(xml, video, "paths"));
    for (std::size_t index = 0; index < count; ++index)
    {
        const auto entry = required(uniqueProperty(xml, paths, "[" + std::to_string(index) + "]"));
        require(node(xml, entry).type == PropertyType::String && clip.mediaReferences[index].find("]]>") == std::string::npos);
        if (const auto current = osp::PropertyView(xml, entry).stringValue(); !current || *current != clip.mediaReferences[index])
            edits.setString(entry, clip.mediaReferences[index]);
    }
    patchString(edits, video, "path", clip.mediaReferences.front());
    baseline.clip.mediaReferences = project.clip.mediaReferences;
    return true;
}

bool patchLinkedMoviePath(SourceEdits& edits, const LegacyProject& project, LegacyProject& baseline, std::span<const double> frameTimes)
{
    const auto& xml = edits.source();
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
    if (project.clip.mediaReferences == baseline.clip.mediaReferences)
        return false;
    patchString(edits, video, "path", project.clip.mediaReferences.front());
    baseline.clip.mediaReferences = project.clip.mediaReferences;
    return true;
}

std::string renderTrackerProjectImpl(const LegacyProject& project,
                                     const TrackerSource& source,
                                     LinkedMedia media,
                                     std::span<const double> frameTimes = {})
{
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
            // New native tracks have no source metadata to copy. Use the audited independent-mass defaults.
            require(std::ranges::all_of(current.marks, [](const auto& mark) { return mark.frame >= 0 && mark.frame <= 500000; }));
            require(std::isfinite(current.mass) && current.mass >= MIN_POINT_MASS);
            std::vector<osp::PropertyValue> arrays;
            if (!current.marks.empty())
            {
                const auto last = std::ranges::max(current.marks, {}, &Mark::frame).frame;
                createdFrameSlots += static_cast<std::size_t>(last) + 1;
                require(createdFrameSlots <= 1000000);
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
            require(
                expected.pointMasses.empty() &&
                std::ranges::all_of(expected.unsupportedTrackTypes, [](const auto& type) { return type == tracker::COORD_AXES_CLASS; }));
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
            patchFrameData(edits,
                           objects.at(current.id),
                           original.samples,
                           current.samples,
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
    auto rendered = edits.render();
    if (!rendered)
    {
        throw ProjectError("This Tracker export cannot preserve all edits or source associations (" + rendered.error() + ").");
    }
    auto output = detail::importTracker(rendered->source);
    const auto& emitted = *output.source;
    // Guards below audit the emitted document, including appended metadata and patched samples.
    requireStableTableColumns(emitted, expected, media == LinkedMedia::Movie);
    // Pinned Tracker 6.3.5 never initializes saved VectorSum members and loses their names on resave.
    // Do not let clearing all independent samples bypass the nonempty-vector safeguards below.
    require(std::ranges::find(expected.unsupportedTrackTypes, "org.opensourcephysics.cabrillo.tracker.VectorSum") ==
            expected.unsupportedTrackTypes.end());
    require(output.bindings.size() == mapping.size());
    std::unordered_map<TrackId, NodeId> emittedObjects;
    for (const auto& binding : output.bindings)
    {
        const auto& item = output.panel.trackItems.at(binding.sourceIndex);
        emittedObjects.emplace(mapping.at(binding.trackId), item.object.value_or(NodeId{}));
    }
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
    require(rendered->xml.size() <= MAX_TRACKER_XML_BYTES);
    auto reparsed = std::move(output.project);
    remapIds(reparsed, mapping, project.nextTrackId);
    require(reparsed == expected);
    return std::move(rendered->xml);
}

void writeXml(const QString& path, const std::string& xml)
{
    require(path.endsWith(".trk", Qt::CaseInsensitive));
    QSaveFile output(path);
    const auto size = static_cast<qint64>(xml.size());
    if (!output.open(QIODevice::WriteOnly) || output.write(xml.data(), size) != size || !output.commit())
    {
        throw ProjectError("Could not write Tracker project: " + output.errorString().toStdString());
    }
}

void requireLocalReference(const std::string& reference)
{
    const auto local = QString::fromStdString(reference);
    require(!local.isEmpty() && local.size() <= 4096 && local.toStdString() == reference && !local.contains("!/") &&
            !local.contains("://") && !local.startsWith(':') && !local.contains("]]>"));
    for (const auto point : local.toUcs4())
        require(QChar::category(point) != QChar::Other_Control && QChar::category(point) != QChar::Other_Format);
}

} // namespace

std::string renderTrackerProject(const LegacyProject& project, const TrackerSource& source)
{
    return renderTrackerProjectImpl(project, source, LinkedMedia::None);
}

void writeTrackerProject(const QString& path, const LegacyProject& project, const TrackerSource& source)
{
    writeXml(path, renderTrackerProject(project, source));
}

namespace
{
std::string prepareTrackerDocument(const QString& path, const WorkspaceDocument& document, const QString& mediaProjectPath)
{
    if (!document.trackerSource)
        throw ProjectError("Tracker export requires its original source.");
    const auto& source = *document.trackerSource;
    require(!document.useDisplayOrientation && !document.archivePayload && !document.archiveMember && document.archiveReference.empty());
    require(document.movie ? document.alignMovieToClip && !document.movieFrameTimes.empty() : document.movieFrameTimes.empty());
    auto project = document.project;
    const auto media = document.movie ? LinkedMedia::Movie : project.clip.mediaReferences.empty() ? LinkedMedia::None : LinkedMedia::Images;
    if (media != LinkedMedia::None)
    {
        require(project.clip.mediaReferences.size() <= 10000 && !mediaProjectPath.isEmpty() && QFileInfo(mediaProjectPath).isAbsolute());
        // A file symlink can be opened under two different project directories. Require a
        // regular destination; directory aliases are resolved consistently by the reader.
        require(!QFileInfo(path).isSymbolicLink());
        const auto targetParent = localProjectDirectory(path);
        require(!targetParent.isEmpty());
        const QDir targetDirectory(targetParent);
        for (auto& reference : project.clip.mediaReferences)
        {
            requireLocalReference(reference);
            const auto resolved = workspaceMediaPath(document, mediaProjectPath, reference);
            require(!resolved.isEmpty() && !sameLocalFile(resolved, targetDirectory.filePath(QFileInfo(path).fileName())));
            QFile file(resolved);
            if (!QFileInfo(resolved).isFile() || !file.open(QIODevice::ReadOnly))
                throw ProjectError("Linked media is missing or unreadable: " + resolved.toStdString());
            reference = QDir::fromNativeSeparators(targetDirectory.relativeFilePath(resolved)).toStdString();
            requireLocalReference(reference); // Rebasing can introduce a forbidden marker from a parent directory.
            // Compare file identity: a destination spelled with another case on Windows keeps its spelling (#297).
            require(sameLocalFile(resolveLocalMediaPath(path, reference), resolved));
        }
        // Native frame bounds are runtime metadata, not an edit to the preserved standard clip.
        if (project.clip.sourceFrameCount)
        {
            const auto count = *project.clip.sourceFrameCount;
            const auto clipEnd = static_cast<std::int64_t>(project.clip.firstFrame) +
                                 (static_cast<std::int64_t>(project.clip.stepCount.value_or(0)) - 1) * project.clip.stepSize + 1;
            require(count > 0 &&
                    (media == LinkedMedia::Images ? std::cmp_equal(count, project.clip.mediaReferences.size())
                                                  : std::cmp_equal(count, document.movieFrameTimes.size()) || count == clipEnd));
            project.clip.sourceFrameCount.reset();
        }
    }
    return renderTrackerProjectImpl(project, source, media, document.movieFrameTimes);
}
} // namespace

void writeTrackerDocument(const QString& path, const WorkspaceDocument& document, const QString& mediaProjectPath)
{
    writeXml(path, prepareTrackerDocument(path, document, mediaProjectPath));
}

namespace
{
std::string prepareArchiveMember(const WorkspaceDocument& document, const std::atomic_bool* cancelled)
{
    require(!cancelled || !cancelled->load());
    if (!document.archivePayload || !document.archiveMember || !document.trackerSource)
        throw ProjectError("Tracker archive export requires its original archive and selected project.");
    // Tracker 6.3.5 does not extract archived AVI files for Xuggle (#140). Limit this
    // adapter to the MOV container covered by actual archive load/save/reopen acceptance.
    if (document.movie)
        require(
            document.project.clip.mediaReferences.size() == 1 &&
            QFileInfo(QString::fromStdString(document.project.clip.mediaReferences.front())).suffix().compare("mov", Qt::CaseInsensitive) ==
                0);
    require(document.archiveReference.empty());
    const auto& archive = *document.archivePayload;
    const auto member = QString::fromStdString(*document.archiveMember);
    require(member.toStdString() == *document.archiveMember);
    require(archive.memberMatchesSource(member, cancelled));
    const auto originalPath = archive.projectPath(member);
    QFile original(originalPath);
    require(original.open(QIODevice::ReadOnly) && original.size() <= static_cast<qint64>(MAX_TRACKER_XML_BYTES) &&
            original.readAll().toStdString() == document.trackerSource->xml);
    auto baseline = parseLegacyProject(document.trackerSource->xml);
    archive.resolveMedia(baseline, member);
    require(baseline.clip.mediaReferences == document.project.clip.mediaReferences);
    std::unordered_set<std::string> checkedMedia;
    for (const auto& reference : document.project.clip.mediaReferences)
        if (checkedMedia.insert(reference).second && !archive.memberMatchesSource(QString::fromStdString(reference), cancelled))
            throw ProjectError("Tracker archive media changed after import; unchanged-media export cannot preserve these edits.");
    auto snapshot = document;
    snapshot.archivePayload.reset();
    snapshot.archiveMember.reset();
    // Imported archive references are validated archive-root-relative members, independent of
    // the original archive's location or the companion document's later location.
    const auto xml = prepareTrackerDocument(originalPath, snapshot, QDir(archive.root()).filePath("media-base.motrack"));
    auto exported = parseLegacyProject(xml);
    archive.resolveMedia(exported, member);
    require(exported.clip.mediaReferences == baseline.clip.mediaReferences);
    return xml;
}
} // namespace

void writeTrackerArchiveDocument(const QString& path, const WorkspaceDocument& document, const std::atomic_bool* cancelled)
{
    writeTrackerArchiveDocuments(path, std::span(&document, 1), cancelled);
}

void writeTrackerArchiveDocuments(const QString& path, std::span<const WorkspaceDocument> documents, const std::atomic_bool* cancelled)
{
    require(!cancelled || !cancelled->load());
    require(!documents.empty() && documents.front().archivePayload);
    const auto& source = *documents.front().archivePayload;
    require(documents.size() <= static_cast<std::size_t>(source.projects().size()));
    std::unordered_set<std::string> members;
    const QFileInfo target(path);
    const auto parent = QFileInfo(target.absolutePath()).canonicalFilePath();
    require(!parent.isEmpty() && !target.isSymLink());
    const auto destination = parent + '/' + target.fileName();
    for (const auto& document : documents)
    {
        require(!cancelled || !cancelled->load());
        require(document.archivePayload && document.archiveMember && document.archivePayload->sourceDigest() == source.sourceDigest() &&
                members.insert(archiveMemberKey(*document.archiveMember)).second);
        require(!QFileInfo(document.archivePayload->sourcePath()).isSymLink());
        // Independently reopened companions own separate source/extraction trees. Protect every
        // owner's paths, even though the common original bytes are read only from the first owner.
        for (const auto& owned : {document.archivePayload->root(), QFileInfo(document.archivePayload->sourcePath()).absolutePath()})
        {
            const auto root = QFileInfo(owned).canonicalFilePath();
            require(!root.isEmpty() && destination != root && !destination.startsWith(root + '/'));
        }
    }
    std::vector<std::string> rendered;
    rendered.reserve(documents.size());
    qint64 remaining = MAX_TRACKER_EXTRACTED_BYTES;
    for (const auto& document : documents)
    {
        auto xml = prepareArchiveMember(document, cancelled);
        require(xml.size() <= MAX_TRACKER_XML_BYTES && std::cmp_less_equal(xml.size(), remaining));
        remaining -= static_cast<qint64>(xml.size());
        rendered.push_back(std::move(xml));
    }
    // Create borrowed views only once their owners can no longer move (including small strings).
    std::vector<TrackerArchiveReplacement> replacements;
    replacements.reserve(documents.size());
    for (std::size_t i = 0; i < documents.size(); ++i)
    {
        const auto& member = documents[i].archiveMember;
        if (!member)
            throw ProjectError("Tracker archive export requires a selected project.");
        replacements.push_back({.member = QString::fromStdString(*member), .xml = rendered[i]});
    }
    writeTrackerArchive(path, source, replacements, cancelled);
}

} // namespace motrack
