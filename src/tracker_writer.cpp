#include "tracker_writer.hpp"

#include <QByteArray>
#include <QDomDocument>
#include <QSaveFile>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
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

void patchDouble(QDomDocument& xml, QDomElement object, const QString& name, double value)
{
    auto mass = uniqueProperty(object, name);
    if (mass.isNull())
    {
        mass = xml.createElement("property");
        mass.setAttribute("name", name);
        mass.setAttribute("type", "double");
        object.appendChild(mass);
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

QDomElement arrayProperty(QDomDocument& xml, QDomElement object, const QString& name, const QString& type)
{
    auto array = uniqueProperty(object, name);
    if (array.isNull())
    {
        array = xml.createElement("property");
        array.setAttribute("name", name);
        array.setAttribute("type", "array");
        object.appendChild(array);
    }
    require(array.attribute("type") == "array" && (array.attribute("class").isEmpty() || array.attribute("class") == type));
    array.setAttribute("class", type);
    return array;
}

int frameIndex(const QString& name)
{
    require(name.startsWith('[') && name.endsWith(']'));
    bool valid = false;
    const auto frame = name.mid(1, name.size() - 2).toInt(&valid);
    require(valid && frame >= 0);
    return frame;
}

std::set<int> readKeys(const QDomElement& object, const std::vector<Mark>& original)
{
    std::set<int> result;
    const auto keys = uniqueProperty(object, "keyFrames");
    if (!keys.isNull())
    {
        require(keys.attribute("type") == "array" && keys.attribute("class") == "[I");
        const auto array = uniqueProperty(keys, "array");
        require(!array.isNull() && array.attribute("type") == "string" && array.firstChildElement().isNull());
        const auto text = array.text().trimmed();
        require(text.startsWith('{') && text.endsWith('}'));
        const auto values = text.mid(1, text.size() - 2).trimmed();
        if (!values.isEmpty())
        {
            for (const auto& value : values.split(','))
            {
                bool valid = false;
                const auto frame = value.trimmed().toInt(&valid);
                require(valid && frame >= 0);
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

void patchMarks(QDomDocument& xml, const QDomElement& object, const std::vector<Mark>& original, const std::vector<Mark>& current)
{
    std::map<int, const Mark*> marks;
    int previous = -1;
    for (const auto& mark : current)
    {
        require(mark.frame > previous && std::isfinite(mark.x) && std::isfinite(mark.y));
        marks.emplace(mark.frame, &mark);
        previous = mark.frame;
    }
    auto keys = readKeys(object, original);
    const auto oldKeys = keys;
    const auto data = arrayProperty(xml, object, "framedata", "[Lorg.opensourcephysics.cabrillo.tracker.PointMass$FrameData;");
    std::map<int, QDomElement> entries;
    int lastEntry = -1;
    for (auto item = data.firstChildElement("property"); !item.isNull(); item = item.nextSiblingElement("property"))
    {
        const auto frame = frameIndex(item.attribute("name"));
        require(frame > lastEntry && entries.emplace(frame, item).second);
        lastEntry = frame;
    }
    for (const auto& mark : original)
    {
        const auto found = marks.find(mark.frame);
        if (found == marks.end())
        {
            auto parent = data;
            parent.removeChild(entries.at(mark.frame));
            entries.erase(mark.frame);
            keys.erase(mark.frame);
        }
        else if (*found->second == mark)
        {
            marks.erase(found); // Keep untouched frame objects and their unknown properties.
        }
    }
    for (const auto& [frame, mark] : marks)
    {
        auto item = entries.contains(frame) ? entries.at(frame) : QDomElement{};
        if (item.isNull())
        {
            item = xml.createElement("property");
            item.setAttribute("name", QString("[%1]").arg(frame));
            item.setAttribute("type", "object");
            auto parent = data;
            const auto next = entries.upper_bound(frame);
            if (next == entries.end())
            {
                parent.appendChild(item);
            }
            else
            {
                parent.insertBefore(item, next->second);
            }
            entries.emplace(frame, item);
        }
        require(item.attribute("type") == "object");
        auto step = item.firstChildElement("object");
        if (step.isNull())
        {
            require(item.firstChildElement().isNull());
            // A null array placeholder may contain text; replace only that known scalar representation.
            for (auto node = item.firstChild(); !node.isNull();)
            {
                const auto next = node.nextSibling();
                if (node.isText() || node.isCDATASection())
                {
                    item.removeChild(node);
                }
                node = next;
            }
            item.setAttribute("type", "object");
            step = xml.createElement("object");
            item.appendChild(step);
        }
        require(
            step.nextSiblingElement("object").isNull() &&
            (step.attribute("class").isEmpty() || step.attribute("class") == "org.opensourcephysics.cabrillo.tracker.PointMass$FrameData"));
        step.setAttribute("class", "org.opensourcephysics.cabrillo.tracker.PointMass$FrameData");
        patchDouble(xml, step, "x", mark->x);
        patchDouble(xml, step, "y", mark->y);
        keys.insert(frame);
    }
    // OSP's array reader consumes consecutive property lines. Deleting an entry can leave a blank
    // line in preserved formatting that makes it stop early; remove only XML indentation here.
    auto array = data;
    for (auto node = array.firstChild(); !node.isNull();)
    {
        const auto next = node.nextSibling();
        if (node.isText() &&
            std::ranges::all_of(node.nodeValue(),
                                [](QChar value) { return value == ' ' || value == '\t' || value == '\r' || value == '\n'; }))
        {
            array.removeChild(node);
        }
        node = next;
    }
    // An empty key list makes Tracker promote every remaining automatic mark to a manual key.
    // Until that distinction is in the editable model, reject edits that cannot retain it.
    require(!keys.empty() || current.empty());
    if (keys != oldKeys)
    {
        auto container = arrayProperty(xml, object, "keyFrames", "[I");
        auto values = uniqueProperty(container, "array");
        if (values.isNull())
        {
            values = xml.createElement("property");
            values.setAttribute("name", "array");
            values.setAttribute("type", "string");
            container.appendChild(values);
        }
        for (auto node = values.firstChild(); !node.isNull();)
        {
            const auto next = node.nextSibling();
            if (node.isText() || node.isCDATASection())
            {
                values.removeChild(node);
            }
            node = next;
        }
        QStringList text;
        for (const auto key : keys)
        {
            text.append(QString::number(key));
        }
        values.appendChild(xml.createTextNode('{' + text.join(',') + '}'));
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
            patchDouble(xml, objects.at(current.id), "mass", current.mass);
            original.mass = current.mass;
            changed = true;
        }
        if (current.marks != original.marks)
        {
            patchMarks(xml, objects.at(current.id), original.marks, current.marks);
            original.marks = current.marks;
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
    auto output = xml.toByteArray(0).toStdString();
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
