#include "tracker_rename.hpp"

#include "model/codecs.hpp"
#include "model/source_edits.hpp"
#include "osp/java_text.hpp"
#include "tracker_import.hpp"
#include "tracker_references.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <map>
#include <ranges>
#include <set>

namespace motrack
{
namespace
{
constexpr std::string_view TRACKER = "org.opensourcephysics.cabrillo.tracker.";
constexpr std::string_view MEDIA = "org.opensourcephysics.media.core.";

using osp::NodeId;
using osp::PropertyType;

void require(bool valid)
{
    if (!valid)
    {
        throw ProjectError("This Tracker rename has unsupported content, names or relationships.");
    }
}

std::string trackerClass(std::string_view name)
{
    return std::string(TRACKER) + std::string(name);
}

std::string media(std::string_view name)
{
    return std::string(MEDIA) + std::string(name);
}

/// The unique property with this name among a node's children (an object's properties or an array's elements).
NodeId property(const osp::Source& xml, NodeId owner, std::string_view name)
{
    std::optional<NodeId> result;
    for (const auto child : xml.node(owner).children)
    {
        if (xml.node(child).kind == osp::NodeKind::Property && xml.node(child).name == name)
        {
            require(!result);
            result = child;
        }
    }
    require(result.has_value());
    return result.value_or(NodeId{});
}

/// The object of a non-null object property.
NodeId objectOf(const osp::Source& xml, NodeId property)
{
    const auto& node = xml.node(property);
    require(node.type == PropertyType::Object && !node.children.empty());
    return node.children.front();
}

std::optional<std::uint32_t> elementIndex(std::string_view label)
{
    if (label.size() < 3 || label.front() != '[' || label.back() != ']')
    {
        return std::nullopt;
    }
    const auto digits = label.substr(1, label.size() - 2);
    std::uint32_t value = 0;
    const auto* begin = std::to_address(digits.begin());
    const auto* end = std::to_address(digits.end());
    const auto [last, error] = std::from_chars(begin, end, value);
    if (error != std::errc{} || last != end || std::to_string(value) != digits)
    {
        return std::nullopt;
    }
    return value;
}

std::size_t denseArraySize(const osp::Source& xml, NodeId array)
{
    std::size_t count = 0;
    for (const auto cell : xml.node(array).children)
    {
        require(xml.node(cell).name == "[" + std::to_string(count) + "]");
        ++count;
    }
    return count;
}

std::size_t modelNamesSize(const osp::Source& xml, NodeId array)
{
    const auto count = denseArraySize(xml, array);
    std::set<std::string> names;
    for (const auto cell : xml.node(array).children)
    {
        const auto& text = xml.node(cell).content;
        require(!text.empty() && names.insert(text).second);
    }
    return count;
}

std::size_t modelValuesSize(const osp::Source& xml, NodeId array)
{
    const auto& children = xml.node(array).children;
    if (!children.empty() && xml.node(children.front()).name == "array")
    {
        const auto& text = xml.node(children.front()).content;
        require(text.size() <= 160000);
        const auto values = osp::parseCompactArray(text, "[D");
        require(values && values->kind == osp::CompactArray::Kind::Double && values->doubles.size() <= 10000);
        require(std::ranges::all_of(values->doubles, [](double value) { return std::isfinite(value); }));
        return values->doubles.size();
    }
    const auto count = denseArraySize(xml, array);
    for (const auto cell : children)
    {
        const auto value = osp::parseJavaDouble(xml.node(cell).content);
        require(value && std::isfinite(*value));
    }
    return count;
}

/// The Tracker number-format grammar MoTrack audits: "0" with up to twelve decimal zeros.
bool decimalPattern(std::string_view pattern)
{
    if (pattern == "0")
    {
        return true;
    }
    if (pattern.size() < 3 || pattern.size() > 14 || !pattern.starts_with("0."))
    {
        return false;
    }
    return std::ranges::all_of(pattern.substr(2), [](char character) { return character == '0'; });
}

// A reference index alone cannot establish rename eligibility. Admit only the audited
// schemas below. Other fields/classes remain preserved by native saving but cannot bypass this gate.
using Fields = std::map<std::string, std::string, std::less<>>;
Fields fields(std::string_view name, bool movie)
{
    if (name == trackerClass("TrackerPanel"))
    {
        return {{"semantic_version", "string"},
                {"width", "double"},
                {"height", "double"},
                {"magnification", "double"},
                {"center_x", "int"},
                {"center_y", "int"},
                {"description", "string"},
                {"author", "string"},
                {"contact", "string"},
                {"hide_description", "boolean"},
                {"length_unit", "string"},
                {"mass_unit", "string"},
                {"time_unit", "string"},
                {"units_visible", "boolean"},
                {"radians", "boolean"},
                {"videoclip", "object:" + media("VideoClip")},
                {"clipcontrol", "object:" + media(movie ? "VideoClipControl" : "StepperClipControl")},
                {"coords", "object:" + media("ImageCoordSystem")},
                {"tracks", "collection:java.util.ArrayList"},
                {"selectedtrack", "string"},
                {"dividers", "array:[D"},
                {"selected_view_types", "array:[I"},
                {"selected_track_views", "string"},
                {"views", "array:[[L" + trackerClass("TView;")},
                {"toolbar", "object:" + trackerClass("TToolBar")},
                {"track_control_x", "int"},
                {"track_control_y", "int"}};
    }
    if (name == trackerClass("PointMass") || name == trackerClass("CenterOfMass") || name == trackerClass("Vector") ||
        name == trackerClass("CoordAxes") || name == trackerClass("Protractor") || name == trackerClass("TapeMeasure") ||
        name == trackerClass("DynamicParticle") || name == trackerClass("DynamicSystem"))
    {
        Fields result{{"name", "string"},
                      {"description", "string"},
                      {"color", "object:java.awt.Color"},
                      {"footprint", "string"},
                      {"visible", "boolean"},
                      {"trail", "boolean"},
                      {"locked", "boolean"}};
        if (name.ends_with(".PointMass") || name.ends_with(".CenterOfMass"))
        {
            result.insert(
                {{"mass", "double"}, {"framedata", "array:[L" + trackerClass("PointMass$FrameData;")}, {"keyFrames", "array:[I"}});
            if (name.ends_with(".PointMass"))
                result.emplace("number_formats", "array:[Ljava.lang.String;");
            if (name.ends_with(".CenterOfMass"))
                result.emplace("masses", "collection:java.util.ArrayList");
        }
        else if (name.ends_with(".Vector"))
        {
            result.insert({{"framedata", "array:[L" + trackerClass("Vector$FrameData;")}});
        }
        else if (name.ends_with(".Protractor"))
        {
            result.insert({{"fixed", "boolean"}, {"framedata", "array:[[D"}, {"ruler_visible", "boolean"}});
        }
        else if (name.ends_with(".TapeMeasure"))
        {
            result.insert({{"attachments", "array:[Ljava.lang.String;"},
                           {"fixedtape", "boolean"},
                           {"fixedlength", "boolean"},
                           {"readonly", "boolean"},
                           {"stickmode", "boolean"},
                           {"framedata", "array:[L" + trackerClass("TapeMeasure$FrameData;")},
                           {"worldlengths", "array:[Ljava.lang.Double;"}});
        }
        else if (name.ends_with(".DynamicParticle") || name.ends_with(".DynamicSystem"))
        {
            result.insert({{"mass", "double"},
                           {"user_parameters", "array:[Lorg.opensourcephysics.tools.Parameter;"},
                           {"initial_values", "array:[Lorg.opensourcephysics.tools.Parameter;"},
                           {"main_functions", "array:[Lorg.opensourcephysics.tools.UserFunction;"}});
            if (name.ends_with(".DynamicSystem"))
                result.emplace("particles", "array:[Ljava.lang.String;");
            else
                result.insert({{"in_system", "boolean"}, {"booster", "string"}});
        }
        return result;
    }
    if (name == "org.opensourcephysics.tools.Parameter")
    {
        return {{"name", "string"},
                {"function", "string"},
                {"editable", "boolean"},
                {"name_editable", "boolean"},
                {"description", "string"},
                {"synced", "boolean"}};
    }
    if (name == "org.opensourcephysics.tools.UserFunction")
    {
        return {{"name", "string"},
                {"description", "string"},
                {"name_editable", "boolean"},
                {"parameter_names", "array:[Ljava.lang.String;"},
                {"parameter_values", "array:[D"},
                {"parameter_descriptions", "array:[Ljava.lang.String;"},
                {"variables", "array:[Ljava.lang.String;"},
                {"expression", "string"}};
    }
    if (name == trackerClass("PointMass$FrameData"))
    {
        return {{"x", "double"}, {"y", "double"}};
    }
    if (name == trackerClass("Vector$FrameData"))
    {
        return {{"xtail", "double"}, {"ytail", "double"}, {"xcomponent", "double"}, {"ycomponent", "double"}};
    }
    if (name == trackerClass("TapeMeasure$FrameData"))
    {
        return {{"x1", "double"}, {"y1", "double"}, {"x2", "double"}, {"y2", "double"}};
    }
    if (name == "java.awt.Color")
    {
        return {{"red", "int"}, {"green", "int"}, {"blue", "int"}, {"alpha", "int"}};
    }
    if (name == media("VideoClip"))
    {
        return {{"startframe", "int"},
                {"stepsize", "int"},
                {"stepcount", "int"},
                {"starttime", "double"},
                {"readout", "string"},
                {"video", movie ? "object:org.opensourcephysics.media.xuggle.XuggleVideo" : "object:" + media("ImageVideo")},
                {"video_framecount", "int"},
                {"playallsteps", "boolean"}};
    }
    if (name == media("ImageVideo"))
    {
        return {{"paths", "array:[Ljava.lang.String;"}, {"path", "string"}, {"delta_t", "double"}};
    }
    if (movie && name == "org.opensourcephysics.media.xuggle.XuggleVideo")
        return {{"path", "string"}};
    if (name == media("StepperClipControl") || (movie && name == media("VideoClipControl")))
    {
        return {{"rate", "double"}, {"delta_t", "double"}, {"frame", "int"}};
    }
    if (name == media("ImageCoordSystem"))
    {
        return {{"fixedorigin", "boolean"},
                {"fixedangle", "boolean"},
                {"fixedscale", "boolean"},
                {"locked", "boolean"},
                {"framedata", "array:[L" + media("ImageCoordSystem$FrameData;")}};
    }
    if (name == media("ImageCoordSystem$FrameData"))
    {
        return {{"xorigin", "double"}, {"yorigin", "double"}, {"angle", "double"}, {"xscale", "double"}, {"yscale", "double"}};
    }
    if (name == trackerClass("TableTView"))
    {
        return {{"selected_track", "string"},
                {"track_columns", "array:[[Ljava.lang.String;"},
                {"column_formats", "array:[[[Ljava.lang.String;"}};
    }
    if (name == trackerClass("TToolBar"))
    {
        return {{"trace", "boolean"},
                {"position", "boolean"},
                {"velocity", "boolean"},
                {"acceleration", "boolean"},
                {"labels", "boolean"},
                {"multiply_by_mass", "boolean"},
                {"trail_length", "int"},
                {"stretch", "int"},
                {"stretch_acceleration", "int"}};
    }
    if (name == trackerClass("PlotTView"))
    {
        return {{"selected_track", "string"}, {"track_views", "collection:java.util.ArrayList"}};
    }
    if (name == trackerClass("PlotTrackView"))
    {
        const auto plot = "object:" + trackerClass("TrackPlottingPanel");
        return {{"track", "string"}, {"linked", "boolean"}, {"plot0", plot}, {"plot1", plot}, {"plot2", plot}};
    }
    if (name == trackerClass("TrackPlottingPanel"))
    {
        return {
            {"x_var", "string"}, {"y_var", "string"}, {"lines", "boolean"}, {"points", "boolean"}, {"guests", "array:[Ljava.lang.String;"}};
    }
    require(false);
    return {};
}

struct RenameSchema
{
    const osp::Source& xml;
    unsigned entries = 0;
    bool movie = false;

