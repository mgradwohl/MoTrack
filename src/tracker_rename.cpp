#include "tracker_rename.hpp"

#include "tracker_references.hpp"

#include <QDomDocument>
#include <QStringList>

#include <algorithm>
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

// A reference index alone cannot establish rename eligibility. This first writer admits only the
// audited media-free mass/vector/protractor/table schema. Unknown fields/classes, models, functions, moving
// frames and attachments remain preserved by native saving but cannot silently bypass this gate.
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
    if (name == QString(TRACKER) + "PointMass" || name == QString(TRACKER) + "Vector" || name == QString(TRACKER) + "CoordAxes" ||
        name == QString(TRACKER) + "Protractor")
    {
        Fields result{{"name", "string"},
                      {"description", "string"},
                      {"color", "object:java.awt.Color"},
                      {"footprint", "string"},
                      {"visible", "boolean"},
                      {"trail", "boolean"},
                      {"locked", "boolean"}};
        if (name.endsWith(".PointMass"))
        {
            result.insert(
                {{"mass", "double"}, {"framedata", "array:[L" + QString(TRACKER) + "PointMass$FrameData;"}, {"keyFrames", "array:[I"}});
        }
        else if (name.endsWith(".Vector"))
        {
            result.insert({{"framedata", "array:[L" + QString(TRACKER) + "Vector$FrameData;"}});
        }
        else if (name.endsWith(".Protractor"))
        {
            result.insert({{"fixed", "boolean"}, {"framedata", "array:[[D"}, {"ruler_visible", "boolean"}});
        }
        return result;
    }
    if (name == QString(TRACKER) + "PointMass$FrameData")
    {
        return {{"x", "double"}, {"y", "double"}};
    }
    if (name == QString(TRACKER) + "Vector$FrameData")
    {
        return {{"xtail", "double"}, {"ytail", "double"}, {"xcomponent", "double"}, {"ycomponent", "double"}};
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
        require(actual == expected || (expected == QString(TRACKER) + "TView" && actual == QString(TRACKER) + "TableTView"));
        const auto allowed = fields(actual);
        std::set<QString> seen;
        for (auto child = value.firstChildElement(); !child.isNull(); child = child.nextSiblingElement())
        {
            const auto name = child.attribute("name");
            require(seen.insert(name).second && allowed.contains(name));
            propertyValue(child, allowed.at(name), depth + 1);
        }
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
            // The Vector serializer retains its frame-array extent with a typed null final cell.
            // This is geometry data, not a missing named object or permission for arbitrary nulls.
            if (nested.isNull() && expected == QString(TRACKER) + "Vector$FrameData" && value.text() == "null")
            {
                return;
            }
            require(!nested.isNull() && nested.nextSiblingElement().isNull());
            object(nested, expected, depth + 1);
        }
        else if (type == "collection")
        {
            require(value.attribute("class") == expected);
            for (auto item = value.firstChildElement(); !item.isNull(); item = item.nextSiblingElement())
            {
                const auto className = item.firstChildElement().attribute("class");
                require(className == QString(TRACKER) + "PointMass" || className == QString(TRACKER) + "CoordAxes" ||
                        className == QString(TRACKER) + "Protractor" || className == QString(TRACKER) + "Vector");
                propertyValue(item, "object:" + className, depth + 1);
            }
        }
        else if (type == "array")
        {
            require(value.attribute("class") == expected && expected.startsWith('['));
            const auto element = expected.mid(1);
            std::set<QString> seen;
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
                require(parsed && index < 10000 && name == "[" + QString::number(index) + "]");
                QString childType;
                if (element.startsWith('['))
                    childType = "array:" + element;
                else if (element == "Ljava.lang.String;")
                    childType = "string";
                else if (element.startsWith('L') && element.endsWith(';'))
                    childType = "object:" + element.mid(1, element.size() - 2);
                else if (element == "D")
                    childType = "double";
                else if (element == "I")
                    childType = "int";
                else
                    require(false);
                propertyValue(item, childType, depth + 1);
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
        (void)part.toUInt(&numeric);
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

bool patchTrackerNames(QDomDocument& xml, const LegacyProject& project, LegacyProject& baseline, const TrackerSource& source)
{
    std::map<TrackId, std::string> renamed;
    const auto collect = [&](const auto& current, const auto& original)
    {
        require(current.size() == original.size());
        for (std::size_t i = 0; i < current.size(); ++i)
        {
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
    QStringList packed;
    for (const auto& reference : index.references)
    {
        require(reference.resolution == TrackerReferenceResolution::Resolved ||
                reference.resolution == TrackerReferenceResolution::NoTarget);
        require(reference.kind == TrackerReferenceKind::SelectedTrack || reference.kind == TrackerReferenceKind::PlotSelection ||
                reference.kind == TrackerReferenceKind::TableSelection || reference.kind == TrackerReferenceKind::TableColumnOwner ||
                reference.kind == TrackerReferenceKind::TableFormatOwner);
        if (reference.resolution == TrackerReferenceResolution::NoTarget)
            continue;
        require(reference.candidateSourceIndices.size() == 1);
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
