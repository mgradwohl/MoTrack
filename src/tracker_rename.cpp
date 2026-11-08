#include "tracker_rename.hpp"

#include "tracker_references.hpp"

#include <QDomDocument>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace motrack
{
namespace
{
constexpr auto TRACKER = "org.opensourcephysics.cabrillo.tracker.";
constexpr auto MEDIA = "org.opensourcephysics.media.core.";

void require(bool valid)
{
    if (!valid)
    {
        throw ProjectError("This Tracker rename has unsupported content, names or relationships.");
    }
}

QDomElement property(const QDomElement& owner, const QString& name)
{
    QDomElement result;
    for (auto child = owner.firstChildElement(); !child.isNull(); child = child.nextSiblingElement())
    {
        if (child.tagName() == "property" && child.attribute("name") == name)
        {
            require(result.isNull());
            result = child;
        }
    }
    require(!result.isNull());
    return result;
}

std::size_t denseArraySize(const QDomElement& array)
{
    std::size_t count = 0;
    for (auto cell = array.firstChildElement(); !cell.isNull(); cell = cell.nextSiblingElement())
    {
        require(cell.attribute("name") == "[" + QString::number(count) + "]");
        ++count;
    }
    return count;
}

std::size_t modelNamesSize(const QDomElement& array)
{
    const auto count = denseArraySize(array);
    std::set<QString> names;
    for (auto cell = array.firstChildElement(); !cell.isNull(); cell = cell.nextSiblingElement())
        require(!cell.text().isEmpty() && names.insert(cell.text()).second);
    return count;
}

std::size_t modelValuesSize(const QDomElement& array)
{
    const auto first = array.firstChildElement();
    if (first.attribute("name") == "array")
    {
        const auto text = first.text().trimmed();
        require(text.size() <= 160000 && text.startsWith('{') && text.endsWith('}'));
        const auto cells = text.mid(1, text.size() - 2).split(',');
        require(cells.size() <= 10000);
        for (const auto& cell : cells)
        {
            bool valid = false;
            const auto value = cell.trimmed().toDouble(&valid);
            require(valid && std::isfinite(value));
        }
        return static_cast<std::size_t>(cells.size());
    }
    const auto count = denseArraySize(array);
    for (auto cell = first; !cell.isNull(); cell = cell.nextSiblingElement())
    {
        bool valid = false;
        const auto value = cell.text().toDouble(&valid);
        require(valid && std::isfinite(value));
    }
    return count;
}

// A reference index alone cannot establish rename eligibility. Admit only the audited media-free
// schemas below. Other fields/classes remain preserved by native saving but cannot bypass this gate.
using Fields = std::map<QString, QString>;
Fields fields(const QString& name)
{
    if (name == QString(TRACKER) + "TrackerPanel")
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
                {"videoclip", "object:" + QString(MEDIA) + "VideoClip"},
                {"clipcontrol", "object:" + QString(MEDIA) + "StepperClipControl"},
                {"coords", "object:" + QString(MEDIA) + "ImageCoordSystem"},
                {"tracks", "collection:java.util.ArrayList"},
                {"selectedtrack", "string"},
                {"dividers", "array:[D"},
                {"selected_view_types", "array:[I"},
                {"selected_track_views", "string"},
                {"views", "array:[[L" + QString(TRACKER) + "TView;"},
                {"toolbar", "object:" + QString(TRACKER) + "TToolBar"},
                {"track_control_x", "int"},
                {"track_control_y", "int"}};
    }
    if (name == QString(TRACKER) + "PointMass" || name == QString(TRACKER) + "CenterOfMass" || name == QString(TRACKER) + "Vector" ||
        name == QString(TRACKER) + "CoordAxes" || name == QString(TRACKER) + "Protractor" || name == QString(TRACKER) + "TapeMeasure" ||
        name == QString(TRACKER) + "DynamicParticle" || name == QString(TRACKER) + "DynamicSystem")
    {
        Fields result{{"name", "string"},
                      {"description", "string"},
                      {"color", "object:java.awt.Color"},
                      {"footprint", "string"},
                      {"visible", "boolean"},
                      {"trail", "boolean"},
                      {"locked", "boolean"}};
        if (name.endsWith(".PointMass") || name.endsWith(".CenterOfMass"))
        {
            result.insert(
                {{"mass", "double"}, {"framedata", "array:[L" + QString(TRACKER) + "PointMass$FrameData;"}, {"keyFrames", "array:[I"}});
            if (name.endsWith(".CenterOfMass"))
                result.emplace("masses", "collection:java.util.ArrayList");
        }
        else if (name.endsWith(".Vector"))
        {
            result.insert({{"framedata", "array:[L" + QString(TRACKER) + "Vector$FrameData;"}});
        }
        else if (name.endsWith(".Protractor"))
        {
            result.insert({{"fixed", "boolean"}, {"framedata", "array:[[D"}, {"ruler_visible", "boolean"}});
        }
        else if (name.endsWith(".TapeMeasure"))
        {
            result.insert({{"attachments", "array:[Ljava.lang.String;"},
                           {"fixedtape", "boolean"},
                           {"fixedlength", "boolean"},
                           {"readonly", "boolean"},
                           {"stickmode", "boolean"},
                           {"framedata", "array:[L" + QString(TRACKER) + "TapeMeasure$FrameData;"},
                           {"worldlengths", "array:[Ljava.lang.Double;"}});
        }
        else if (name.endsWith(".DynamicParticle") || name.endsWith(".DynamicSystem"))
        {
            result.insert({{"mass", "double"},
                           {"user_parameters", "array:[Lorg.opensourcephysics.tools.Parameter;"},
                           {"initial_values", "array:[Lorg.opensourcephysics.tools.Parameter;"},
                           {"main_functions", "array:[Lorg.opensourcephysics.tools.UserFunction;"}});
            if (name.endsWith(".DynamicSystem"))
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
    if (name == QString(TRACKER) + "PointMass$FrameData")
    {
        return {{"x", "double"}, {"y", "double"}};
    }
    if (name == QString(TRACKER) + "Vector$FrameData")
    {
        return {{"xtail", "double"}, {"ytail", "double"}, {"xcomponent", "double"}, {"ycomponent", "double"}};
    }
    if (name == QString(TRACKER) + "TapeMeasure$FrameData")
    {
        return {{"x1", "double"}, {"y1", "double"}, {"x2", "double"}, {"y2", "double"}};
    }
    if (name == "java.awt.Color")
    {
        return {{"red", "int"}, {"green", "int"}, {"blue", "int"}, {"alpha", "int"}};
    }
    if (name == QString(MEDIA) + "VideoClip")
    {
        return {{"startframe", "int"},
                {"stepsize", "int"},
                {"stepcount", "int"},
                {"starttime", "double"},
                {"readout", "string"},
                {"playallsteps", "boolean"}};
    }
    if (name == QString(MEDIA) + "StepperClipControl")
    {
        return {{"rate", "double"}, {"delta_t", "double"}, {"frame", "int"}};
    }
    if (name == QString(MEDIA) + "ImageCoordSystem")
    {
        return {{"fixedorigin", "boolean"},
                {"fixedangle", "boolean"},
                {"fixedscale", "boolean"},
                {"locked", "boolean"},
                {"framedata", "array:[L" + QString(MEDIA) + "ImageCoordSystem$FrameData;"}};
    }
    if (name == QString(MEDIA) + "ImageCoordSystem$FrameData")
    {
        return {{"xorigin", "double"}, {"yorigin", "double"}, {"angle", "double"}, {"xscale", "double"}, {"yscale", "double"}};
    }
    if (name == QString(TRACKER) + "TableTView")
    {
        return {{"selected_track", "string"},
                {"track_columns", "array:[[Ljava.lang.String;"},
                {"column_formats", "array:[[[Ljava.lang.String;"}};
    }
    if (name == QString(TRACKER) + "TToolBar")
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
    if (name == QString(TRACKER) + "PlotTView")
    {
        return {{"selected_track", "string"}, {"track_views", "collection:java.util.ArrayList"}};
    }
    if (name == QString(TRACKER) + "PlotTrackView")
    {
        const auto plot = "object:" + QString(TRACKER) + "TrackPlottingPanel";
        return {{"track", "string"}, {"linked", "boolean"}, {"plot0", plot}, {"plot1", plot}, {"plot2", plot}};
    }
    if (name == QString(TRACKER) + "TrackPlottingPanel")
    {
        return {
            {"x_var", "string"}, {"y_var", "string"}, {"lines", "boolean"}, {"points", "boolean"}, {"guests", "array:[Ljava.lang.String;"}};
    }
    require(false);
    return {};
}

struct RenameSchema
{
    unsigned entries = 0;

    void object(const QDomElement& value, const QString& expected, unsigned depth = 0)
    {
        require(depth < 32 && value.tagName() == "object");
        require(value.attributes().size() == 1 && value.hasAttribute("class"));
        const auto actual = value.attribute("class");
        require(actual == expected || (expected == QString(TRACKER) + "TView" &&
                                       (actual == QString(TRACKER) + "TableTView" || actual == QString(TRACKER) + "PlotTView")));
        const auto allowed = fields(actual);
        std::set<QString> seen;
        for (auto child = value.firstChildElement(); !child.isNull(); child = child.nextSiblingElement())
        {
            const auto name = child.attribute("name");
            require(seen.insert(name).second && allowed.contains(name));
            propertyValue(child, allowed.at(name), depth + 1);
        }
        require(actual != QString(TRACKER) + "CenterOfMass" || seen.contains("masses"));
        if (actual == "org.opensourcephysics.tools.Parameter")
            require(seen.contains("name") && seen.contains("function"));
        if (actual == "org.opensourcephysics.tools.UserFunction")
        {
            require(seen.contains("name") && seen.contains("expression") && seen.contains("variables") &&
                    seen.contains("parameter_names") && seen.contains("parameter_values"));
            const auto count = modelNamesSize(property(value, "parameter_names"));
            require(count == modelValuesSize(property(value, "parameter_values")));
            if (seen.contains("parameter_descriptions"))
                require(denseArraySize(property(value, "parameter_descriptions")) == count);
            // Both supported force evaluators pass five state values. A mismatched variable list
            // changes force evaluation; it is not harmless preserved metadata.
            require(modelNamesSize(property(value, "variables")) == 5);
        }
        if (actual == QString(TRACKER) + "DynamicParticle" || actual == QString(TRACKER) + "DynamicSystem")
        {
            require(seen.contains("user_parameters") && seen.contains("initial_values") && seen.contains("main_functions"));
            const auto initial = property(value, "initial_values");
            require(denseArraySize(initial) == 5);
            const auto names =
                actual == QString(TRACKER) + "DynamicParticle"
                    ? QStringList{"t", "x", "y", "vx", "vy"}
                    : QStringList{"t", "r_relative", QString::fromUtf8("θ_relative"), "vr_relative", QString::fromUtf8("ω_relative")};
            auto cell = initial.firstChildElement();
            for (const auto& name : names)
            {
                require(property(cell.firstChildElement(), "name").text() == name);
                cell = cell.nextSiblingElement();
            }
            if (actual == QString(TRACKER) + "DynamicParticle")
                require(property(value, "in_system").text() == "true");
            else
                require(seen.contains("particles"));
        }
        if (actual == QString(TRACKER) + "TapeMeasure")
        {
            // Only the read-only fixed tape mode has edited-name application acceptance.
            require(seen.contains("attachments") && property(value, "readonly").text() == "true" &&
                    property(value, "fixedtape").text() == "true" && property(value, "fixedlength").text() == "true" &&
                    property(value, "stickmode").text() == "false");
            for (const auto& field : {"framedata", "worldlengths"})
            {
                const auto item = property(value, field).firstChildElement();
                require(!item.isNull() && item.attribute("name") == "[0]" && item.nextSiblingElement().isNull());
            }
        }

        // Tracker synchronizes linked x axes while loading each panel. Until that behavior has
        // its own application acceptance case, admit only explicitly unlinked custom plots.
        if (actual == QString(TRACKER) + "PlotTrackView")
            require(property(value, "linked").text() == "false");
    }

    void propertyValue(const QDomElement& value, const QString& signature, unsigned depth)
    {
        require(depth < 32 && ++entries <= 10000 && value.tagName() == "property");
        const auto separator = signature.indexOf(':');
        const auto type = separator < 0 ? signature : signature.left(separator);
        const auto expected = separator < 0 ? QString{} : signature.mid(separator + 1);
        require(value.attribute("type") == type);
        require(value.hasAttribute("name") && value.attributes().size() == (type == "array" || type == "collection" ? 3 : 2));
        if (type == "object")
        {
            const auto nested = value.firstChildElement();
            // Track serializers retain frame-array extent with typed null final cells.
            // This is geometry data, not a missing named object or permission for arbitrary nulls.
            if (nested.isNull() &&
                (expected == QString(TRACKER) + "Vector$FrameData" || expected == QString(TRACKER) + "PointMass$FrameData") &&
                value.text() == "null")
            {
                return;
            }
            require(!nested.isNull() && nested.nextSiblingElement().isNull());
            object(nested, expected, depth + 1);
        }
        else if (type == "collection")
        {
            require(value.attribute("class") == expected);
            std::set<QString> members;
            std::set<QString> owners;
            for (auto item = value.firstChildElement(); !item.isNull(); item = item.nextSiblingElement())
            {
                if (value.attribute("name") == "masses")
                {
                    propertyValue(item, "string", depth + 1);
                    require(members.insert(item.text()).second); // Java coalesces duplicate members; do not accept that loss.
                    continue;
                }
                const auto className = item.firstChildElement().attribute("class");
                if (value.attribute("name") == "track_views")
                {
                    require(className == QString(TRACKER) + "PlotTrackView");
                    // The Java loader reuses one view per owner; later duplicates overwrite it.
                    require(owners.insert(property(item.firstChildElement(), "track").text()).second);
                }
                else
                {
                    require(value.attribute("name") == "tracks" &&
                            (className == QString(TRACKER) + "PointMass" || className == QString(TRACKER) + "CoordAxes" ||
                             className == QString(TRACKER) + "Protractor" || className == QString(TRACKER) + "Vector" ||
                             className == QString(TRACKER) + "CenterOfMass" || className == QString(TRACKER) + "TapeMeasure" ||
                             className == QString(TRACKER) + "DynamicParticle" || className == QString(TRACKER) + "DynamicSystem"));
                }
                propertyValue(item, "object:" + className, depth + 1);
            }
            require(value.attribute("name") != "masses" || !members.empty());
        }
        else if (type == "array")
        {
            require(value.attribute("class") == expected && expected.startsWith('['));
            const auto element = expected.mid(1);
            std::set<QString> seen;
            std::set<QString> guests;
            std::set<QString> modelNames;
            for (auto item = value.firstChildElement(); !item.isNull(); item = item.nextSiblingElement())
            {
                const auto name = item.attribute("name");
                require(seen.insert(name).second);
                if (name == "array" && (element == "D" || element == "I" || element == "[D"))
                {
                    require(value.firstChildElement() == item && item.nextSiblingElement().isNull());
                    propertyValue(item, "string", depth + 1); // Primitive packed values contain no track names.
                    continue;
                }
                bool parsed = false;
                const auto index = name.mid(1, name.size() - 2).toUInt(&parsed);
                // A sparse original frame is not an entry count. Match the writer's track-frame bound
                // without relaxing generic array indices or the aggregate schema-entry/depth limits.
                const bool trackFrames =
                    value.attribute("name") == "framedata" &&
                    (element == "L" + QString(TRACKER) + "PointMass$FrameData;" || element == "L" + QString(TRACKER) + "Vector$FrameData;");
                require(parsed && index <= (trackFrames ? 500000u : 9999u) && name == "[" + QString::number(index) + "]");
                if (value.attribute("name") == "attachments")
                {
                    require(index < 2);
                    if (item.attribute("type") == "object")
                    {
                        require(depth + 1 < 32 && item.tagName() == "property" && item.attributes().size() == 2 &&
                                item.firstChildElement().isNull() && item.text() == "null" && ++entries <= 10000);
                        continue; // Explicit empty endpoint; implicit gaps remain untouched too.
                    }
                }
                QString childType;
                if (element.startsWith('['))
                    childType = "array:" + element;
                else if (element == "Ljava.lang.String;")
                    childType = "string";
                else if (element == "D" || element == "Ljava.lang.Double;")
                    childType = "double";
                else if (element.startsWith('L') && element.endsWith(';'))
                    childType = "object:" + element.mid(1, element.size() - 2);
                else if (element == "I")
                    childType = "int";
                else
                    require(false);
                propertyValue(item, childType, depth + 1);
                // Java ignores an already-present guest. The same guest in another panel is valid.
                require(value.attribute("name") != "guests" || guests.insert(item.text()).second);
                if (element == "Lorg.opensourcephysics.tools.Parameter;" || element == "Lorg.opensourcephysics.tools.UserFunction;")
                {
                    const auto nameField = property(item.firstChildElement(), "name");
                    require(!nameField.isNull() && !nameField.text().isEmpty() && modelNames.insert(nameField.text()).second);
                }
            }
            require(value.attribute("name") != "particles" || seen.size() == 2);
            if (element == "Lorg.opensourcephysics.tools.Parameter;" || element == "Lorg.opensourcephysics.tools.UserFunction;")
            {
                require(!seen.empty());
                for (std::size_t i = 0; i < seen.size(); ++i)
                    require(seen.contains("[" + QString::number(i) + "]"));
                require(value.attribute("name") != "main_functions" || seen.size() == 2);
            }
        }
        else
        {
            require(value.firstChildElement().isNull());
        }
    }
};

void patchString(QDomDocument& xml, QDomElement value, const QString& text)
{
    require(value.attribute("type") == "string" && !text.contains("]]>"));
    for (auto child = value.firstChild(); !child.isNull(); child = child.nextSibling())
    {
        require(child.isText() || child.isCDATASection()); // Match OSP's strict CDATA terminator handling.
    }
    while (!value.firstChild().isNull())
        value.removeChild(value.firstChild());
    value.appendChild(xml.createCDATASection(text));
}

QDomElement referenceProperty(const QDomElement& root, const std::string& path)
{
    auto current = root;
    for (const auto& part : QString::fromStdString(path).split('/', Qt::SkipEmptyParts))
    {
        if (current.tagName() == "property" && current.attribute("type") == "object")
            current = current.firstChildElement("object");
        bool numeric = false;
        const auto ordinal = part.toUInt(&numeric);
        if (numeric && current.attribute("type") == "collection")
        {
            auto item = current.firstChildElement("property");
            for (unsigned index = 0; index < ordinal && !item.isNull(); ++index)
                item = item.nextSiblingElement("property");
            require(!item.isNull());
            current = item; // Collection paths use source ordinals, not the serializer's repeated "item" labels.
        }
        else
            current = property(current, numeric ? "[" + part + "]" : part);
    }
    return current;
}

void requireName(const std::string& name)
{
    const auto text = QString::fromStdString(name);
    require(!text.isEmpty() && text.size() <= 4096 && text.toStdString() == name && text == text.trimmed() && text != "null" &&
            !text.contains(',') && !text.contains(';') && !text.contains("]]>"));
    for (const auto point : text.toUcs4())
    {
        require(QChar::category(point) != QChar::Other_Control && QChar::category(point) != QChar::Other_Format);
    }
}
} // namespace

void validateTrackerTrackMembership(const QDomDocument& xml, const LegacyProject& project, const TrackerSource& source)
{
    RenameSchema{}.object(xml.documentElement(), QString(TRACKER) + "TrackerPanel");
    const auto index = indexTrackerReferences(source, project);
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
        require(track.className == std::string(TRACKER) + "PointMass" || track.className == std::string(TRACKER) + "Vector" ||
                track.className == std::string(TRACKER) + "CoordAxes" || track.className == std::string(TRACKER) + "Protractor");
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
            // Retained source names must never silently rebind to a newly created track.
            require(!target->second->nativeId || liveIds.contains(*target->second->nativeId));
            if (reference.kind != TrackerReferenceKind::SelectedTrack)
                require(target->second->className != std::string(TRACKER) + "CoordAxes");
        }
    }
}