    [[nodiscard]] std::string_view text(NodeId node) const
    {
        return xml.node(node).content;
    }

    void object(NodeId value, std::string_view expected, unsigned depth = 0)
    {
        require(depth < 32 && xml.node(value).kind == osp::NodeKind::Object);
        const auto& actual = xml.node(value).className;
        require(actual == expected ||
                (expected == trackerClass("TView") && (actual == trackerClass("TableTView") || actual == trackerClass("PlotTView"))));
        const auto allowed = fields(actual, movie);
        std::set<std::string, std::less<>> seen;
        for (const auto child : xml.node(value).children)
        {
            const auto& field = xml.node(child).name;
            const auto name = field.value_or(std::string{});
            require(field.has_value() && seen.insert(name).second);
            const auto signature = allowed.find(name);
            require(signature != allowed.end());
            propertyValue(child, signature->second, depth + 1);
        }
        require(actual != trackerClass("CenterOfMass") || seen.contains("masses"));
        if (actual == media("VideoClip") && seen.contains("video_framecount"))
            require(seen.contains("video"));
        if (actual == media("ImageVideo"))
            require(seen.contains("paths") && seen.contains("path") && seen.contains("delta_t") &&
                    denseArraySize(xml, property(xml, value, "paths")) > 0);
        if (actual == trackerClass("PointMass") && seen.contains("number_formats"))
        {
            const auto formats = property(xml, value, "number_formats");
            require(denseArraySize(xml, formats) % 2 == 0);
            std::set<std::string_view> variables;
            const auto& cells = xml.node(formats).children;
            for (std::size_t cell = 0; cell + 1 < cells.size(); cell += 2)
            {
                const auto variable = text(cells[cell]);
                require((variable == "x" || variable == "y" || variable == "t" || variable == "frame" || variable == "step") &&
                        variables.insert(variable).second);
                require(decimalPattern(text(cells[cell + 1])));
            }
        }
        if (actual == "org.opensourcephysics.tools.Parameter")
            require(seen.contains("name") && seen.contains("function"));
        if (actual == "org.opensourcephysics.tools.UserFunction")
        {
            require(seen.contains("name") && seen.contains("expression") && seen.contains("variables") &&
                    seen.contains("parameter_names") && seen.contains("parameter_values"));
            const auto count = modelNamesSize(xml, property(xml, value, "parameter_names"));
            require(count == modelValuesSize(xml, property(xml, value, "parameter_values")));
            if (seen.contains("parameter_descriptions"))
                require(denseArraySize(xml, property(xml, value, "parameter_descriptions")) == count);
            // Both supported force evaluators pass five state values. A mismatched variable list
            // changes force evaluation; it is not harmless preserved metadata.
            require(modelNamesSize(xml, property(xml, value, "variables")) == 5);
        }
        if (actual == trackerClass("DynamicParticle") || actual == trackerClass("DynamicSystem"))
        {
            require(seen.contains("user_parameters") && seen.contains("initial_values") && seen.contains("main_functions"));
            const auto initial = property(xml, value, "initial_values");
            require(denseArraySize(xml, initial) == 5);
            const std::array<std::string_view, 5> particle{"t", "x", "y", "vx", "vy"};
            const std::array<std::string_view, 5> system{"t", "r_relative", "θ_relative", "vr_relative", "ω_relative"};
            const auto& names = actual == trackerClass("DynamicParticle") ? particle : system;
            const auto& cells = xml.node(initial).children;
            for (std::size_t index = 0; index < names.size(); ++index)
            {
                require(text(property(xml, objectOf(xml, cells[index]), "name")) == names[index]);
            }
            if (actual == trackerClass("DynamicParticle"))
                require(text(property(xml, value, "in_system")) == "true");
            else
                require(seen.contains("particles"));
        }
        if (actual == trackerClass("TapeMeasure"))
        {
            // Only the read-only fixed tape mode has edited-name application acceptance.
            require(seen.contains("attachments") && text(property(xml, value, "readonly")) == "true" &&
                    text(property(xml, value, "fixedtape")) == "true" && text(property(xml, value, "fixedlength")) == "true" &&
                    text(property(xml, value, "stickmode")) == "false");
            for (const auto* field : {"framedata", "worldlengths"})
            {
                const auto& items = xml.node(property(xml, value, field)).children;
                require(items.size() == 1 && xml.node(items.front()).name == "[0]");
            }
        }

        // Tracker synchronizes linked x axes while loading each panel. Until that behavior has
        // its own application acceptance case, admit only explicitly unlinked custom plots.
        if (actual == trackerClass("PlotTrackView"))
            require(text(property(xml, value, "linked")) == "false");
    }

