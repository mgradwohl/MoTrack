#include "tracker_writer.hpp"

#include <QByteArray>
#include <QDomDocument>
#include <QSaveFile>

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace motrack
{
namespace
{

void require(bool valid)
{
    if (!valid)
    {
        throw ProjectError("This Tracker export cannot preserve all edits or source associations.");
    }
}

QDomElement uniqueProperty(const QDomElement& object, const QString& name)
{
    QDomElement result;
    for (auto child = object.firstChildElement("property"); !child.isNull(); child = child.nextSiblingElement("property"))
    {
        if (child.attribute("name") == name)
        {
            require(result.isNull());
            result = child;
        }
    }
    return result;
}

void patchMass(QDomDocument& xml, QDomElement object, double value)
{
    auto mass = uniqueProperty(object, "mass");
    if (mass.isNull())
    {
        mass = xml.createElement("property");
        mass.setAttribute("name", "mass");
        mass.setAttribute("type", "double");
        // Added fields need their own lines for OSP's reader; leave every existing text node alone.
        object.appendChild(xml.createTextNode("\n"));
        object.appendChild(mass);
        object.appendChild(xml.createTextNode("\n"));
    }
    require(mass.attribute("type") == "double" && mass.firstChildElement().isNull());
    // Preserve comments and processing instructions within the scalar property.
    bool written = false;
    for (auto child = mass.firstChild(); !child.isNull();)
    {
        const auto next = child.nextSibling();
        if (child.isText() || child.isCDATASection())
        {
            if (!written)
            {
                mass.replaceChild(xml.createTextNode(QString::number(value, 'g', 17)), child);
                written = true;
            }
            else
            {
                mass.removeChild(child);
            }
        }
        child = next;
    }
    if (!written)
    {
        mass.appendChild(xml.createTextNode(QString::number(value, 'g', 17)));
    }
}

void sortTracks(LegacyProject& project)
{
    std::ranges::sort(project.pointMasses, {}, &PointMass::id);
    std::ranges::sort(project.vectors, {}, &VectorTrack::id);
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

} // namespace

std::string renderTrackerProject(const LegacyProject& project, const TrackerSource& source)
{
    require(project.clip.mediaReferences.empty()); // Relocation/packaging is not implemented by this initial writer.
    validateTrackerSource(source, project);
    auto baseline = parseTrackerProject(source.xml);
    if (!source.bindings || !baseline.source.bindings)
    {
        throw ProjectError("Tracker export requires original object associations.");
    }
    std::unordered_map<std::size_t, TrackId> sourceIds;
    for (const auto& binding : *source.bindings)
    {
        sourceIds.emplace(binding.sourceIndex, binding.trackId);
    }
    std::unordered_map<TrackId, TrackId> mapping;
    for (const auto& binding : *baseline.source.bindings)
    {
        mapping.emplace(binding.trackId, sourceIds.at(binding.sourceIndex));
    }
    remapIds(baseline.project, mapping, project.nextTrackId);
    auto expected = project;
    sortTracks(expected);
    require(expected.pointMasses.size() == baseline.project.pointMasses.size());

    QDomDocument xml;
    require(static_cast<bool>(xml.setContent(QByteArray::fromStdString(source.xml), QDomDocument::ParseOption::PreserveSpacingOnlyNodes)));
    const auto tracks = uniqueProperty(xml.documentElement(), "tracks");
    std::unordered_map<TrackId, QDomElement> objects;
    std::size_t position = 0;
    for (auto item = tracks.firstChildElement("property"); !item.isNull(); item = item.nextSiblingElement("property"), ++position)
    {
        if (const auto found = sourceIds.find(position); found != sourceIds.end())
        {
            objects.emplace(found->second, item.firstChildElement("object"));
        }
    }
    bool changed = false;
    for (std::size_t index = 0; index < expected.pointMasses.size(); ++index)
    {
        const auto& current = expected.pointMasses[index];
        auto& original = baseline.project.pointMasses[index];
        require(current.id == original.id && std::isfinite(current.mass) && current.mass >= MIN_POINT_MASS);
        if (current.mass != original.mass)
        {
            patchMass(xml, objects.at(current.id), current.mass);
            original.mass = current.mass;
            changed = true;
        }
    }
    // Never produce a superficially valid file that silently omits an unsupported edit.
    require(baseline.project == expected);
    if (!changed)
    {
        return source.xml;
    }
    // QDom emits UTF-8 bytes; do not retain an input declaration for a different encoding.
    for (auto node = xml.firstChild(); !node.isNull(); node = node.nextSibling())
    {
        if (node.isProcessingInstruction() && node.nodeName() == "xml")
        {
            node.setNodeValue(R"(version="1.0" encoding="UTF-8")");
        }
    }
    // A nonnegative indent inserts newlines even inside untouched scalar/mixed/xml:space content.
    auto output = xml.toByteArray(-1).toStdString();
    require(output.size() <= MAX_TRACKER_XML_BYTES);
    auto reparsed = parseLegacyProject(output);
    remapIds(reparsed, mapping, project.nextTrackId);
    require(reparsed == expected);
    return output;
}

void writeTrackerProject(const QString& path, const LegacyProject& project, const TrackerSource& source)
{
    require(path.endsWith(".trk", Qt::CaseInsensitive));
    const auto xml = renderTrackerProject(project, source);
    QSaveFile output(path);
    const auto size = static_cast<qint64>(xml.size());
    require(output.open(QIODevice::WriteOnly) && output.write(xml.data(), size) == size && output.commit());
}

} // namespace motrack