bool patchTrackerNames(QDomDocument& xml, const LegacyProject& project, LegacyProject& baseline, const TrackerSource& source)
{
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
    RenameSchema{}.object(xml.documentElement(), QString(TRACKER) + "TrackerPanel");
    const auto index = indexTrackerReferences(source, project);
    require(index.malformedPaths.empty());
    std::map<std::size_t, QString> replacements;
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
            continue; // The membership audit has already rejected references to any deleted track.
        const auto found = track.nativeId ? renamed.find(*track.nativeId) : renamed.end();
        const auto& name = found == renamed.end() ? *track.originalName : found->second;
        require(newNames.insert(name).second);
        if (found != renamed.end())
        {
            requireName(name);
            replacements.emplace(track.sourceIndex, QString::fromStdString(name));
        }
    }
    require(replacements.size() == renamed.size());
    bool packedChanged = false;
    std::set<std::size_t> systemMembers;
    QStringList packed;
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
            require(target != index.tracks.end() && target->className == "org.opensourcephysics.cabrillo.tracker.PointMass");
        }
        if (reference.kind == TrackerReferenceKind::PlotSelection || reference.kind == TrackerReferenceKind::TableSelection ||
            reference.kind == TrackerReferenceKind::PlotOwner || reference.kind == TrackerReferenceKind::PlotGuest ||
            reference.kind == TrackerReferenceKind::TableColumnOwner || reference.kind == TrackerReferenceKind::TableFormatOwner)
        {
            const auto target = std::ranges::find(index.tracks, reference.candidateSourceIndices.front(), &TrackerSourceTrack::sourceIndex);
            // TrackChooserTView ignores CoordAxes even when the named source object exists.
            require(target != index.tracks.end() && target->className != "org.opensourcephysics.cabrillo.tracker.CoordAxes");
        }
        const auto replacement = replacements.find(reference.candidateSourceIndices.front());
        if (replacement == replacements.end())
            continue;
        if (reference.path.starts_with("/selected_track_views/"))
        {
            if (packed.empty())
                packed = property(xml.documentElement(), "selected_track_views").text().replace(';', ',').split(',');
            const auto parts = QString::fromStdString(reference.path).split('/');
            const auto slot = parts[2].toInt() * 2 + (parts[3] == "table" ? 1 : 0);
            require(packed.size() == 8 && slot >= 0 && slot < 8 && packed[slot].toStdString() == reference.originalName);
            packed[slot] = replacement->second;
            packedChanged = true;
        }
        else
        {
            auto value = referenceProperty(xml.documentElement(), reference.path);
            require(value.text().toStdString() == reference.originalName);
            patchString(xml, value, replacement->second);
        }
    }
    for (const auto& track : index.tracks)
        if (track.className == "org.opensourcephysics.cabrillo.tracker.DynamicParticle")
            require(systemMembers.contains(track.sourceIndex));
    if (packedChanged)
    {
        QStringList pairs;
        for (int i = 0; i < 8; i += 2)
            pairs.append(packed[i] + "," + packed[i + 1]);
        patchString(xml, property(xml.documentElement(), "selected_track_views"), pairs.join(';'));
    }
    std::size_t ordinal = 0;
    const auto tracks = property(xml.documentElement(), "tracks");
    for (auto item = tracks.firstChildElement("property"); !item.isNull(); item = item.nextSiblingElement("property"), ++ordinal)
    {
        if (const auto found = replacements.find(ordinal); found != replacements.end())
            patchString(xml, property(item.firstChildElement("object"), "name"), found->second);
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
