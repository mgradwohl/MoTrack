#include "legacy_project.hpp"

#include <QByteArray>
#include <QDomDocument>

#include <algorithm>
#include <cmath>
#include <fstream>

namespace motrack {
namespace {

constexpr std::size_t maxProjectBytes = 32 * 1024 * 1024;
constexpr auto trackerPanelClass = "org.opensourcephysics.cabrillo.tracker.TrackerPanel";
constexpr auto pointMassClass = "org.opensourcephysics.cabrillo.tracker.PointMass";

QDomElement property(const QDomElement& parent, const QString& name)
{
    for (auto node = parent.firstChildElement("property"); !node.isNull();
         node = node.nextSiblingElement("property")) {
        if (node.attribute("name") == name) {
            return node;
        }
    }
    return {};
}

std::string value(const QDomElement& parent, const QString& name)
{
    return property(parent, name).text().toStdString();
}

int integer(const QString& text, int minimum)
{
    bool ok = false;
    const auto number = text.toInt(&ok);
    if (!ok || number < minimum) {
        throw ProjectError("Invalid project integer.");
    }
    return number;
}

int arrayIndex(const QDomElement& entry)
{
    const auto name = entry.attribute("name");
    if (!name.startsWith('[') || !name.endsWith(']')) {
        throw ProjectError("Invalid Tracker array entry.");
    }
    return integer(name.sliced(1, name.size() - 2), 0);
}

double coordinate(const QDomElement& object, const QString& axis)
{
    bool ok = false;
    const auto number = property(object, axis).text().toDouble(&ok);
    if (!ok || !std::isfinite(number)) {
        throw ProjectError("Invalid point mass coordinate.");
    }
    return number;
}

Clip readClip(const QDomElement& panel)
{
    Clip clip;
    const auto object = property(panel, "videoclip").firstChildElement("object");
    const auto start = property(object, "startframe");
    const auto size = property(object, "stepsize");
    const auto count = property(object, "stepcount");
    if (!start.isNull()) {
        clip.firstFrame = integer(start.text(), 0);
    }
    if (!size.isNull()) {
        clip.stepSize = integer(size.text(), 1);
    }
    if (!count.isNull()) {
        clip.stepCount = integer(count.text(), 0);
    }
    const auto video = property(object, "video").firstChildElement("object");
    const auto paths = property(video, "paths");
    std::vector<std::pair<int, std::string>> indexedPaths;
    for (auto entry = paths.firstChildElement("property"); !entry.isNull();
         entry = entry.nextSiblingElement("property")) {
        indexedPaths.emplace_back(arrayIndex(entry), entry.text().toStdString());
    }
    std::ranges::sort(indexedPaths, {}, &std::pair<int, std::string>::first);
    for (const auto& [index, path] : indexedPaths) {
        if (static_cast<std::size_t>(index) != clip.mediaReferences.size() || path.empty()) {
            throw ProjectError("Invalid Tracker image sequence.");
        }
        clip.mediaReferences.push_back(path);
    }
    if (const auto path = value(video, "path"); clip.mediaReferences.empty() && !path.empty()) {
        clip.mediaReferences.push_back(path);
    }
    return clip;
}

PointMass readPointMass(const QDomElement& object)
{
    PointMass track{value(object, "name"), {}};
    const auto data = property(object, "framedata");
    for (auto entry = data.firstChildElement("property"); !entry.isNull();
         entry = entry.nextSiblingElement("property")) {
        const auto frame = entry.firstChildElement("object");
        if (!frame.isNull()) {
            track.marks.push_back({arrayIndex(entry), coordinate(frame, "x"), coordinate(frame, "y")});
        }
    }
    std::ranges::sort(track.marks, {}, &Mark::frame);
    if (std::ranges::adjacent_find(track.marks, {}, &Mark::frame) != track.marks.end()) {
        throw ProjectError("Duplicate Tracker point mass frame.");
    }
    return track;
}

} // namespace

LegacyProject parseLegacyProject(std::string_view xml)
{
    if (xml.size() > maxProjectBytes) {
        throw ProjectError("Tracker project is too large.");
    }
    QDomDocument document;
    if (!document.setContent(QByteArray::fromRawData(xml.data(), static_cast<qsizetype>(xml.size())),
                             QDomDocument::ParseOption::Default)) {
        throw ProjectError("Invalid Tracker XML.");
    }
    if (!document.doctype().name().isEmpty()) {
        throw ProjectError("Tracker XML must not contain a document type.");
    }
    const auto panel = document.documentElement();
    if (panel.tagName() != "object" || panel.attribute("class") != trackerPanelClass) {
        throw ProjectError("Expected a TrackerPanel project.");
    }
    LegacyProject project{value(panel, "semantic_version"), value(panel, "description"),
                          readClip(panel), {}, {}};
    const auto tracks = property(panel, "tracks");
    for (auto item = tracks.firstChildElement("property"); !item.isNull();
         item = item.nextSiblingElement("property")) {
        const auto object = item.firstChildElement("object");
        const auto type = object.attribute("class");
        if (type == pointMassClass) {
            project.pointMasses.push_back(readPointMass(object));
        } else if (!type.isEmpty()) {
            project.unsupportedTrackTypes.push_back(type.toStdString());
        }
    }
    return project;
}

LegacyProject readLegacyProject(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    const auto length = file.tellg();
    if (length < 0) {
        throw ProjectError("Could not open the Tracker project.");
    }
    if (length > static_cast<std::streamoff>(maxProjectBytes)) {
        throw ProjectError("Tracker project is too large.");
    }
    std::string xml(static_cast<std::size_t>(length), '\0');
    file.seekg(0);
    if (!file.read(xml.data(), static_cast<std::streamsize>(length))) {
        throw ProjectError("Could not read the Tracker project.");
    }
    return parseLegacyProject(xml);
}

} // namespace motrack
