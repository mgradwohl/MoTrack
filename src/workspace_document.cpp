#include "workspace_document.hpp"

#include "local_media_path.hpp"
#include "track_identity.hpp"
#include "tracker_archive.hpp"
#include "workspace_storage.hpp"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace motrack
{
namespace
{

constexpr int DOCUMENT_VERSION = 5;

void require(bool valid)
{
    if (!valid)
    {
        throw ProjectError("Invalid MoTrack workspace.");
    }
}

double number(const QJsonValue& value)
{
    const auto result = value.toDouble(std::numeric_limits<double>::quiet_NaN());
    require(std::isfinite(result));
    return result;
}

int integer(const QJsonValue& value, int minimum)
{
    const auto result = value.toInt(std::numeric_limits<int>::min());
    require(result >= minimum);
    return result;
}

std::string string(const QJsonValue& value)
{
    require(value.isString());
    return value.toString().toStdString();
}

TrackId identity(const QJsonValue& value)
{
    const auto text = string(value);
    TrackId result = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && result > 0 && std::to_string(result) == text);
    return result;
}

QJsonArray array(const QJsonValue& value, qsizetype size)
{
    require(value.isArray());
    auto result = value.toArray();
    require(size < 0 || result.size() == size);
    return result;
}

std::vector<std::string> strings(const QJsonValue& value)
{
    const auto input = array(value, -1);
    std::vector<std::string> result;
    result.reserve(static_cast<std::size_t>(input.size()));
    for (const auto& entry : input)
    {
        result.push_back(string(entry));
    }
    return result;
}

QJsonArray writeStrings(const std::vector<std::string>& values)
{
    QJsonArray result;
    for (const auto& value : values)
    {
        result.append(QString::fromStdString(value));
    }
    return result;
}

std::optional<std::vector<TrackSourceBinding>> readBindings(const QJsonValue& value)
{
    if (value.isNull())
    {
        return std::nullopt;
    }
    std::vector<TrackSourceBinding> result;
    for (const auto& entry : array(value, -1))
    {
        require(entry.isObject());
        const auto binding = entry.toObject();
        const auto kind = string(binding.value("kind"));
        require(kind == "pointMass" || kind == "vector");
        result.push_back({.trackId = identity(binding.value("trackId")),
                          .sourceIndex = static_cast<std::size_t>(integer(binding.value("sourceIndex"), 0)),
                          .kind = kind == "pointMass" ? TrackKind::PointMass : TrackKind::Vector});
    }
    return result;
}

QJsonValue writeBindings(const std::optional<std::vector<TrackSourceBinding>>& bindings)
{
    if (!bindings)
    {
        return QJsonValue::Null;
    }
    QJsonArray result;
    for (const auto& binding : *bindings)
    {
        require(std::in_range<int>(binding.sourceIndex));
        result.append(QJsonObject{{"trackId", QString::fromStdString(std::to_string(binding.trackId))},
                                  {"sourceIndex", static_cast<int>(binding.sourceIndex)},
                                  {"kind", binding.kind == TrackKind::PointMass ? "pointMass" : "vector"}});
    }
    return result;
}

std::optional<TrackerSource> readTrackerSource(const QJsonValue& value, int version)
{
    if (value.isNull())
    {
        return std::nullopt;
    }
    require(value.isObject());
    const auto object = value.toObject();
    const auto encoded = object.value("xmlBase64");
    require(encoded.isString());
    const auto text = encoded.toString();
    constexpr auto MAX_ENCODED_SIZE = (static_cast<qsizetype>(MAX_TRACKER_XML_BYTES) + 2) / 3 * 4;
    require(text.size() <= MAX_ENCODED_SIZE);
    const auto bytes = QByteArray::fromBase64Encoding(text.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    require(static_cast<bool>(bytes));
    // Require the canonical ASCII encoding: rejects ignored characters and non-Latin text substitutions.
    require(QString::fromLatin1(bytes.decoded.toBase64()) == text);
    TrackerSource result{.xml = bytes.decoded.toStdString()};
    if (version >= 4)
    {
        result.bindings = readBindings(object.value("bindings"));
    }
    else
    {
        require(!object.contains("bindings"));
    }
    return result;
}

QJsonValue writeTrackerSource(const std::optional<TrackerSource>& source, const LegacyProject& project)
{
    if (!source)
    {
        return QJsonValue::Null;
    }
    validateTrackerSource(*source, project);
    const auto bytes = QByteArray::fromRawData(source->xml.data(), static_cast<qsizetype>(source->xml.size()));
    return QJsonObject{{"xmlBase64", QString::fromLatin1(bytes.toBase64())}, {"bindings", writeBindings(source->bindings)}};
}

QJsonValue writeCalibration(const std::optional<Calibration>& calibration)
{
    if (!calibration)
    {
        return QJsonValue::Null;
    }
    QJsonArray keys;
    for (const auto& key : calibration->keyframes)
    {
        keys.append(QJsonArray{key.frame, key.originX, key.originY, key.angleDegrees, key.scaleX, key.scaleY});
    }
    return QJsonObject{{"fixedOrigin", calibration->fixedOrigin},
                       {"fixedAngle", calibration->fixedAngle},
                       {"fixedScale", calibration->fixedScale},
                       {"keys", keys}};
}

std::optional<Calibration> readCalibration(const QJsonValue& value)
{
    if (value.isNull())
    {
        return std::nullopt;
    }
    const auto input = value.toObject();
    const auto keys = array(input.value("keys"), -1);
    Calibration result{.keyframes = {},
                       .fixedOrigin = input.value("fixedOrigin").toBool(),
                       .fixedAngle = input.value("fixedAngle").toBool(),
                       .fixedScale = input.value("fixedScale").toBool()};
    for (const auto& entry : keys)
    {
        const auto key = array(entry, 6);
        const int frame = integer(key.at(0), 0);
        require(result.keyframes.empty() || result.keyframes.back().frame < frame);
        result.keyframes.push_back({.frame = frame,
                                    .originX = number(key.at(1)),
                                    .originY = number(key.at(2)),
                                    .angleDegrees = number(key.at(3)),
                                    .scaleX = number(key.at(4)),
                                    .scaleY = number(key.at(5))});
    }
    return result;
}

QJsonArray writeTracks(const std::vector<PointMass>& tracks)
{
    QJsonArray result;
    for (const auto& track : tracks)
    {
        QJsonArray marks;
        for (const auto& mark : track.marks)
        {
            marks.append(QJsonArray{mark.frame, mark.x, mark.y});
        }
        result.append(QJsonObject{{"name", QString::fromStdString(track.name)},
                                  {"marks", marks},
                                  {"mass", track.mass},
                                  {"id", QString::fromStdString(std::to_string(track.id))}});
    }
    return result;
}

std::vector<PointMass> readTracks(const QJsonValue& value, int version)
{
    const auto tracks = array(value, -1);
    std::vector<PointMass> result;
    for (const auto& entry : tracks)
    {
        const auto track = entry.toObject();
        PointMass mass{.name = string(track.value("name")), .marks = {}};
        if (version >= 3)
        {
            mass.id = identity(track.value("id"));
        }
        else
        {
            require(!track.contains("id"));
        }
        if (const auto savedMass = track.value("mass"); !savedMass.isUndefined())
        {
            mass.mass = number(savedMass);
            require(mass.mass >= MIN_POINT_MASS);
        }
        const auto marks = array(track.value("marks"), -1);
        for (const auto& item : marks)
        {
            const auto mark = array(item, 3);
            const auto frame = integer(mark.at(0), 0);
            require(mass.marks.empty() || mass.marks.back().frame < frame);
            mass.marks.push_back({.frame = frame, .x = number(mark.at(1)), .y = number(mark.at(2))});
        }
        result.push_back(std::move(mass));
    }
    return result;
}

QJsonArray writeVectors(const std::vector<VectorTrack>& tracks)
{
    QJsonArray result;
    for (const auto& track : tracks)
    {
        QJsonArray samples;
        for (const auto& sample : track.samples)
        {
            samples.append(QJsonArray{sample.frame, sample.tailX, sample.tailY, sample.componentX, sample.componentY});
        }
        result.append(QJsonObject{
            {"name", QString::fromStdString(track.name)}, {"samples", samples}, {"id", QString::fromStdString(std::to_string(track.id))}});
    }
    return result;
}

std::vector<VectorTrack> readVectors(const QJsonValue& value, int version)
{
    std::vector<VectorTrack> result;
    for (const auto& entry : array(value, -1))
    {
        const auto track = entry.toObject();
        VectorTrack vector{.name = string(track.value("name")), .samples = {}};
        if (version >= 3)
        {
            vector.id = identity(track.value("id"));
        }
        else
        {
            require(!track.contains("id"));
        }
        for (const auto& item : array(track.value("samples"), -1))
        {
            const auto sample = array(item, 5);
            const auto frame = integer(sample.at(0), 0);
            require(vector.samples.empty() || vector.samples.back().frame < frame);
            vector.samples.push_back({.frame = frame,
                                      .tailX = number(sample.at(1)),
                                      .tailY = number(sample.at(2)),
                                      .componentX = number(sample.at(3)),
                                      .componentY = number(sample.at(4))});
        }
        result.push_back(std::move(vector));
    }
    return result;
}

void validateArchive(const WorkspaceDocument& document)
{
    require(!document.archivePayload || document.archiveReference.empty());
    if (!document.archiveMember)
    {
        return;
    }
    require(document.archivePayload && !document.archiveMember->empty());
    const auto member = QString::fromStdString(*document.archiveMember);
    const auto path = document.archivePayload->projectPath(member);
    if (document.trackerSource)
    {
        QFile original(path);
        require(original.open(QIODevice::ReadOnly) && original.size() <= static_cast<qint64>(MAX_TRACKER_XML_BYTES));
        require(original.read(static_cast<qint64>(MAX_TRACKER_XML_BYTES) + 1).toStdString() == document.trackerSource->xml);
    }
}

} // namespace

void retainWorkspaceMediaLocations(WorkspaceDocument& document, const QString& sourcePath)
{
    document.mediaLocations.clear();
    if (document.archivePayload || document.archiveMember || !document.archiveReference.empty())
        return;
    for (const auto& reference : document.project.clip.mediaReferences)
        document.mediaLocations.insert(QString::fromStdString(reference), resolveLocalMediaPath(sourcePath, reference));
}

QString workspaceMediaPath(const WorkspaceDocument& document, const QString& sourcePath, const std::string& reference)
{
    const auto retained = document.mediaLocations.constFind(QString::fromStdString(reference));
    return retained == document.mediaLocations.cend() ? resolveLocalMediaPath(sourcePath, reference) : retained.value();
}

WorkspaceDocument importTrackerDocument(const QString& path)
{
    auto imported = readTrackerProject(std::filesystem::path(path.toStdU16String()));
    return {.project = std::move(imported.project), .movieFrameTimes = {}, .trackerSource = std::move(imported.source)};
}

WorkspaceDocument readWorkspaceDocument(const QString& path, const std::atomic_bool* cancelled)
{
    auto storage = readWorkspaceStorage(path, cancelled);
    const auto input = QJsonDocument::fromJson(storage.json).object();
    const auto version = input.value("version").toInt();
    require(input.value("format") == "MoTrack" && version >= 1 && version <= DOCUMENT_VERSION);
    WorkspaceDocument result;
    if (version >= 5)
    {
        const auto embedded = input.value("embeddedArchive");
        require(embedded.isBool() && embedded.toBool() == static_cast<bool>(storage.archive));
        result.archivePayload = std::move(storage.archive);
        const auto member = input.value("archiveMember");
        if (!member.isNull())
        {
            result.archiveMember = string(member);
        }
    }
    else
    {
        require(!storage.archive && !input.contains("embeddedArchive") && !input.contains("archiveMember"));
    }
    if (version >= 2)
    {
        result.trackerSource = readTrackerSource(input.value("trackerSource"), version);
    }
    else
    {
        // Version 1 did not preserve source XML. Never imply its lost content can be recovered.
        require(!input.contains("trackerSource"));
    }
    require(input.value("movie").isBool());
    result.movie = input.value("movie").toBool();
    const auto orientation = input.value("displayOrientation");
    require(orientation.isUndefined() || orientation.isBool());
    result.useDisplayOrientation = orientation.toBool();
    const auto alignment = input.value("alignMovieToClip");
    require(alignment.isUndefined() || alignment.isBool());
    result.alignMovieToClip = alignment.toBool();
    const auto archive = input.value("archive");
    if (!archive.isUndefined())
    {
        result.archiveReference = string(archive);
    }
    auto& project = result.project;
    project.version = string(input.value("trackerVersion"));
    project.description = string(input.value("description"));
    project.lengthUnit = string(input.value("lengthUnit"));
    if (const auto unit = input.value("massUnit"); !unit.isUndefined())
    {
        project.massUnit = string(unit);
    }
    if (const auto unit = input.value("timeUnit"); !unit.isUndefined())
    {
        project.timeUnit = string(unit);
    }
    else if (result.trackerSource)
    {
        // Earlier native files retained this label only in the original Tracker XML.
        project.timeUnit = parseLegacyProject(result.trackerSource->xml).timeUnit;
    }
    project.unsupportedTrackTypes = strings(input.value("unsupportedTrackTypes"));
    project.calibration = readCalibration(input.value("calibration"));
    project.pointMasses = readTracks(input.value("pointMasses"), version);
    if (const auto vectors = input.value("vectors"); version >= 3 || !vectors.isUndefined())
    {
        project.vectors = readVectors(vectors, version);
    }
    if (version >= 3)
    {
        project.nextTrackId = identity(input.value("nextTrackId"));
    }
    else
    {
        require(!input.contains("nextTrackId"));
        for (auto& track : project.pointMasses)
        {
            track.id = allocateTrackId(project);
        }
        for (auto& track : project.vectors)
        {
            track.id = allocateTrackId(project);
        }
    }
    validateTrackIdentities(project);
    if (result.trackerSource)
    {
        validateTrackerSource(*result.trackerSource, project);
    }
    const auto clip = input.value("clip").toObject();
    project.clip.firstFrame = integer(clip.value("firstFrame"), 0);
    project.clip.stepSize = integer(clip.value("stepSize"), 1);
    project.clip.mediaReferences = strings(clip.value("media"));
    project.clip.startTimeMs = number(clip.value("startTimeMs"));
    if (!clip.value("stepCount").isNull())
    {
        project.clip.stepCount = integer(clip.value("stepCount"), 0);
    }
    if (!clip.value("frameDurationMs").isNull())
    {
        project.clip.frameDurationMs = number(clip.value("frameDurationMs"));
        require(project.clip.frameDurationMs.value() > 0.0);
    }
    const auto sourceCount = clip.value("sourceFrameCount");
    if (!sourceCount.isNull() && !sourceCount.isUndefined())
    {
        project.clip.sourceFrameCount = integer(sourceCount, 1);
        const auto steps = project.clip.stepCount.value_or(1);
        const auto last = project.clip.firstFrame + static_cast<std::int64_t>(steps - 1) * project.clip.stepSize;
        require(steps == 0 || last < project.clip.sourceFrameCount.value());
    }
    const auto frameTimes = array(input.value("movieFrameTimes"), -1);
    for (const auto& entry : frameTimes)
    {
        const auto time = number(entry);
        require(result.movieFrameTimes.empty() ? time == 0.0 : time > result.movieFrameTimes.back());
        result.movieFrameTimes.push_back(time);
    }
    validateArchive(result);
    return result;
}

void rebaseWorkspaceMedia(WorkspaceDocument& document, const QString& sourcePath, const QString& destinationPath)
{
    if (!document.archivePayload)
    {
        const auto directory = localProjectDirectory(destinationPath);
        if (directory.isEmpty())
        {
            throw ProjectError("Could not resolve the native project destination.");
        }
        const QDir destination(directory);
        const auto existingTarget = QFileInfo(destinationPath).canonicalFilePath();
        const auto target = existingTarget.isEmpty() ? destination.filePath(QFileInfo(destinationPath).fileName()) : existingTarget;
        for (auto& reference : document.project.clip.mediaReferences)
        {
            const auto source = workspaceMediaPath(document, sourcePath, reference);
            if (source.isEmpty())
                throw ProjectError("Could not resolve a native project media reference.");
            if (sameLocalFile(source, target))
                throw ProjectError("Native saving cannot overwrite linked media.");
            const auto rebased = destination.relativeFilePath(source).toStdString();
            const auto resolved = resolveLocalMediaPath(destinationPath, rebased);
            if (resolved.isEmpty() || QDir::cleanPath(resolved) != QDir::cleanPath(source))
            {
                throw ProjectError("Could not preserve a native project media reference.");
            }
            reference = rebased;
        }
    }
}

void writeWorkspaceDocument(const QString& path, const WorkspaceDocument& document)
{
    const auto& project = document.project;
    validateTrackIdentities(project);
    validateArchive(document);
    const auto& clip = project.clip;
    QJsonArray frameTimes;
    for (const auto time : document.movieFrameTimes)
    {
        frameTimes.append(time);
    }
    const QJsonObject clipObject{{"firstFrame", clip.firstFrame},
                                 {"stepSize", clip.stepSize},
                                 {"stepCount", clip.stepCount ? QJsonValue(*clip.stepCount) : QJsonValue::Null},
                                 {"media", writeStrings(clip.mediaReferences)},
                                 {"frameDurationMs", clip.frameDurationMs ? QJsonValue(*clip.frameDurationMs) : QJsonValue::Null},
                                 {"sourceFrameCount", clip.sourceFrameCount ? QJsonValue(*clip.sourceFrameCount) : QJsonValue::Null},
                                 {"startTimeMs", clip.startTimeMs}};
    const QJsonObject output{
        {"format", "MoTrack"},
        {"version", DOCUMENT_VERSION},
        {"nextTrackId", QString::fromStdString(std::to_string(project.nextTrackId))},
        {"trackerSource", writeTrackerSource(document.trackerSource, project)},
        {"trackerVersion", QString::fromStdString(project.version)},
        {"description", QString::fromStdString(project.description)},
        {"lengthUnit", QString::fromStdString(project.lengthUnit)},
        {"massUnit", QString::fromStdString(project.massUnit)},
        {"timeUnit", QString::fromStdString(project.timeUnit)},
        {"unsupportedTrackTypes", writeStrings(project.unsupportedTrackTypes)},
        {"calibration", writeCalibration(project.calibration)},
        {"pointMasses", writeTracks(project.pointMasses)},
        {"vectors", writeVectors(project.vectors)},
        {"movie", document.movie},
        {"displayOrientation", document.useDisplayOrientation},
        {"alignMovieToClip", document.alignMovieToClip},
        {"archive", QString::fromStdString(document.archiveReference)},
        {"embeddedArchive", static_cast<bool>(document.archivePayload)},
        {"archiveMember", document.archiveMember ? QJsonValue(QString::fromStdString(*document.archiveMember)) : QJsonValue::Null},
        {"movieFrameTimes", frameTimes},
        {"clip", clipObject}};
    const auto bytes = QJsonDocument(output).toJson(QJsonDocument::Compact);
    writeWorkspaceStorage(path, bytes, document.archivePayload);
}

} // namespace motrack
