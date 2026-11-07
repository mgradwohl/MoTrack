#include "tracker_references.hpp"

#include <QByteArray>
#include <QDomDocument>
#include <QStringList>

#include <algorithm>
#include <map>
#include <unordered_map>
#include <utility>

namespace motrack
{
namespace
{

constexpr std::size_t MAX_SOURCE_TRACKS = 10000;
constexpr qsizetype MAX_NAME_UNITS = 4096;
constexpr qsizetype MAX_VIEW_UNITS = 65536;

struct Property
{
    QDomElement element;
    bool duplicate = false;
};

Property property(const QDomElement& owner, const QString& name)
{
    Property result;
    for (auto child = owner.firstChildElement("property"); !child.isNull(); child = child.nextSiblingElement("property"))
    {
        if (child.attribute("name") == name)
        {
            if (!result.element.isNull())
            {
                result.duplicate = true;
            }
            else
            {
                result.element = child;
            }
        }
    }
    return result;
}

std::optional<QString> stringValue(const Property& value, qsizetype limit)
{
    if (value.duplicate || value.element.isNull() || value.element.attribute("type") != "string")
    {
        return std::nullopt;
    }
    QString result;
    for (auto child = value.element.firstChild(); !child.isNull(); child = child.nextSibling())
    {
        if ((!child.isText() && !child.isCDATASection()) || child.nodeValue().size() > limit - result.size())
        {
            return std::nullopt;
        }
        result += child.nodeValue();
    }
    return result;
}

using NameTargets = std::unordered_map<std::string, std::vector<std::size_t>>;

bool onlyChildren(const QDomElement& owner, const QString& tag)
{
    for (auto child = owner.firstChild(); !child.isNull(); child = child.nextSibling())
    {
        if (child.isComment() || child.isProcessingInstruction() || (child.isElement() && child.nodeName() == tag))
        {
            continue;
        }
        if ((child.isText() || child.isCDATASection()) &&
            std::ranges::all_of(child.nodeValue(),
                                [](QChar value) { return value == ' ' || value == '\t' || value == '\n' || value == '\r'; }))
        {
            continue;
        }
        return false;
    }
    return true;
}

void addReference(
    TrackerReferenceIndex& result, const NameTargets& names, TrackerReferenceKind kind, std::string path, const QString& name, bool packed)
{
    TrackerNameReference reference{.kind = kind,
                                   .path = std::move(path),
                                   .originalName = name.toStdString(),
                                   .resolution = TrackerReferenceResolution::Missing,
                                   .candidateSourceIndices = {}};
    if (packed && name == "null")
    {
        reference.resolution = TrackerReferenceResolution::NoTarget;
    }
    else if (const auto found = names.find(reference.originalName); found != names.end())
    {
        reference.candidateSourceIndices = found->second;
        reference.resolution = found->second.size() == 1 ? TrackerReferenceResolution::Resolved : TrackerReferenceResolution::Ambiguous;
    }
    result.references.push_back(std::move(reference));
}

void panelReference(TrackerReferenceIndex& result,
                    const NameTargets& names,
                    const QDomElement& panel,
                    const QString& field,
                    TrackerReferenceKind kind,
                    const std::string& prefix = {},
                    bool required = false)
{
    const auto value = property(panel, field);
    if (value.element.isNull() && !required)
    {
        return;
    }
    const auto name = stringValue(value, MAX_NAME_UNITS);
    const auto path = prefix + "/" + field.toStdString();
    if (!name)
    {
        result.malformedPaths.push_back(path);
        return;
    }
    addReference(result, names, kind, path, *name, false);
}

void viewReferences(TrackerReferenceIndex& result, const NameTargets& names, const QDomElement& panel)
{
    const auto propertyValue = property(panel, "selected_track_views");
    if (propertyValue.element.isNull())
    {
        return;
    }
    const auto value = stringValue(propertyValue, MAX_VIEW_UNITS);
    const auto malformed = [&]()
    {
        result.malformedPaths.emplace_back("/selected_track_views");
    };
    if (!value)
    {
        malformed();
        return;
    }
    const auto choosers = value->split(';');
    if (choosers.size() != 4)
    {
        malformed();
        return;
    }
    // Validate the entire packed value before emitting any references from it.
    std::vector<QStringList> pairs;
    for (const auto& chooser : choosers)
    {
        auto pair = chooser.split(',');
        if (pair.size() != 2 || pair[0].isEmpty() || pair[1].isEmpty() || pair[0].size() > MAX_NAME_UNITS ||
            pair[1].size() > MAX_NAME_UNITS)
        {
            malformed();
            return;
        }
        pairs.push_back(std::move(pair));
    }
    for (std::size_t index = 0; index < pairs.size(); ++index)
    {
        const auto path = "/selected_track_views/" + std::to_string(index);
        addReference(result, names, TrackerReferenceKind::PlotSelection, path + "/plot", pairs[index][0], true);
        addReference(result, names, TrackerReferenceKind::TableSelection, path + "/table", pairs[index][1], true);
    }
}

QDomElement viewObject(const QDomElement& item)
{
    const auto object = item.firstChildElement("object");
    if (item.attribute("type") != "object" || object.isNull() || !object.nextSiblingElement().isNull() ||
        object.attribute("class").isEmpty() || !onlyChildren(item, "object"))
    {
        return {};
    }
    return object;
}

struct SavedViewReader
{
    TrackerReferenceIndex& result;
    const NameTargets& names;
    std::size_t entries = 0;