    void propertyValue(NodeId value, std::string_view signature, unsigned depth)
    {
        const auto& node = xml.node(value);
        require(depth < 32 && ++entries <= 10000 && node.kind == osp::NodeKind::Property);
        const auto separator = signature.find(':');
        const auto type = signature.substr(0, separator);
        const auto expected = separator == std::string_view::npos ? std::string_view{} : signature.substr(separator + 1);
        require(node.typeName == type && node.name.has_value());
        if (type == "object")
        {
            // Track and coordinate serializers retain frame-array extent with typed null final cells.
            // This is geometry data, not a missing named object or permission for arbitrary nulls.
            if (node.children.empty() && (expected == trackerClass("Vector$FrameData") || expected == trackerClass("PointMass$FrameData") ||
                                          expected == media("ImageCoordSystem$FrameData")))
            {
                return;
            }
            object(objectOf(xml, value), expected, depth + 1);
        }
        else if (type == "collection")
        {
            require(node.className == expected);
            std::set<std::string> members;
            std::set<std::string> owners;
            for (const auto item : node.children)
            {
                if (node.name == "masses")
                {
                    propertyValue(item, "string", depth + 1);
                    require(members.insert(xml.node(item).content).second); // Java coalesces duplicate members; do not accept that loss.
                    continue;
                }
                const auto& className = xml.node(objectOf(xml, item)).className;
                if (node.name == "track_views")
                {
                    require(className == trackerClass("PlotTrackView"));
                    // The Java loader reuses one view per owner; later duplicates overwrite it.
                    require(owners.insert(std::string(text(property(xml, objectOf(xml, item), "track")))).second);
                }
                else
                {
                    require(node.name == "tracks" &&
                            (className == trackerClass("PointMass") || className == trackerClass("CoordAxes") ||
                             className == trackerClass("Protractor") || className == trackerClass("Vector") ||
                             className == trackerClass("CenterOfMass") || className == trackerClass("TapeMeasure") ||
                             className == trackerClass("DynamicParticle") || className == trackerClass("DynamicSystem")));
                }
                propertyValue(item, "object:" + className, depth + 1);
            }
            require(node.name != "masses" || !members.empty());
        }
        else if (type == "array")
        {
            require(node.className == expected && expected.starts_with('['));
            const auto element = expected.substr(1);
            std::set<std::string> seen;
            std::set<std::string> guests;
            std::set<std::string> modelNames;
            for (const auto item : node.children)
            {
                const auto& itemNode = xml.node(item);
                const auto name = itemNode.name.value_or(std::string{});
                require(itemNode.name.has_value() && seen.insert(name).second);
                if (name == "array" && (element == "D" || element == "I" || element == "[D"))
                {
                    require(node.children.front() == item && node.children.size() == 1);
                    propertyValue(item, "string", depth + 1); // Primitive packed values contain no track names.
                    continue;
                }
                const auto index = elementIndex(name);
                // A sparse original frame is not an entry count. Match the writer's track-frame bound
                // without relaxing generic array indices or the aggregate schema-entry/depth limits.
                const bool trackFrames = node.name == "framedata" && (element == "L" + trackerClass("PointMass$FrameData;") ||
                                                                      element == "L" + trackerClass("Vector$FrameData;"));
                require(index.has_value());
                const auto position = index.value_or(0);
                require(position <= (trackFrames ? 500000U : 9999U));
                if (node.name == "attachments")
                {
                    require(position < 2);
                    if (itemNode.typeName == "object")
                    {
                        require(depth + 1 < 32 && itemNode.children.empty() && ++entries <= 10000);
                        continue; // Explicit empty endpoint; implicit gaps remain untouched too.
                    }
                }
                std::string childType;
                if (element.starts_with('['))
                    childType = "array:" + std::string(element);
                else if (element == "Ljava.lang.String;")
                    childType = "string";
                else if (element == "D" || element == "Ljava.lang.Double;")
                    childType = "double";
                else if (element.starts_with('L') && element.ends_with(';'))
                    childType = "object:" + std::string(element.substr(1, element.size() - 2));
                else if (element == "I")
                    childType = "int";
                else
                    require(false);
                propertyValue(item, childType, depth + 1);
                // Java ignores an already-present guest. The same guest in another panel is valid.
                require(node.name != "guests" || guests.insert(itemNode.content).second);
                if (element == "Lorg.opensourcephysics.tools.Parameter;" || element == "Lorg.opensourcephysics.tools.UserFunction;")
                {
                    const auto nameText = text(property(xml, objectOf(xml, item), "name"));
                    require(!nameText.empty() && modelNames.insert(std::string(nameText)).second);
                }
            }
            require(node.name != "particles" || seen.size() == 2);
            if (element == "Lorg.opensourcephysics.tools.Parameter;" || element == "Lorg.opensourcephysics.tools.UserFunction;")
            {
                require(!seen.empty());
                for (std::size_t i = 0; i < seen.size(); ++i)
                    require(seen.contains("[" + std::to_string(i) + "]"));
                require(node.name != "main_functions" || seen.size() == 2);
            }
        }
        else
        {
            require(osp::isTextType(node.type));
        }
    }
};

void patchString(SourceEdits& xml, NodeId value, const std::string& text)
{
    require(xml.source().node(value).type == PropertyType::String && text.find("]]>") == std::string::npos);
    xml.setString(value, text);
}

NodeId referenceProperty(const osp::Source& xml, const std::string& path)
{
    auto current = xml.root();
    std::size_t start = 0;
    while (start <= path.size())
    {
        const auto end = std::min(path.find('/', start), path.size());
        const auto part = std::string_view(path).substr(start, end - start);
        start = end + 1;
        if (part.empty())
        {
            continue;
        }
        const auto& node = xml.node(current);
        if (node.kind == osp::NodeKind::Property && node.type == PropertyType::Object)
        {
            current = objectOf(xml, current);
        }
        unsigned ordinal = 0;
        const auto* first = std::to_address(part.begin());
        const auto* last = std::to_address(part.end());
        const bool numeric = std::from_chars(first, last, ordinal).ptr == last;
        if (numeric && xml.node(current).type == PropertyType::Collection && xml.node(current).kind == osp::NodeKind::Property)
        {
            const auto& items = xml.node(current).children;
            require(ordinal < items.size());
            current = items[ordinal]; // Collection paths use source ordinals, not the serializer's repeated "item" labels.
        }
        else
        {
            current = property(xml, current, numeric ? "[" + std::string(part) + "]" : std::string(part));
        }
    }
    return current;
}

void requireName(const std::string& name)
{
    require(model::isTrackerTrackName(name));
}

std::vector<std::string> splitPacked(std::string text)
{
    std::ranges::replace(text, ';', ',');
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (;;)
    {
        const auto end = text.find(',', start);
        parts.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos)
            return parts;
        start = end + 1;
    }
}

std::string joinPacked(const std::vector<std::string>& packed)
{
    std::string result;
    for (std::size_t i = 0; i < 8; i += 2)
    {
        if (i > 0)
            result += ';';
        result += packed[i] + "," + packed[i + 1];
    }
    return result;
}

/// Index of a packed selection slot: "/selected_track_views/<chooser>/<plot|table>".
std::size_t packedSlot(const std::string& path)
{
    const auto rest = std::string_view(path).substr(std::string_view("/selected_track_views/").size());
    const auto slash = rest.find('/');
    require(slash != std::string_view::npos);
    std::size_t chooser = 0;
    const auto digits = rest.substr(0, slash);
    require(std::from_chars(digits.data(), digits.data() + digits.size(), chooser).ptr == digits.data() + digits.size());
    return chooser * 2 + (rest.substr(slash + 1) == "table" ? 1 : 0);
}

} // namespace

