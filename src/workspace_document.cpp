#include "workspace_document.hpp"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

namespace motrack
{
namespace
{

constexpr qint64 MAX_DOCUMENT_BYTES = qint64{128} * 1024 * 1024;
constexpr int DOCUMENT_VERSION = 1;

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
        result.append(QJsonObject{{"name", QString::fromStdString(track.name)}, {"marks", marks}});
    }
    return result;
}

std::vector<PointMass> readTracks(const QJsonValue& value)
{
    const auto tracks = array(value, -1);
    std::vector<PointMass> result;
    for (const auto& entry : tracks)
    {
        const auto track = entry.toObject();
        PointMass mass{.name = string(track.value("name")), .marks = {}};
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

} // namespace

WorkspaceDocument readWorkspaceDocument(const QString& path)
{
    QFile file(path);
    require(file.open(QIODevice::ReadOnly) && file.size() <= MAX_DOCUMENT_BYTES);
    const auto input = QJsonDocument::fromJson(file.read(MAX_DOCUMENT_BYTES + 1)).object();
    require(input.value("format") == "MoTrack" && input.value("version").toInt() == DOCUMENT_VERSION);
    WorkspaceDocument result;
    require(input.value("movie").isBool());
    result.movie = input.value("movie").toBool();
    const auto orientation = input.value("displayOrientation");
    require(orientation.isUndefined() || orientation.isBool());
    result.useDisplayOrientation = orientation.toBool();
    const auto archive = input.value("archive");
    if (!archive.isUndefined())
    {
        result.archiveReference = string(archive);
    }
    auto& project = result.project;
    project.version = string(input.value("trackerVersion"));
    project.description = string(input.value("description"));
    project.lengthUnit = string(input.value("lengthUnit"));
    project.unsupportedTrackTypes = strings(input.value("unsupportedTrackTypes"));
    project.calibration = readCalibration(input.value("calibration"));
    project.pointMasses = readTracks(input.value("pointMasses"));
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
    return result;
}

void writeWorkspaceDocument(const QString& path, const WorkspaceDocument& document)
{
    const auto& project = document.project;
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
    const QJsonObject output{{"format", "MoTrack"},
                             {"version", DOCUMENT_VERSION},
                             {"trackerVersion", QString::fromStdString(project.version)},
                             {"description", QString::fromStdString(project.description)},
                             {"lengthUnit", QString::fromStdString(project.lengthUnit)},
                             {"unsupportedTrackTypes", writeStrings(project.unsupportedTrackTypes)},
                             {"calibration", writeCalibration(project.calibration)},
                             {"pointMasses", writeTracks(project.pointMasses)},
                             {"movie", document.movie},
                             {"displayOrientation", document.useDisplayOrientation},
                             {"archive", QString::fromStdString(document.archiveReference)},
                             {"movieFrameTimes", frameTimes},
                             {"clip", clipObject}};
    const auto bytes = QJsonDocument(output).toJson(QJsonDocument::Compact);
    QSaveFile file(path);
    require(bytes.size() <= MAX_DOCUMENT_BYTES && file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit());
}

} // namespace motrack