    using Array = std::map<unsigned, QDomElement>;
    using Owners = std::vector<std::pair<std::string, QString>>;

    void countEntry()
    {
        if (++entries > 10000)
        {
            throw ProjectError("Too many nested Tracker view entries to index.");
        }
    }

    std::optional<Array> array(const Property& value, const QString& className, const std::string& path, unsigned bound = 10000)
    {
        Array children;
        bool valid = !value.duplicate && value.element.attribute("type") == "array" && value.element.attribute("class") == className &&
                     onlyChildren(value.element, "property");
        if (valid)
        {
            for (auto child = value.element.firstChildElement("property"); !child.isNull(); child = child.nextSiblingElement("property"))
            {
                countEntry();
                const auto label = child.attribute("name");
                bool parsed = false;
                const auto index = label.mid(1, label.size() - 2).toUInt(&parsed);
                if (!parsed || index >= bound || label != "[" + QString::number(index) + "]" || !children.emplace(index, child).second)
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
    std::optional<Owners> rows(const Property& value, const std::string& path, bool formats)
    {
        const auto rows = array(value, "[[Ljava.lang.String;", path);
        if (!rows)
        {
            return std::nullopt;
        }
        Owners owners;
        if (formats && (rows->empty() || rows->begin()->first != 0 || rows->rbegin()->first != rows->size() - 1))
        {
            result.malformedPaths.push_back(path);
            return std::nullopt;
        }
        for (const auto& [rowIndex, row] : *rows)
        {
            const auto rowPath = path + "/" + std::to_string(rowIndex);
            const auto columns = array({.element = row}, "[Ljava.lang.String;", rowPath);
            if (!columns)
            {
                return std::nullopt;
            }
            if (columns->empty() || columns->begin()->first != 0 || columns->rbegin()->first != columns->size() - 1 ||
                (formats && columns->size() != 3))
            {
                result.malformedPaths.push_back(rowPath);
                return std::nullopt;
            }
            for (const auto& [columnIndex, column] : *columns)
            {
                const auto text = stringValue({.element = column}, MAX_NAME_UNITS);
                if (!text)
                {
                    result.malformedPaths.push_back(rowPath + "/" + std::to_string(columnIndex));
                    return std::nullopt;
                }
                if (columnIndex == 0)
                {
                    if (formats && !owners.empty() && owners.front().second != *text)
                    {
                        result.malformedPaths.push_back(rowPath + "/0");
                        return std::nullopt;
                    }
                    owners.emplace_back(rowPath + "/0", *text);
                }
            }
        }
        return owners;
    }

    void table(const QDomElement& object, const std::string& path)
    {
        panelReference(result, names, object, "selected_track", TrackerReferenceKind::TableSelection, path);
        const auto columns = property(object, "track_columns");
        if (!columns.element.isNull())
        {
            if (const auto owners = rows(columns, path + "/track_columns", false))
            {
                for (const auto& [ownerPath, name] : *owners)
                {
                    addReference(result, names, TrackerReferenceKind::TableColumnOwner, ownerPath, name, false);
                }
            }
        }
        const auto formats = property(object, "column_formats");
        if (formats.element.isNull())
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
            if (const auto owners = rows({.element = group}, path + "/column_formats/" + std::to_string(groupIndex), true))
            {
                for (const auto& [ownerPath, name] : *owners)
                {
                    addReference(result, names, TrackerReferenceKind::TableFormatOwner, ownerPath, name, false);
                }
            }
        }
    }

    void plot(const QDomElement& object, const std::string& path)
    {
        panelReference(result, names, object, "selected_track", TrackerReferenceKind::PlotSelection, path);
        const auto views = property(object, "track_views");
        if (views.element.isNull())
        {
            return; // Legacy plotN properties use the selected track, not separate track owners.
        }
        const auto ownerPath = path + "/track_views";
        if (views.duplicate || views.element.attribute("type") != "collection" ||
            views.element.attribute("class") != "java.util.ArrayList" || !onlyChildren(views.element, "property"))
        {
            result.malformedPaths.push_back(ownerPath);
            return;
        }
        std::size_t ordinal = 0;
        for (auto item = views.element.firstChildElement("property"); !item.isNull(); item = item.nextSiblingElement("property"), ++ordinal)
        {
            countEntry();
            const auto child = viewObject(item);
            const auto childPath = ownerPath + "/" + std::to_string(ordinal);
            if (child.attribute("class") != "org.opensourcephysics.cabrillo.tracker.PlotTrackView")
            {
                result.malformedPaths.push_back(childPath);
                continue;
            }
            panelReference(result, names, child, "track", TrackerReferenceKind::PlotOwner, childPath, true);
        }
    }

    void read(const QDomElement& panel)
    {
        const auto views = property(panel, "views");
        if (views.element.isNull())
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
            const auto viewSlots = array({.element = chooser}, "[Lorg.opensourcephysics.cabrillo.tracker.TView;", path, 4);
            if (!viewSlots)
            {
                continue;
            }
            for (const auto& [slot, item] : *viewSlots)
            {
                const auto childPath = path + "/" + std::to_string(slot);
                const auto object = viewObject(item);
                const auto className = object.attribute("class");
                if (object.isNull() || (className == "org.opensourcephysics.cabrillo.tracker.TableTView" && slot != 1) ||
                    (className == "org.opensourcephysics.cabrillo.tracker.PlotTView" && slot != 0))
                {
                    result.malformedPaths.push_back(childPath);
                }
                else if (className == "org.opensourcephysics.cabrillo.tracker.TableTView")
                {
                    table(object, childPath);
                }
                else if (className == "org.opensourcephysics.cabrillo.tracker.PlotTView")
                {
                    plot(object, childPath);
                }
            }
        }
    }
};

} // namespace

TrackerReferenceIndex indexTrackerReferences(const TrackerSource& source, const LegacyProject& project)
{
    validateTrackerSource(source, project);
    QDomDocument xml;
    if (!xml.setContent(QByteArray::fromStdString(source.xml), QDomDocument::ParseOption::PreserveSpacingOnlyNodes))
    {
        throw ProjectError("Invalid Tracker reference XML.");
    }
    TrackerReferenceIndex result;
    NameTargets names;
    std::unordered_map<std::size_t, TrackId> ids;
    if (source.bindings)
    {
        for (const auto& binding : *source.bindings)
        {
            ids.emplace(binding.sourceIndex, binding.trackId);
        }
    }
    const auto panel = xml.documentElement();
    const auto tracks = property(panel, "tracks");
    const auto containerType = tracks.element.attribute("type");
    if (tracks.duplicate || (!containerType.isEmpty() && containerType != "collection") || !onlyChildren(tracks.element, "property"))
    {
        result.malformedPaths.emplace_back("/tracks");
    }
    else
    {
        std::size_t index = 0;
        for (auto item = tracks.element.firstChildElement("property"); !item.isNull(); item = item.nextSiblingElement("property"), ++index)
        {
            if (index >= MAX_SOURCE_TRACKS)
            {
                throw ProjectError("Too many Tracker source tracks to index.");
            }
            const auto object = item.firstChildElement("object");
            TrackerSourceTrack track{
                .sourceIndex = index, .className = object.attribute("class").toStdString(), .originalName = {}, .nativeId = {}};
            const auto path = "/tracks/" + std::to_string(index);
            const auto itemType = item.attribute("type");
            if (object.isNull() || !object.nextSiblingElement().isNull() || track.className.empty() ||
                (!itemType.isEmpty() && itemType != "object") || !onlyChildren(item, "object"))
            {
                result.malformedPaths.push_back(path);
            }
            else if (const auto name = stringValue(property(object, "name"), MAX_NAME_UNITS))
            {
                track.originalName = name->toStdString();
                names[*track.originalName].push_back(index);
            }
            else
            {
                result.malformedPaths.push_back(path + "/name");
            }
            if (const auto id = ids.find(index); id != ids.end())
            {
                track.nativeId = id->second;
            }
            result.tracks.push_back(std::move(track));
        }
    }
    panelReference(result, names, panel, "selectedtrack", TrackerReferenceKind::SelectedTrack);
    panelReference(result, names, panel, "referenceframe", TrackerReferenceKind::ReferenceFrame);
    viewReferences(result, names, panel);
    SavedViewReader{.result = result, .names = names}.read(panel);
    return result;
}

} // namespace motrack