void validateTrackerEditSchema(const osp::Source& xml)
{
    RenameSchema{.xml = xml}.object(xml.root(), trackerClass("TrackerPanel"));
}

void validateTrackerMovieEditSchema(const osp::Source& xml)
{
    RenameSchema{.xml = xml, .movie = true}.object(xml.root(), trackerClass("TrackerPanel"));
}

void validateTrackerTrackMembership(const osp::Source& xml, const LegacyProject& project, const TrackerSource& source, bool movie)
{
    if (movie)
        validateTrackerMovieEditSchema(xml);
    else
        validateTrackerEditSchema(xml);
    const auto index = detail::indexTrackerReferences(xml, source); // The writer validated the source and bindings.
    require(index.malformedPaths.empty());
    std::set<std::string> originalNames;
    std::set<std::string> finalNames;
    std::set<TrackId> liveIds;
    std::map<std::size_t, const TrackerSourceTrack*> originalTracks;
    const auto collect = [&](const auto& tracks)
    {
        for (const auto& track : tracks)
        {
            requireName(track.name);
            require(finalNames.insert(track.name).second);
            liveIds.insert(track.id);
        }
    };
    collect(project.pointMasses);
    collect(project.vectors);
    for (const auto& track : index.tracks)
    {
        originalTracks.emplace(track.sourceIndex, &track);
        require(track.className == trackerClass("PointMass") || track.className == trackerClass("Vector") ||
                track.className == trackerClass("CoordAxes") || track.className == trackerClass("Protractor"));
        if (!track.originalName)
        {
            throw ProjectError("Tracker membership edits require every original track name.");
        }
        requireName(*track.originalName);
        require(originalNames.insert(*track.originalName).second);
        if (!track.nativeId)
            require(finalNames.insert(*track.originalName).second);
    }
    require(finalNames.size() <= 10000);
    for (const auto& reference : index.references)
    {
        // Even a previously dangling name can become a live relationship when a new track is added.
        require(reference.resolution == TrackerReferenceResolution::Resolved ||
                reference.resolution == TrackerReferenceResolution::NoTarget);
        require(reference.kind == TrackerReferenceKind::SelectedTrack || reference.kind == TrackerReferenceKind::PlotSelection ||
                reference.kind == TrackerReferenceKind::TableSelection || reference.kind == TrackerReferenceKind::TableColumnOwner ||
                reference.kind == TrackerReferenceKind::TableFormatOwner || reference.kind == TrackerReferenceKind::PlotOwner ||
                reference.kind == TrackerReferenceKind::PlotGuest);
        if (reference.resolution == TrackerReferenceResolution::Resolved)
        {
            require(reference.candidateSourceIndices.size() == 1);
            const auto target = originalTracks.find(reference.candidateSourceIndices.front());
            require(target != originalTracks.end());
            // Saved views have explicit cleanup policies. Dataset selections and scientific
            // dependencies must not silently disappear or rebind to a new same-name track.
            const auto targetId = target->second->nativeId;
            require(!targetId || liveIds.contains(*targetId) || reference.kind == TrackerReferenceKind::SelectedTrack ||
                    reference.kind == TrackerReferenceKind::PlotSelection || reference.kind == TrackerReferenceKind::TableSelection ||
                    reference.kind == TrackerReferenceKind::PlotOwner || reference.kind == TrackerReferenceKind::PlotGuest ||
                    reference.kind == TrackerReferenceKind::TableColumnOwner || reference.kind == TrackerReferenceKind::TableFormatOwner);
            if (reference.kind != TrackerReferenceKind::SelectedTrack)
                require(target->second->className != trackerClass("CoordAxes"));
        }
    }
}

