#include "tracker_references.hpp"

#include "tracker_import.hpp"

#include <algorithm>
#include <charconv>
#include <map>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace motrack
{
namespace
{

constexpr std::size_t MAX_SOURCE_TRACKS = 10000;
constexpr std::size_t MAX_NAME_UNITS = 4096;
constexpr std::size_t MAX_VIEW_UNITS = 65536;
constexpr std::string_view POINT_MASS_CLASS = "org.opensourcephysics.cabrillo.tracker.PointMass";
constexpr std::string_view VECTOR_CLASS = "org.opensourcephysics.cabrillo.tracker.Vector";
constexpr std::string_view DYNAMIC_PARTICLE_CLASS = "org.opensourcephysics.cabrillo.tracker.DynamicParticle";
constexpr std::string_view POLAR_PARTICLE_CLASS = "org.opensourcephysics.cabrillo.tracker.DynamicParticlePolar";

using osp::NodeId;
using osp::PropertyType;

/// A named property of an object, with the duplicate flag XMLControl's last-wins lookup would hide.
struct Property
{
    std::optional<NodeId> node;
    bool duplicate = false;
};

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

std::vector<std::string_view> split(std::string_view text, char separator)
{
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    for (;;)
    {
        const auto end = text.find(separator, start);
        parts.push_back(text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        if (end == std::string_view::npos)
        {
            return parts;
        }
        start = end + 1;
    }
}

/// Parses the canonical decimal text of an unsigned integer.
std::optional<std::uint64_t> canonicalUnsigned(std::string_view text)
{
    std::uint64_t value = 0;
    const auto* begin = std::to_address(text.begin());
    const auto* end = std::to_address(text.end());
    const auto [last, error] = std::from_chars(begin, end, value);
    if (text.empty() || error != std::errc{} || last != end || std::to_string(value) != text)
    {
        return std::nullopt;
    }
    return value;
}

struct Reader
{
    const osp::Source& xml;

    [[nodiscard]] const osp::Node& node(NodeId id) const
    {
        return xml.node(id);
    }

    /// The property named `name` among a node's children (an object's properties or a property's elements).
    [[nodiscard]] Property property(std::optional<NodeId> owner, std::string_view name) const
    {
        Property result;
        if (!owner)
        {
            return result;
        }
        for (const auto child : node(*owner).children)
        {
            if (node(child).kind == osp::NodeKind::Property && node(child).name == name)
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

    [[nodiscard]] std::optional<std::string> stringValue(const Property& value, std::size_t limit) const
    {
        if (value.duplicate || !value.node || node(*value.node).type != PropertyType::String)
        {
            return std::nullopt;
        }
        auto text = osp::PropertyView(xml, *value.node).stringValue();
        if (!text || utf16Units(*text) > limit)
        {
            return std::nullopt;
        }
        return std::move(*text);
    }

    /// The object of a non-null object property with a class.
    [[nodiscard]] std::optional<NodeId> viewObject(NodeId item) const
    {
        const auto& property = node(item);
        if (property.type != PropertyType::Object || property.children.empty() || node(property.children.front()).className.empty())
        {
            return std::nullopt;
        }
        return property.children.front();
    }
};

struct NameTargets
{
    std::unordered_map<std::string, std::vector<std::size_t>> byName;
    std::size_t remainingCandidates = 100000;
};

void addReference(TrackerReferenceIndex& result,
                  NameTargets& names,
                  TrackerReferenceKind kind,
                  std::string path,
                  const std::string& name,
                  bool packed,
                  std::string_view constituentClass = {})
{
    TrackerNameReference reference{.kind = kind,
                                   .path = std::move(path),
                                   .originalName = name,
                                   .resolution = TrackerReferenceResolution::Missing,
                                   .candidateSourceIndices = {}};
    if (packed && name == "null")
    {
        reference.resolution = TrackerReferenceResolution::NoTarget;
    }
    else if (const auto found = names.byName.find(reference.originalName); found != names.byName.end())
    {
        // Entry caps alone permit tracks * owners candidate copies for duplicate names.
        // Charge before filtering/copying, including wrong-type candidates and panel references.
        if (found->second.size() > names.remainingCandidates)
        {
            throw ProjectError("Too many Tracker reference candidates to index.");
        }
        names.remainingCandidates -= found->second.size();
        bool unclassified = false;
        for (const auto sourceIndex : found->second)
        {
            const auto& className = result.tracks[sourceIndex].className;
            const bool knownPolarMember = constituentClass == DYNAMIC_PARTICLE_CLASS && className == POLAR_PARTICLE_CLASS;
            if (!constituentClass.empty() && className != constituentClass && !knownPolarMember)
            {
                if (className == POINT_MASS_CLASS || className == VECTOR_CLASS)
                {
                    continue; // The two exact base types cannot be constituents of each other's family.
                }
                unclassified = true; // Do not guess subclass eligibility, including nested dependent tracks.
            }
            reference.candidateSourceIndices.push_back(sourceIndex);
        }
        if (unclassified)
        {
            reference.resolution = TrackerReferenceResolution::Unclassified;
        }
        else if (!reference.candidateSourceIndices.empty())
        {
            reference.resolution =
                reference.candidateSourceIndices.size() == 1 ? TrackerReferenceResolution::Resolved : TrackerReferenceResolution::Ambiguous;
        }
    }
    result.references.push_back(std::move(reference));
}

struct ReferenceReader
{
    TrackerReferenceIndex& result;
    NameTargets& names;
    Reader xml;
    std::size_t entries = 0;

    using Array = std::map<unsigned, NodeId>;
    using Owners = std::vector<std::pair<std::string, std::string>>;

    enum class RowKind : std::uint8_t
    {
        Columns,
        Formats,
        Datasets
    };

    void countEntry()
    {
        if (++entries > 10000)
        {
            throw ProjectError("Too many nested Tracker reference entries to index.");
        }
    }

    void panelReference(std::optional<NodeId> owner,
                        std::string_view field,
                        TrackerReferenceKind kind,
                        const std::string& prefix = {},
                        bool required = false,
                        std::string_view constituentClass = {})
    {
        const auto value = xml.property(owner, field);
        if (!value.node && !required)
        {
            return;
        }
        const auto name = xml.stringValue(value, MAX_NAME_UNITS);
        auto path = prefix + "/" + std::string(field);
        if (!name)
        {
            result.malformedPaths.push_back(std::move(path));
            return;
        }
        addReference(result, names, kind, std::move(path), *name, false, constituentClass);
    }

    void viewReferences(NodeId panel)
    {
        const auto propertyValue = xml.property(panel, "selected_track_views");
        if (!propertyValue.node)
        {
            return;
        }
        const auto value = xml.stringValue(propertyValue, MAX_VIEW_UNITS);
        const auto malformed = [&]()
        {
            result.malformedPaths.emplace_back("/selected_track_views");
        };
        if (!value)
        {
            malformed();
            return;
        }
        const auto choosers = split(*value, ';');
        if (choosers.size() != 4)
        {
            malformed();
            return;
        }
        // Validate the entire packed value before emitting any references from it.
        std::vector<std::vector<std::string_view>> pairs;
        for (const auto chooser : choosers)
        {
            auto pair = split(chooser, ',');
            if (pair.size() != 2 || pair[0].empty() || pair[1].empty() || utf16Units(pair[0]) > MAX_NAME_UNITS ||
                utf16Units(pair[1]) > MAX_NAME_UNITS)
            {
                malformed();
                return;
            }
            pairs.push_back(std::move(pair));
        }
        for (std::size_t index = 0; index < pairs.size(); ++index)
        {
            const auto path = "/selected_track_views/" + std::to_string(index);
            addReference(result, names, TrackerReferenceKind::PlotSelection, path + "/plot", std::string(pairs[index][0]), true);
            addReference(result, names, TrackerReferenceKind::TableSelection, path + "/table", std::string(pairs[index][1]), true);
        }
    }

    std::optional<Array> array(const Property& value, std::string_view className, const std::string& path, unsigned bound = 10000)
    {
        Array children;
        bool valid = !value.duplicate && value.node && xml.node(*value.node).type == PropertyType::Array &&
                     xml.node(*value.node).className == className;
        if (valid)
        {
            for (const auto child : xml.node(*value.node).children)
            {
                countEntry();
                const auto& label = xml.node(child).name;
                const auto index = label && label->size() > 2 && label->front() == '[' && label->back() == ']'
                                       ? canonicalUnsigned(std::string_view(*label).substr(1, label->size() - 2))
                                       : std::nullopt;
                if (!index || *index >= bound || !children.emplace(static_cast<unsigned>(*index), child).second)
                {
                    valid = false;
                    break;
                }
            }
        }
        if (!valid)
        {
            result.malformedPaths.push_back(path);
            return std::nullopt;
        }
        return children;
    }

    // All cells must be well-formed strings, but only the first cell is an owner reference.
    // Preserve column/format text as data, including text identical to another track's name.
    std::optional<Owners> rows(const Property& value, const std::string& path, RowKind kind)
    {
        const auto rows = array(value, "[[Ljava.lang.String;", path);
        if (!rows)
        {
            return std::nullopt;
        }
        Owners owners;
        if ((kind == RowKind::Formats && rows->empty()) ||
            (kind != RowKind::Columns && !rows->empty() && (rows->begin()->first != 0 || rows->rbegin()->first != rows->size() - 1)))
        {
            result.malformedPaths.push_back(path);
            return std::nullopt;
        }
        for (const auto& [rowIndex, row] : *rows)
        {
            const auto rowPath = path + "/" + std::to_string(rowIndex);
            const auto columns = array({.node = row}, "[Ljava.lang.String;", rowPath);
            if (!columns)
            {
                return std::nullopt;
            }
            if (columns->empty() || columns->begin()->first != 0 || columns->rbegin()->first != columns->size() - 1 ||
                (kind == RowKind::Formats && columns->size() != 3) || (kind == RowKind::Datasets && columns->size() != 2))
            {
                result.malformedPaths.push_back(rowPath);
                return std::nullopt;
            }
            for (const auto& [columnIndex, column] : *columns)
            {
                const auto text = xml.stringValue({.node = column}, MAX_NAME_UNITS);
                if (!text)
                {
                    result.malformedPaths.push_back(rowPath + "/" + std::to_string(columnIndex));
                    return std::nullopt;
                }
                if (columnIndex == 0)
                {
                    if (kind == RowKind::Formats && !owners.empty() && owners.front().second != *text)
                    {
                        result.malformedPaths.push_back(rowPath + "/0");
                        return std::nullopt;
                    }
                    owners.emplace_back(rowPath + "/0", *text);
                }
                else if (kind == RowKind::Datasets)
                {
                    // The serializer emits Integer.toString(index) only when index >= 0.
                    // Index source owners conservatively; this does not validate the available dataset range.
                    const auto dataset = canonicalUnsigned(*text);
                    if (!dataset || *dataset > 2147483647U)
                    {
                        result.malformedPaths.push_back(rowPath + "/1");
                        return std::nullopt;
                    }
                }
            }
        }
        return owners;
    }

    void table(NodeId object, const std::string& path)
    {
        panelReference(object, "selected_track", TrackerReferenceKind::TableSelection, path);
        if (const auto columns = xml.property(object, "track_columns"); columns.node)
        {
            if (const auto owners = rows(columns, path + "/track_columns", RowKind::Columns))
            {
                for (const auto& [ownerPath, name] : *owners)
                {
                    addReference(result, names, TrackerReferenceKind::TableColumnOwner, ownerPath, name, false);
                }
            }
        }
        if (const auto datasets = xml.property(object, "dataset_indices"); datasets.node)
        {
            if (const auto owners = rows(datasets, path + "/dataset_indices", RowKind::Datasets))
            {
                for (const auto& [ownerPath, name] : *owners)
                {
                    addReference(result, names, TrackerReferenceKind::TableDatasetOwner, ownerPath, name, false);
                }
            }
        }
        const auto formats = xml.property(object, "column_formats");
        if (!formats.node)
        {
            return;
        }
        const auto groups = array(formats, "[[[Ljava.lang.String;", path + "/column_formats");
        if (!groups)
        {
            return;
        }
        if (!groups->empty() && (groups->begin()->first != 0 || groups->rbegin()->first != groups->size() - 1))
        {
            result.malformedPaths.push_back(path + "/column_formats");
            return;
        }
        for (const auto& [groupIndex, group] : *groups)
        {
            if (const auto owners = rows({.node = group}, path + "/column_formats/" + std::to_string(groupIndex), RowKind::Formats))
            {
                for (const auto& [ownerPath, name] : *owners)
                {
                    addReference(result, names, TrackerReferenceKind::TableFormatOwner, ownerPath, name, false);
                }
            }
        }
    }

    void plotGuests(NodeId owner, const std::string& path)
    {
        for (unsigned plotIndex = 0; plotIndex < 3; ++plotIndex)
        {
            const auto field = "plot" + std::to_string(plotIndex);
            const auto value = xml.property(owner, field);
            if (!value.node)
            {
                continue;
            }
            countEntry();
            auto plotPath = path;
            plotPath += '/';
            plotPath += field;
            const auto object = xml.viewObject(*value.node);
            if (value.duplicate || !object || xml.node(*object).className != "org.opensourcephysics.cabrillo.tracker.TrackPlottingPanel")
            {
                result.malformedPaths.push_back(plotPath);
                continue;
            }
            const auto guests = xml.property(object, "guests");
            if (!guests.node)
            {
                continue;
            }
            const auto guestPath = plotPath + "/guests";
            const auto cells = array(guests, "[Ljava.lang.String;", guestPath);
            if (!cells)
            {
                continue;
            }
            if (!cells->empty() && (cells->begin()->first != 0 || cells->rbegin()->first != cells->size() - 1))
            {
                result.malformedPaths.push_back(guestPath);
                continue;
            }
            Owners references;
            for (const auto& [cellIndex, cell] : *cells)
            {
                const auto cellPath = guestPath + "/" + std::to_string(cellIndex);
                const auto name = xml.stringValue({.node = cell}, MAX_NAME_UNITS);
                if (!name)
                {
                    result.malformedPaths.push_back(cellPath);
                    references.clear();
                    break; // Do not emit part of a malformed guest array.
                }
                references.emplace_back(cellPath, *name);
            }
            for (const auto& [guestSlot, name] : references)
            {
                addReference(result, names, TrackerReferenceKind::PlotGuest, guestSlot, name, false);
            }
        }
    }

    void plot(NodeId object, const std::string& path)
    {
        panelReference(object, "selected_track", TrackerReferenceKind::PlotSelection, path);
        const auto views = xml.property(object, "track_views");
        if (!views.node)
        {
            // Legacy plotN properties use the selected track, not separate track owners.
            // Index serialized slots even if a loader would ignore a slot after a gap.
            plotGuests(object, path);
            return;
        }
        const auto ownerPath = path + "/track_views";
        if (views.duplicate || xml.node(*views.node).type != PropertyType::Collection ||
            xml.node(*views.node).className != "java.util.ArrayList")
        {
            result.malformedPaths.push_back(ownerPath);
            return;
        }
        std::size_t ordinal = 0;
        for (const auto item : xml.node(*views.node).children)
        {
            countEntry();
            const auto child = xml.viewObject(item);
            const auto childPath = ownerPath + "/" + std::to_string(ordinal++);
            if (!child || xml.node(*child).className != "org.opensourcephysics.cabrillo.tracker.PlotTrackView")
            {
                result.malformedPaths.push_back(childPath);
                continue;
            }
            panelReference(child, "track", TrackerReferenceKind::PlotOwner, childPath, true);
            plotGuests(*child, childPath);
        }
    }

    void attachments(NodeId object, std::size_t sourceIndex)
    {
        const auto& className = xml.node(object).className;
        const bool circle = className == "org.opensourcephysics.cabrillo.tracker.CircleFitter";
        if (!circle && className != "org.opensourcephysics.cabrillo.tracker.TapeMeasure" &&
            className != "org.opensourcephysics.cabrillo.tracker.Protractor")
        {
            return;
        }
        const auto trackPath = "/tracks/" + std::to_string(sourceIndex);
        if (circle)
        {
            panelReference(object, "step_attachment", TrackerReferenceKind::CircleStepAttachment, trackPath);
        }
        const auto value = xml.property(object, "attachments");
        if (!value.node)
        {
            return;
        }
        const auto path = trackPath + "/attachments";
        const auto attachmentSlots = array(value, "[Ljava.lang.String;", path);
        if (!attachmentSlots)
        {
            return;
        }
        // OSP omits null entries before the last encoded cell. The decoded extent is bounded
        // by array(), and implicit null cells must consume the shared entry budget too.
        const auto extent = attachmentSlots->empty() ? 0U : attachmentSlots->rbegin()->first + 1;
        for (auto count = attachmentSlots->size(); count < extent; ++count)
        {
            countEntry();
        }
        std::vector<std::optional<std::string>> targets(extent);
        for (const auto& [slot, item] : *attachmentSlots)
        {
            if (xml.node(item).type == PropertyType::Object && xml.node(item).children.empty())
            {
                continue; // An explicit null element.
            }
            if (auto name = xml.stringValue({.node = item}, MAX_NAME_UNITS))
            {
                targets[slot] = std::move(*name);
            }
            else
            {
                result.malformedPaths.push_back(path + "/" + std::to_string(slot));
                return;
            }
        }
        for (std::size_t slot = 0; slot < targets.size(); ++slot)
        {
            const auto slotPath = path + "/" + std::to_string(slot);
            if (const auto& target = targets[slot])
            {
                addReference(result, names, TrackerReferenceKind::Attachment, slotPath, *target, false);
            }
            else
            {
                result.references.push_back({.kind = TrackerReferenceKind::Attachment,
                                             .path = slotPath,
                                             .originalName = {},
                                             .resolution = TrackerReferenceResolution::NoTarget,
                                             .candidateSourceIndices = {}});
            }
        }
    }

    void modelDependencies(NodeId object, std::size_t sourceIndex)
    {
        const auto& className = xml.node(object).className;
        const bool system = className == "org.opensourcephysics.cabrillo.tracker.DynamicSystem";
        if (!system && className != DYNAMIC_PARTICLE_CLASS && className != POLAR_PARTICLE_CLASS)
        {
            return;
        }
        const auto trackPath = "/tracks/" + std::to_string(sourceIndex);
        panelReference(object, "booster", TrackerReferenceKind::ModelBooster, trackPath, false, POINT_MASS_CLASS);
        if (!system)
        {
            return;
        }
        const auto value = xml.property(object, "particles");
        if (!value.node)
        {
            return; // Empty systems omit their particles field in the normal serializer.
        }
        const auto path = trackPath + "/particles";
        const auto members = array(value, "[Ljava.lang.String;", path, 2);
        if (!members)
        {
            return;
        }
        if (!members->empty() && (members->begin()->first != 0 || members->rbegin()->first != members->size() - 1))
        {
            result.malformedPaths.push_back(path);
            return;
        }
        Owners targets;
        for (const auto& [slot, item] : *members)
        {
            const auto memberPath = path + "/" + std::to_string(slot);
            const auto name = xml.stringValue({.node = item}, MAX_NAME_UNITS);
            if (!name)
            {
                result.malformedPaths.push_back(memberPath);
                return;
            }
            targets.emplace_back(memberPath, *name);
        }
        for (const auto& [memberPath, name] : targets)
        {
            addReference(result, names, TrackerReferenceKind::DynamicSystemMember, memberPath, name, false, DYNAMIC_PARTICLE_CLASS);
        }
    }

    void constituents(NodeId object, std::size_t sourceIndex)
    {
        const auto& className = xml.node(object).className;
        const bool center = className == "org.opensourcephysics.cabrillo.tracker.CenterOfMass";
        if (!center && className != "org.opensourcephysics.cabrillo.tracker.VectorSum")
        {
            return;
        }
        const auto* field = center ? "masses" : "vectors";
        const auto value = xml.property(object, field);
        const auto path = "/tracks/" + std::to_string(sourceIndex) + "/" + field;
        if (value.duplicate || !value.node || xml.node(*value.node).type != PropertyType::Collection ||
            xml.node(*value.node).className != "java.util.ArrayList")
        {
            result.malformedPaths.push_back(path);
            return;
        }
        Owners members;
        for (const auto item : xml.node(*value.node).children)
        {
            countEntry();
            const auto memberPath = path + "/" + std::to_string(members.size());
            const auto name = xml.stringValue({.node = item}, MAX_NAME_UNITS);
            if (!name)
            {
                result.malformedPaths.push_back(memberPath);
                return; // No partially valid constituent list is emitted.
            }
            members.emplace_back(memberPath, *name);
        }
        for (const auto& [memberPath, name] : members)
        {
            addReference(result,
                         names,
                         center ? TrackerReferenceKind::CenterOfMassMember : TrackerReferenceKind::VectorSumMember,
                         memberPath,
                         name,
                         false,
                         center ? POINT_MASS_CLASS : VECTOR_CLASS);
        }
    }

    void read(NodeId panel)
    {
        const auto views = xml.property(panel, "views");
        if (!views.node)
        {
            return;
        }
        const auto choosers = array(views, "[[Lorg.opensourcephysics.cabrillo.tracker.TView;", "/views", 4);
        if (!choosers)
        {
            return;
        }
        for (const auto& [chooserIndex, chooser] : *choosers)
        {
            const auto path = "/views/" + std::to_string(chooserIndex);
            const auto viewSlots = array({.node = chooser}, "[Lorg.opensourcephysics.cabrillo.tracker.TView;", path, 4);
            if (!viewSlots)
            {
                continue;
            }
            for (const auto& [slot, item] : *viewSlots)
            {
                const auto childPath = path + "/" + std::to_string(slot);
                const auto object = xml.viewObject(item);
                const auto className = object ? std::string_view(xml.node(*object).className) : std::string_view{};
                if (!object || (className == "org.opensourcephysics.cabrillo.tracker.TableTView" && slot != 1) ||
                    (className == "org.opensourcephysics.cabrillo.tracker.PlotTView" && slot != 0))
                {
                    result.malformedPaths.push_back(childPath);
                }
                else if (className == "org.opensourcephysics.cabrillo.tracker.TableTView")
                {
                    table(*object, childPath);
                }
                else if (className == "org.opensourcephysics.cabrillo.tracker.PlotTView")
                {
                    plot(*object, childPath);
                }
            }
        }
    }
};

} // namespace

TrackerReferenceIndex indexTrackerReferences(const TrackerSource& source, const LegacyProject& project)
{
    const auto validated = detail::validatedTrackerSource(source, project);
    return detail::indexTrackerReferences(*validated.source, source);
}

TrackerReferenceIndex detail::indexTrackerReferences(const osp::Source& xml, const TrackerSource& source)
{
    const Reader reader{.xml = xml};
    TrackerReferenceIndex result;
    std::vector<std::pair<std::size_t, NodeId>> sourceObjects;
    NameTargets names;
    std::unordered_map<std::size_t, TrackId> ids;
    if (source.bindings)
    {
        for (const auto& binding : *source.bindings)
        {
            ids.emplace(binding.sourceIndex, binding.trackId);
        }
    }
    const auto panel = xml.root();
    const auto tracks = reader.property(panel, "tracks");
    if (tracks.duplicate || (tracks.node && xml.node(*tracks.node).type != PropertyType::Collection))
    {
        result.malformedPaths.emplace_back("/tracks");
    }
    else if (tracks.node)
    {
        std::size_t index = 0;
        for (const auto item : xml.node(*tracks.node).children)
        {
            if (index >= MAX_SOURCE_TRACKS)
            {
                throw ProjectError("Too many Tracker source tracks to index.");
            }
            const auto object = xml.node(item).type == PropertyType::Object && !xml.node(item).children.empty()
                                    ? std::optional(xml.node(item).children.front())
                                    : std::nullopt;
            TrackerSourceTrack track{.sourceIndex = index,
                                     .className = object ? xml.node(*object).className : std::string{},
                                     .originalName = {},
                                     .nativeId = {}};
            const auto path = "/tracks/" + std::to_string(index);
            if (!object || track.className.empty())
            {
                result.malformedPaths.push_back(path);
            }
            else
            {
                sourceObjects.emplace_back(index, *object);
                if (auto name = reader.stringValue(reader.property(object, "name"), MAX_NAME_UNITS))
                {
                    track.originalName = std::move(*name);
                    names.byName[*track.originalName].push_back(index);
                }
                else
                {
                    result.malformedPaths.push_back(path + "/name");
                }
            }
            if (const auto id = ids.find(index); id != ids.end())
            {
                track.nativeId = id->second;
            }
            result.tracks.push_back(std::move(track));
            ++index;
        }
    }
    ReferenceReader references{.result = result, .names = names, .xml = reader};
    references.panelReference(panel, "selectedtrack", TrackerReferenceKind::SelectedTrack);
    references.panelReference(panel, "referenceframe", TrackerReferenceKind::ReferenceFrame);
    references.viewReferences(panel);
    references.read(panel);
    for (const auto& [sourceIndex, object] : sourceObjects)
    {
        references.constituents(object, sourceIndex);
        references.attachments(object, sourceIndex);
        references.modelDependencies(object, sourceIndex);
    }
    return result;
}

} // namespace motrack
