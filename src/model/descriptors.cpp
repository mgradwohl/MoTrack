#include "model/descriptors.hpp"

#include <array>
#include <charconv>
#include <map>
#include <ranges>
#include <string>
#include <utility>

namespace motrack::model
{
namespace
{

using osp::PropertyType;
using enum Facet;

constexpr std::size_t MAX_NAME_UNITS = 4096;
constexpr std::string_view TRACKER_PACKAGE = "org.opensourcephysics.cabrillo.tracker.";

// Saver order of each family's typed fields (TrackerPanel, VideoClip, ImageVideo, ClipControl, ImageCoordSystem,
// PointMass and Vector loaders of Tracker 6.3.5), with the untyped properties between them omitted.
constexpr std::array PANEL_FIELDS{
    FieldDescriptor{.id = "semantic_version", .property = "semantic_version", .type = PropertyType::String},
    FieldDescriptor{.id = "description", .property = "description", .type = PropertyType::String},
    FieldDescriptor{.id = "time_unit", .property = "time_unit", .type = PropertyType::String, .commandEdits = true, .loaderDefault = "s"},
    FieldDescriptor{
        .id = "length_unit", .property = "length_unit", .type = PropertyType::String, .commandEdits = true, .loaderDefault = "m"},
    FieldDescriptor{.id = "mass_unit", .property = "mass_unit", .type = PropertyType::String, .commandEdits = true, .loaderDefault = "kg"},
    FieldDescriptor{.id = "referenceframe", .property = "referenceframe", .type = PropertyType::String},
    FieldDescriptor{.id = "selectedtrack", .property = "selectedtrack", .type = PropertyType::String},
};
constexpr std::array CLIP_FIELDS{
    FieldDescriptor{.id = "video_framecount", .property = "video_framecount", .type = PropertyType::Int},
    FieldDescriptor{.id = "startframe",
                    .property = "startframe",
                    .type = PropertyType::Int,
                    .commandEdits = true,
                    .facet = Definition,
                    .loaderDefault = "0"},
    FieldDescriptor{.id = "stepsize",
                    .property = "stepsize",
                    .type = PropertyType::Int,
                    .commandEdits = true,
                    .facet = Definition,
                    .loaderDefault = "1"},
    FieldDescriptor{.id = "stepcount", .property = "stepcount", .type = PropertyType::Int, .commandEdits = true, .facet = Definition},
    FieldDescriptor{.id = "starttime",
                    .property = "starttime",
                    .type = PropertyType::Double,
                    .commandEdits = true,
                    .facet = Definition,
                    .loaderDefault = "0.0"},
    FieldDescriptor{.id = "frameshift", .property = "frameshift", .type = PropertyType::Int, .loaderDefault = "0"},
};
constexpr std::array VIDEO_FIELDS{
    FieldDescriptor{.id = "paths", .property = "paths", .type = PropertyType::Array},
    FieldDescriptor{.id = "path", .property = "path", .type = PropertyType::String},
    FieldDescriptor{.id = "delta_t", .property = "delta_t", .type = PropertyType::Double},
};
constexpr std::array CLIP_CONTROL_FIELDS{
    FieldDescriptor{.id = "rate", .property = "rate", .type = PropertyType::Double},
    FieldDescriptor{.id = "delta_t", .property = "delta_t", .type = PropertyType::Double, .commandEdits = true, .facet = Definition},
    FieldDescriptor{.id = "frame", .property = "frame", .type = PropertyType::Int},
    FieldDescriptor{.id = "looping", .property = "looping", .type = PropertyType::Boolean},
};
constexpr std::array COORDINATES_FIELDS{
    FieldDescriptor{.id = "fixedorigin", .property = "fixedorigin", .type = PropertyType::Boolean, .loaderDefault = "false"},
    FieldDescriptor{.id = "fixedangle", .property = "fixedangle", .type = PropertyType::Boolean, .loaderDefault = "false"},
    FieldDescriptor{.id = "fixedscale", .property = "fixedscale", .type = PropertyType::Boolean, .loaderDefault = "false"},
    FieldDescriptor{.id = "locked", .property = "locked", .type = PropertyType::Boolean, .loaderDefault = "false"},
    FieldDescriptor{.id = "framedata",
                    .property = "framedata",
                    .type = PropertyType::Array,
                    .kind = FieldKind::FrameKeyed,
                    .commandEdits = true,
                    .facet = Geometry},
};
constexpr std::array POINT_MASS_FIELDS{
    FieldDescriptor{
        .id = "mass", .property = "mass", .type = PropertyType::Double, .commandEdits = true, .facet = Definition, .loaderDefault = "1.0"},
    FieldDescriptor{.id = "name", .property = "name", .type = PropertyType::String, .commandEdits = true, .label = true},
    FieldDescriptor{.id = "framedata",
                    .property = "framedata",
                    .type = PropertyType::Array,
                    .kind = FieldKind::FrameKeyed,
                    .commandEdits = true,
                    .facet = Geometry},
    // Mark commands keep the key frames with the marks they change.
    FieldDescriptor{.id = "keyFrames", .property = "keyFrames", .type = PropertyType::Array, .commandEdits = true, .facet = Geometry},
};
constexpr std::array VECTOR_FIELDS{
    FieldDescriptor{.id = "name", .property = "name", .type = PropertyType::String, .commandEdits = true, .label = true},
    FieldDescriptor{.id = "framedata",
                    .property = "framedata",
                    .type = PropertyType::Array,
                    .kind = FieldKind::FrameKeyed,
                    .commandEdits = true,
                    .facet = Geometry},
};

template <typename T> FieldValue fromRead(const Read<T>& value)
{
    if (!value)
    {
        return value.error();
    }
    return FieldValue(*value);
}

/// UTF-16 code units of UTF-8 text, as Java's String.length() counts them.
std::size_t utf16Units(std::string_view text)
{
    std::size_t units = 0;
    for (const auto byte : text)
    {
        const auto value = static_cast<unsigned char>(byte);
        if ((value & 0xC0U) != 0x80U)
        {
            units += value >= 0xF0U ? 2 : 1;
        }
    }
    return units;
}

/// The only property of this name among an object's children; nothing when absent, and `duplicate` when repeated.
struct NamedProperty
{
    std::optional<osp::NodeId> node;
    bool duplicate = false;
};

NamedProperty namedProperty(const osp::Source& source, osp::NodeId owner, std::string_view name)
{
    NamedProperty result;
    for (const auto child : source.node(owner).children)
    {
        const auto& node = source.node(child);
        if (node.kind == osp::NodeKind::Property && node.name == name)
        {
            if (result.node)
            {
                result.duplicate = true;
            }
            else
            {
                result.node = child;
            }
        }
    }
    return result;
}

/// A string property's value of at most 4096 UTF-16 units, as the source reference index reads names.
std::optional<std::string> boundedString(const osp::Source& source, osp::NodeId property)
{
    if (source.node(property).type != PropertyType::String)
    {
        return std::nullopt;
    }
    auto text = osp::PropertyView(source, property).stringValue();
    if (!text || utf16Units(*text) > MAX_NAME_UNITS)
    {
        return std::nullopt;
    }
    return std::move(*text);
}

/// The canonical decimal index of an "[n]" element name.
std::optional<std::uint32_t> elementIndex(const std::optional<std::string>& label, std::uint32_t bound)
{
    if (!label || label->size() <= 2 || label->front() != '[' || label->back() != ']')
    {
        return std::nullopt;
    }
    const auto* begin = label->c_str() + 1;
    const auto* end = label->c_str() + label->size() - 1;
    std::uint32_t value = 0;
    const auto [last, error] = std::from_chars(begin, end, value);
    if (error != std::errc{} || last != end || std::to_string(value) != std::string_view(begin, end) || value >= bound)
    {
        return std::nullopt;
    }
    return value;
}

SavedReferences malformedReferences()
{
    return {.names = {}, .malformed = true};
}

std::optional<SavedReferences> readArray(const osp::Source& source, osp::NodeId property, const ReferenceDescriptor& field)
{
    const auto& node = source.node(property);
    if (node.type != PropertyType::Array || node.className != "[Ljava.lang.String;")
    {
        return malformedReferences();
    }
    std::map<std::uint32_t, osp::NodeId> elements;
    for (const auto child : node.children)
    {
        const auto index = elementIndex(source.node(child).name, field.maxSlots);
        if (!index || !elements.emplace(*index, child).second)
        {
            return malformedReferences();
        }
    }
    if (field.contiguous && !elements.empty() && (elements.begin()->first != 0 || elements.rbegin()->first != elements.size() - 1))
    {
        return malformedReferences();
    }
    SavedReferences result;
    result.names.resize(elements.empty() ? 0 : elements.rbegin()->first + 1);
    for (const auto& [index, element] : elements)
    {
        const auto& item = source.node(element);
        if (!field.contiguous && item.type == PropertyType::Object && item.children.empty())
        {
            continue; // A typed null element is an explicitly empty slot.
        }
        auto name = boundedString(source, element);
        if (!name)
        {
            return malformedReferences();
        }
        result.names[index] = std::move(name);
    }
    return result;
}

std::optional<SavedReferences> readCollection(const osp::Source& source, const NamedProperty& property)
{
    // CenterOfMass and VectorSum always save their member list; an absent or repeated one is not read as Tracker does.
    if (!property.node || property.duplicate)
    {
        return malformedReferences();
    }
    const auto& node = source.node(*property.node);
    if (node.type != PropertyType::Collection || node.className != "java.util.ArrayList")
    {
        return malformedReferences();
    }
    SavedReferences result;
    for (const auto item : node.children)
    {
        auto name = boundedString(source, item);
        if (!name)
        {
            return malformedReferences();
        }
        result.names.push_back(std::move(name));
    }
    return result;
}

std::vector<std::string> classes(std::initializer_list<std::string_view> names)
{
    std::vector<std::string> result;
    for (const auto name : names)
    {
        result.push_back(std::string(TRACKER_PACKAGE) + std::string(name));
    }
    return result;
}

} // namespace

std::span<const FieldDescriptor> fieldDescriptors(Family family) noexcept
{
    switch (family)
    {
    case Family::Panel:
        return PANEL_FIELDS;
    case Family::Clip:
        return CLIP_FIELDS;
    case Family::Video:
        return VIDEO_FIELDS;
    case Family::ClipControl:
        return CLIP_CONTROL_FIELDS;
    case Family::Coordinates:
        return COORDINATES_FIELDS;
    case Family::PointMass:
        return POINT_MASS_FIELDS;
    case Family::Vector:
        return VECTOR_FIELDS;
    case Family::Unknown:
        break;
    }
    return {};
}

const FieldDescriptor* findField(Family family, std::string_view id) noexcept
{
    const auto fields = fieldDescriptors(family);
    const auto found = std::ranges::find(fields, id, &FieldDescriptor::id);
    return found == fields.end() ? nullptr : &*found;
}

FieldValue scalarValue(const TypedValue& value, const FieldDescriptor& field)
{
    const auto id = field.id;
    if (field.kind == FieldKind::FrameKeyed)
    {
        return std::monostate{};
    }
    return std::visit(
        [id](const auto& state) -> FieldValue
        {
            using State = std::decay_t<decltype(state)>;
            if constexpr (std::is_same_v<State, PanelState>)
            {
                const std::array<std::pair<std::string_view, const Read<std::string>*>, 7> fields{
                    {{"semantic_version", &state.semanticVersion},
                     {"description", &state.description},
                     {"time_unit", &state.timeUnit},
                     {"length_unit", &state.lengthUnit},
                     {"mass_unit", &state.massUnit},
                     {"referenceframe", &state.referenceFrame},
                     {"selectedtrack", &state.selectedTrack}}};
                const auto found = std::ranges::find(fields, id, &std::pair<std::string_view, const Read<std::string>*>::first);
                return found == fields.end() ? FieldValue{} : fromRead(*found->second);
            }
            else if constexpr (std::is_same_v<State, ClipState>)
            {
                if (id == "starttime")
                {
                    return fromRead(state.startTime);
                }
                const std::array<std::pair<std::string_view, const Read<std::int32_t>*>, 5> fields{{{"video_framecount", &state.frameCount},
                                                                                                    {"startframe", &state.startFrame},
                                                                                                    {"stepsize", &state.stepSize},
                                                                                                    {"stepcount", &state.stepCount},
                                                                                                    {"frameshift", &state.frameShift}}};
                const auto found = std::ranges::find(fields, id, &std::pair<std::string_view, const Read<std::int32_t>*>::first);
                return found == fields.end() ? FieldValue{} : fromRead(*found->second);
            }
            else if constexpr (std::is_same_v<State, VideoState>)
            {
                if (id == "paths")
                {
                    return state.paths ? FieldValue(*state.paths) : FieldValue(osp::DecodeError::Missing);
                }
                return id == "path" ? fromRead(state.path) : id == "delta_t" ? fromRead(state.deltaT) : FieldValue{};
            }
            else if constexpr (std::is_same_v<State, ClipControlState>)
            {
                if (id == "frame")
                {
                    return fromRead(state.frame);
                }
                if (id == "looping")
                {
                    return fromRead(state.looping);
                }
                return id == "rate" ? fromRead(state.rate) : id == "delta_t" ? fromRead(state.deltaT) : FieldValue{};
            }
            else if constexpr (std::is_same_v<State, CoordinatesState>)
            {
                const std::array<std::pair<std::string_view, const Read<bool>*>, 4> fields{{{"fixedorigin", &state.fixedOrigin},
                                                                                            {"fixedangle", &state.fixedAngle},
                                                                                            {"fixedscale", &state.fixedScale},
                                                                                            {"locked", &state.locked}}};
                const auto found = std::ranges::find(fields, id, &std::pair<std::string_view, const Read<bool>*>::first);
                return found == fields.end() ? FieldValue{} : fromRead(*found->second);
            }
            else if constexpr (std::is_same_v<State, PointMassState>)
            {
                if (id == "keyFrames")
                {
                    return fromRead(state.keyFrames);
                }
                return id == "mass" ? fromRead(state.mass) : id == "name" ? fromRead(state.name) : FieldValue{};
            }
            else if constexpr (std::is_same_v<State, VectorState>)
            {
                return id == "name" ? fromRead(state.name) : FieldValue{};
            }
            else
            {
                return FieldValue{};
            }
        },
        value);
}

std::span<const ReferenceDescriptor> referenceDescriptors()
{
    static const std::array<std::string_view, 1> centerOwners{"org.opensourcephysics.cabrillo.tracker.CenterOfMass"};
    static const std::array<std::string_view, 1> sumOwners{"org.opensourcephysics.cabrillo.tracker.VectorSum"};
    static const std::array<std::string_view, 3> toolOwners{"org.opensourcephysics.cabrillo.tracker.TapeMeasure",
                                                            "org.opensourcephysics.cabrillo.tracker.Protractor",
                                                            "org.opensourcephysics.cabrillo.tracker.CircleFitter"};
    static const std::array<std::string_view, 1> circleOwners{"org.opensourcephysics.cabrillo.tracker.CircleFitter"};
    static const std::array<std::string_view, 3> modelOwners{"org.opensourcephysics.cabrillo.tracker.DynamicParticle",
                                                             "org.opensourcephysics.cabrillo.tracker.DynamicParticlePolar",
                                                             "org.opensourcephysics.cabrillo.tracker.DynamicSystem"};
    static const std::array<std::string_view, 1> systemOwners{"org.opensourcephysics.cabrillo.tracker.DynamicSystem"};
    static const std::array<std::string_view, 1> panelOwners{tracker::TRACKER_PANEL_CLASS};
    // The source reference index's type rules (src/tracker_references.cpp; tests/model/name_binding_test.cpp).
    static const std::array<ReferenceDescriptor, 7> descriptors{
        ReferenceDescriptor{.id = "center-of-mass.masses",
                            .ownerClasses = centerOwners,
                            .property = "masses",
                            .shape = SlotShape::Collection,
                            .kind = DependencyKind::MemberOf,
                            .filter = {.eligibleClasses = classes({"PointMass"}), .excludedClasses = classes({"Vector"})}},
        ReferenceDescriptor{.id = "vector-sum.vectors",
                            .ownerClasses = sumOwners,
                            .property = "vectors",
                            .shape = SlotShape::Collection,
                            .kind = DependencyKind::MemberOf,
                            .filter = {.eligibleClasses = classes({"Vector"}), .excludedClasses = classes({"PointMass"})}},
        ReferenceDescriptor{.id = "tool.attachments",
                            .ownerClasses = toolOwners,
                            .property = "attachments",
                            .shape = SlotShape::StringArray,
                            .kind = DependencyKind::AttachedTo,
                            .onMissing = MissingTarget::FallsBack},
        // The circle fitter's step attachment shares AttachedTo with its point attachments, in a separate slot range.
        ReferenceDescriptor{.id = "circle-fitter.step_attachment",
                            .ownerClasses = circleOwners,
                            .property = "step_attachment",
                            .kind = DependencyKind::AttachedTo,
                            .onMissing = MissingTarget::FallsBack,
                            .slotBase = 0x8000'0000U},
        ReferenceDescriptor{.id = "model.booster",
                            .ownerClasses = modelOwners,
                            .property = "booster",
                            .kind = DependencyKind::BoosterOf,
                            .targetFacet = Data,
                            .onMissing = MissingTarget::FallsBack,
                            .filter = {.eligibleClasses = classes({"PointMass"}), .excludedClasses = classes({"Vector"})}},
        ReferenceDescriptor{.id = "dynamic-system.particles",
                            .ownerClasses = systemOwners,
                            .property = "particles",
                            .shape = SlotShape::StringArray,
                            .maxSlots = 2,
                            .contiguous = true,
                            .kind = DependencyKind::MemberOf,
                            .targetFacet = Definition,
                            .filter = {.eligibleClasses = classes({"DynamicParticle", "DynamicParticlePolar"}),
                                       .excludedClasses = classes({"PointMass", "Vector"})}},
        // A missing origin silently gives Tracker's default frame.
        ReferenceDescriptor{.id = "panel.referenceframe",
                            .ownerClasses = panelOwners,
                            .property = "referenceframe",
                            .kind = DependencyKind::FrameOf,
                            .onMissing = MissingTarget::FallsBack},
    };
    return descriptors;
}

std::optional<SavedReferences> readReferences(const osp::Source& source, osp::NodeId owner, const ReferenceDescriptor& field)
{
    const auto property = namedProperty(source, owner, field.property);
    if (field.shape == SlotShape::Collection)
    {
        return readCollection(source, property);
    }
    if (!property.node)
    {
        return std::nullopt;
    }
    if (property.duplicate)
    {
        return malformedReferences();
    }
    if (field.shape == SlotShape::StringArray)
    {
        return readArray(source, *property.node, field);
    }
    auto name = boundedString(source, *property.node);
    if (!name)
    {
        return malformedReferences();
    }
    return SavedReferences{.names = {std::move(name)}, .malformed = false};
}

std::optional<std::string> savedLabel(const osp::Source& source, osp::NodeId object)
{
    const auto property = namedProperty(source, object, "name");
    if (!property.node || property.duplicate)
    {
        return std::nullopt;
    }
    return boundedString(source, *property.node);
}

namespace
{

template <typename T, typename Properties>
osp::PropertyValue frameArray(std::string_view frameClass, const FrameSeries<T>& series, Properties properties)
{
    osp::PropertyValue array{.name = "framedata", .typeName = "array", .className = "[L" + std::string(frameClass) + ";"};
    series.forEach(
        [&](std::int32_t frame, const FrameElement<T>& element)
        {
            if (frame >= 0)
            {
                array.elements.push_back(
                    osp::objectProperty("[" + std::to_string(frame) + "]",
                                        osp::ObjectValue{.className = std::string(frameClass), .properties = properties(element.value)}));
            }
        });
    return array;
}

void appendScalar(std::vector<osp::PropertyValue>& properties, const FieldDescriptor& field, const FieldValue& value)
{
    const auto name = std::string(field.property);
    std::visit(
        [&](const auto& item)
        {
            using Item = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<Item, bool>)
            {
                properties.push_back(osp::booleanProperty(name, item));
            }
            else if constexpr (std::is_same_v<Item, std::int32_t>)
            {
                properties.push_back(osp::intProperty(name, item));
            }
            else if constexpr (std::is_same_v<Item, double>)
            {
                properties.push_back(osp::doubleProperty(name, item));
            }
            else if constexpr (std::is_same_v<Item, std::string>)
            {
                properties.push_back(osp::stringProperty(name, item));
            }
            else if constexpr (std::is_same_v<Item, std::vector<std::string>>)
            {
                std::vector<std::optional<osp::PropertyValue>> elements;
                elements.reserve(item.size());
                for (const auto& text : item)
                {
                    elements.emplace_back(osp::stringProperty("item", text));
                }
                properties.push_back(osp::arrayProperty(name, "[Ljava.lang.String;", elements));
            }
            else if constexpr (std::is_same_v<Item, std::vector<std::int32_t>>)
            {
                auto array = osp::primitiveArrayProperty(name, "[I", osp::CompactArray{.kind = osp::CompactArray::Kind::Int, .ints = item});
                if (array)
                {
                    properties.push_back(std::move(*array));
                }
            }
        },
        value);
}

} // namespace

std::vector<osp::PropertyValue> saverProperties(const TypedValue& value)
{
    std::vector<osp::PropertyValue> properties;
    if (const auto* mass = std::get_if<PointMassState>(&value))
    {
        properties.push_back(osp::doubleProperty("mass", loadedMass(*mass)));
        properties.push_back(osp::stringProperty("name", loadedName(mass->name)));
        properties.push_back(frameArray(tracker::POINT_MASS_FRAME_CLASS,
                                        mass->marks,
                                        [](const Mark& mark)
                                        { return std::vector{osp::doubleProperty("x", mark.x), osp::doubleProperty("y", mark.y)}; }));
        appendScalar(properties, POINT_MASS_FIELDS[3], loadedKeyFrames(*mass));
        return properties;
    }
    if (const auto* vector = std::get_if<VectorState>(&value))
    {
        properties.push_back(osp::stringProperty("name", loadedName(vector->name)));
        properties.push_back(frameArray(tracker::VECTOR_FRAME_CLASS,
                                        vector->samples,
                                        [](const VectorSample& sample)
                                        {
                                            return std::vector{osp::doubleProperty("xtail", sample.xTail),
                                                               osp::doubleProperty("ytail", sample.yTail),
                                                               osp::doubleProperty("xcomponent", sample.xComponent),
                                                               osp::doubleProperty("ycomponent", sample.yComponent)};
                                        }));
        return properties;
    }
    for (const auto& field : fieldDescriptors(familyOf(value)))
    {
        if (field.kind == FieldKind::Scalar)
        {
            appendScalar(properties, field, scalarValue(value, field));
        }
        else if (const auto* coords = std::get_if<CoordinatesState>(&value))
        {
            properties.push_back(frameArray(tracker::COORD_FRAME_CLASS,
                                            coords->keys,
                                            [](const CoordKey& key)
                                            {
                                                return std::vector{osp::doubleProperty("xorigin", key.xOrigin),
                                                                   osp::doubleProperty("yorigin", key.yOrigin),
                                                                   osp::doubleProperty("angle", key.angle),
                                                                   osp::doubleProperty("xscale", key.xScale),
                                                                   osp::doubleProperty("yscale", key.yScale)};
                                            }));
        }
    }
    return properties;
}

} // namespace motrack::model