void patchTrackerDeletedReferences(SourceEdits& edits, const LegacyProject& project, const TrackerSource& source)
{
    const auto& xml = edits.source();
    const auto index = detail::indexTrackerReferences(xml, source); // The writer validated the source and bindings.
    std::set<TrackId> liveIds;
    for (const auto& mass : project.pointMasses)
        liveIds.insert(mass.id);
    for (const auto& vector : project.vectors)
        liveIds.insert(vector.id);
    std::map<std::size_t, const TrackerSourceTrack*> originalTracks;
    for (const auto& track : index.tracks)
        originalTracks.emplace(track.sourceIndex, &track);
    // Tracker's chooser selects the first remaining viewable track in source order. Names
    // have already been patched; appended tracks follow the retained original objects.
    std::optional<std::string> fallback;
    const auto tracks = property(xml, xml.root(), "tracks");
    const auto& items = xml.node(tracks).children;
    for (std::size_t ordinal = 0; ordinal < items.size(); ++ordinal)
    {
        const auto found = originalTracks.find(ordinal);
        const auto originalId = found == originalTracks.end() ? std::optional<TrackId>{} : found->second->nativeId;
        if ((originalId && !liveIds.contains(*originalId)) || edits.removed(items[ordinal]))
            continue;
        const auto object = objectOf(xml, items[ordinal]);
        if (xml.node(object).className != trackerClass("CoordAxes"))
        {
            fallback = edits.text(property(xml, object, "name"));
            break;
        }
    }
    if (!fallback)
    {
        for (const auto& created : edits.insertions(tracks))
        {
            if (created.object && created.object->className != trackerClass("CoordAxes"))
            {
                const auto name =
                    std::ranges::find(created.object->properties, std::optional<std::string>("name"), &osp::PropertyValue::name);
                require(name != created.object->properties.end());
                fallback = name->content;
                break;
            }
        }
    }
    std::vector<std::string> packed;
    std::map<std::string, NodeId> removals;
    std::map<std::string, NodeId> changedParents;
    std::map<std::string, NodeId> changedTableViews;
    for (const auto& reference : index.references)
    {
        if (reference.resolution != TrackerReferenceResolution::Resolved)
            continue;
        require(reference.candidateSourceIndices.size() == 1);
        const auto target = originalTracks.find(reference.candidateSourceIndices.front());
        require(target != originalTracks.end());
        const auto targetId = target->second->nativeId;
        if (!targetId || liveIds.contains(*targetId))
            continue;
        if (reference.kind == TrackerReferenceKind::PlotOwner || reference.kind == TrackerReferenceKind::PlotGuest ||
            reference.kind == TrackerReferenceKind::TableColumnOwner || reference.kind == TrackerReferenceKind::TableFormatOwner)
        {
            const auto value = referenceProperty(xml, reference.path);
            require(edits.text(value) == reference.originalName);
            // Resolve source paths before any removal. Multiple format rows share one owner group;
            // remove that whole group once, retaining other owners' column/format order.
            auto removed = value;
            auto removedPath = reference.path;
            const auto up = [&](NodeId node)
            {
                const auto parent = xml.node(node).parent;
                require(parent.has_value());
                return *parent;
            };
            if (reference.kind == TrackerReferenceKind::PlotOwner)
            {
                removed = up(up(removed));
                removedPath.resize(removedPath.rfind('/'));
            }
            else if (reference.kind != TrackerReferenceKind::PlotGuest)
            {
                const int levels = reference.kind == TrackerReferenceKind::TableFormatOwner ? 2 : 1;
                for (int level = 0; level < levels; ++level)
                {
                    removed = up(removed);
                    removedPath.resize(removedPath.rfind('/'));
                }
            }
            const auto parentPath = removedPath.substr(0, removedPath.rfind('/'));
            changedParents.emplace(parentPath, up(removed));
            if (reference.kind == TrackerReferenceKind::TableColumnOwner || reference.kind == TrackerReferenceKind::TableFormatOwner)
                changedTableViews.emplace(parentPath.substr(0, parentPath.rfind('/')), up(up(removed)));
            removals.emplace(removedPath, removed);
            continue;
        }
        require(reference.kind == TrackerReferenceKind::SelectedTrack || reference.kind == TrackerReferenceKind::PlotSelection ||
                reference.kind == TrackerReferenceKind::TableSelection);
        if (reference.path.starts_with("/selected_track_views/"))
        {
            if (packed.empty())
                packed = splitPacked(edits.text(property(xml, xml.root(), "selected_track_views")));
            const auto slot = packedSlot(reference.path);
            require(packed.size() == 8 && slot < 8 && packed[slot] == reference.originalName);
            packed[slot] = fallback.value_or("null");
        }
        else
        {
            const auto value = referenceProperty(xml, reference.path);
            require(edits.text(value) == reference.originalName);
            if (reference.kind == TrackerReferenceKind::TableSelection && !fallback)
                changedTableViews.emplace(reference.path.substr(0, reference.path.rfind('/')), *xml.node(value).parent);
            if (reference.kind == TrackerReferenceKind::SelectedTrack || !fallback)
                edits.remove(value);
            else
                patchString(edits, value, *fallback);
        }
    }
    if (!packed.empty())
        patchString(edits, property(xml, xml.root(), "selected_track_views"), joinPacked(packed));
    for (const auto& [path, value] : removals)
    {
        const auto parent = xml.node(value).parent;
        require(parent && (xml.node(*parent).type == PropertyType::Array || xml.node(*parent).type == PropertyType::Collection));
        edits.remove(value);
    }
    for (const auto& [path, parent] : changedParents)
    {
        const auto& node = xml.node(parent);
        if (node.type != PropertyType::Array)
            continue;
        const auto remaining = edits.children(parent);
        if (remaining.empty())
        {
            edits.remove(parent); // Tracker omits empty guest/column/format lists.
            continue;
        }
        // Array semantics follow numeric indices, not source node order.
        std::map<std::uint32_t, NodeId> ordered;
        for (const auto item : remaining)
        {
            const auto position = elementIndex(xml.node(item).name.value_or(std::string{}));
            require(position && ordered.emplace(*position, item).second);
        }
        const bool sorted = std::ranges::equal(remaining, ordered | std::views::values);
        std::uint32_t next = 0;
        for (const auto& [originalIndex, item] : ordered)
        {
            const auto name = "[" + std::to_string(next++) + "]";
            if (sorted)
            {
                if (xml.node(item).name != name)
                    edits.rename(item, name);
                continue;
            }
            auto moved = edits.value(item);
            moved.name = name;
            edits.remove(item);
            edits.append(parent, std::move(moved));
        }
    }
    for (const auto& [path, view] : changedTableViews)
    {
        require(xml.node(view).className == trackerClass("TableTView"));
        for (const auto child : edits.children(view))
        {
            const auto& name = xml.node(child).name;
            if ((name == "track_columns" || name == "column_formats") && edits.children(child).empty() && !edits.hasInsertions(child))
                edits.remove(child);
        }
        if (!edits.children(view).empty())
            continue;
        // Tracker's TableTView loader passes its missing selection to getTrackByName,
        // which dereferences null. Its own saver omits a view with no custom state.
        // Remove only this emptied slot and empty enclosing arrays; view indices are roles.
        const auto slot = xml.node(view).parent;
        require(slot.has_value());
        const auto chooser = xml.node(*slot).parent;
        require(chooser && xml.node(*chooser).type == PropertyType::Array);
        const auto views = xml.node(*chooser).parent;
        require(views && xml.node(*views).name == "views");
        edits.remove(*slot);
        if (edits.children(*chooser).empty())
            edits.remove(*chooser);
        if (edits.children(*views).empty())
            edits.remove(*views);
    }
}

