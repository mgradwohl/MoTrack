#pragma once

#include "legacy_project.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <string>

/// Test support for comparing legacy projects.
namespace motrack::test
{

/// A readable rendering of the reduced model, so a parity failure shows the differing field.
inline QJsonObject legacyProjectDump(const motrack::LegacyProject& project)
{
    QJsonArray masses;
    for (const auto& track : project.pointMasses)
    {
        QJsonArray marks;
        for (const auto& mark : track.marks)
        {
            marks.append(QJsonArray{mark.frame, mark.x, mark.y});
        }
        masses.append(QJsonObject{
            {"id", QString::number(track.id)}, {"name", QString::fromStdString(track.name)}, {"mass", track.mass}, {"marks", marks}});
    }
    QJsonArray vectors;
    for (const auto& track : project.vectors)
    {
        QJsonArray samples;
        for (const auto& sample : track.samples)
        {
            samples.append(QJsonArray{sample.frame, sample.tailX, sample.tailY, sample.componentX, sample.componentY});
        }
        vectors.append(QJsonObject{{"id", QString::number(track.id)}, {"name", QString::fromStdString(track.name)}, {"samples", samples}});
    }
    QJsonArray unsupported;
    for (const auto& type : project.unsupportedTrackTypes)
    {
        unsupported.append(QString::fromStdString(type));
    }
    QJsonArray media;
    for (const auto& reference : project.clip.mediaReferences)
    {
        media.append(QString::fromStdString(reference));
    }
    QJsonValue calibration = QJsonValue::Null;
    if (project.calibration)
    {
        QJsonArray keys;
        for (const auto& key : project.calibration->keyframes)
        {
            keys.append(QJsonArray{key.frame, key.originX, key.originY, key.angleDegrees, key.scaleX, key.scaleY});
        }
        calibration = QJsonObject{
            {"keys", keys},
            {"fixed", QJsonArray{project.calibration->fixedOrigin, project.calibration->fixedAngle, project.calibration->fixedScale}}};
    }
    const auto& clip = project.clip;
    return {{"version", QString::fromStdString(project.version)},
            {"description", QString::fromStdString(project.description)},
            {"units",
             QJsonArray{QString::fromStdString(project.lengthUnit),
                        QString::fromStdString(project.massUnit),
                        QString::fromStdString(project.timeUnit)}},
            {"clip",
             QJsonObject{{"first", clip.firstFrame},
                         {"step", clip.stepSize},
                         {"count", clip.stepCount ? QJsonValue(*clip.stepCount) : QJsonValue::Null},
                         {"duration", clip.frameDurationMs ? QJsonValue(*clip.frameDurationMs) : QJsonValue::Null},
                         {"start", clip.startTimeMs},
                         {"sourceFrames", clip.sourceFrameCount ? QJsonValue(*clip.sourceFrameCount) : QJsonValue::Null},
                         {"media", media}}},
            {"calibration", calibration},
            {"pointMasses", masses},
            {"vectors", vectors},
            {"unsupported", unsupported},
            {"nextTrackId", QString::number(project.nextTrackId)}};
}

inline std::string legacyProjectText(const motrack::LegacyProject& project)
{
    return QJsonDocument(legacyProjectDump(project)).toJson().toStdString();
}

} // namespace motrack::test