bool patchTrackerNames(SourceEdits& edits, const LegacyProject& project, LegacyProject& baseline, const TrackerSource& source, bool movie)
{
    const auto& xml = edits.source();
    std::map<TrackId, std::string> renamed;
    std::set<TrackId> liveIds;
    const auto collect = [&](const auto& current, const auto& original)
    {
        require(current.size() == original.size());
        for (std::size_t i = 0; i < current.size(); ++i)
        {
            liveIds.insert(current[i].id);
            require(current[i].id == original[i].id);
            if (current[i].name != original[i].name)
                renamed.emplace(current[i].id, current[i].name);
        }
    };
    collect(project.pointMasses, baseline.pointMasses);
    collect(project.vectors, baseline.vectors);
    if (renamed.empty())
        return false;
    if (movie)
        validateTrackerMovieEditSchema(xml);
    else
        validateTrackerEditSchema(xml);
    const auto index = detail::indexTrackerReferences(xml, source); // The writer validated the source and bindings.
    require(index.malformedPaths.empty());
    std::map<std::size_t, std::string> replacements;
    std::set<std::string> oldNames;
    std::set<std::string> newNames;
    for (const auto& track : index.tracks)
    {
        if (!track.originalName)
        {
            throw ProjectError("Tracker rename requires every original track name.");
        }
        // Original labels use the same packed codec: "null" cannot distinguish a track from no selection.
        requireName(*track.originalName);
        require(oldNames.insert(*track.originalName).second);
        if (track.nativeId && !liveIds.contains(*track.nativeId))
            continue; // Deleted selection references are cleaned up after live-name patches.
        const auto found = track.nativeId ? renamed.find(*track.nativeId) : renamed.end();
        const auto& name = found == renamed.end() ? *track.originalName : found->second;
        require(newNames.insert(name).second);
        if (found != renamed.end())
        {
            requireName(name);
            replacements.emplace(track.sourceIndex, name);
        }
    }
    require(replacements.size() == renamed.size());
    bool packedChanged = false;
    std::set<std::size_t> systemMembers;
    std::vector<std::string> packed;
    for (const auto& reference : index.references)
    {
        require(reference.resolution == TrackerReferenceResolution::Resolved ||
                reference.resolution == TrackerReferenceResolution::NoTarget);
        require(reference.kind == TrackerReferenceKind::SelectedTrack || reference.kind == TrackerReferenceKind::PlotSelection ||
                reference.kind == TrackerReferenceKind::TableSelection || reference.kind == TrackerReferenceKind::TableColumnOwner ||
                reference.kind == TrackerReferenceKind::TableFormatOwner || reference.kind == TrackerReferenceKind::PlotOwner ||
                reference.kind == TrackerReferenceKind::PlotGuest || reference.kind == TrackerReferenceKind::CenterOfMassMember ||
                reference.kind == TrackerReferenceKind::Attachment || reference.kind == TrackerReferenceKind::ModelBooster ||
                reference.kind == TrackerReferenceKind::DynamicSystemMember);
        if (reference.resolution == TrackerReferenceResolution::NoTarget)
            continue;
        require(reference.candidateSourceIndices.size() == 1);
        if (reference.kind == TrackerReferenceKind::DynamicSystemMember)
            require(systemMembers.insert(reference.candidateSourceIndices.front()).second); // One system per particle.
        if (reference.kind == TrackerReferenceKind::Attachment)
        {
            const auto target = std::ranges::find(index.tracks, reference.candidateSourceIndices.front(), &TrackerSourceTrack::sourceIndex);
            require(target != index.tracks.end() && target->className == trackerClass("PointMass"));
        }
        if (reference.kind == TrackerReferenceKind::PlotSelection || reference.kind == TrackerReferenceKind::TableSelection ||
            reference.kind == TrackerReferenceKind::PlotOwner || reference.kind == TrackerReferenceKind::PlotGuest ||
            reference.kind == TrackerReferenceKind::TableColumnOwner || reference.kind == TrackerReferenceKind::TableFormatOwner)
        {
            const auto target = std::ranges::find(index.tracks, reference.candidateSourceIndices.front(), &TrackerSourceTrack::sourceIndex);
            // TrackChooserTView ignores CoordAxes even when the named source object exists.
            require(target != index.tracks.end() && target->className != trackerClass("CoordAxes"));
        }
        const auto replacement = replacements.find(reference.candidateSourceIndices.front());
        if (replacement == replacements.end())
            continue;
        if (reference.path.starts_with("/selected_track_views/"))
        {
            if (packed.empty())
                packed = splitPacked(edits.text(property(xml, xml.root(), "selected_track_views")));
            const auto slot = packedSlot(reference.path);
            require(packed.size() == 8 && slot < 8 && packed[slot] == reference.originalName);
            packed[slot] = replacement->second;
            packedChanged = true;
        }
        else
        {
            const auto value = referenceProperty(xml, reference.path);
            require(edits.text(value) == reference.originalName);
            patchString(edits, value, replacement->second);
        }
    }
    for (const auto& track : index.tracks)
        if (track.className == trackerClass("DynamicParticle"))
            require(systemMembers.contains(track.sourceIndex));
    if (packedChanged)
        patchString(edits, property(xml, xml.root(), "selected_track_views"), joinPacked(packed));
    const auto tracks = property(xml, xml.root(), "tracks");
    const auto& items = xml.node(tracks).children;
    for (std::size_t ordinal = 0; ordinal < items.size(); ++ordinal)
    {
        if (const auto found = replacements.find(ordinal); found != replacements.end())
            patchString(edits, property(xml, objectOf(xml, items[ordinal]), "name"), found->second);
    }
    const auto update = [&](auto& tracks)
    {
        for (auto& track : tracks)
            if (const auto found = renamed.find(track.id); found != renamed.end())
                track.name = found->second;
    };
    update(baseline.pointMasses);
    update(baseline.vectors);
    return true;
}
} // namespace motrack
