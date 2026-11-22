#pragma once

#include "model/descriptors.hpp"
#include "model/tracker_document.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <cmath>
#include <cstdint>
#include <string>
#include <variant>

/// A canonical JSON description of a typed document for golden tests: every object with its identity, allocation
/// range, role, family, class, baseline path, state and typed fields, and the graph's dependencies. Identities are
/// decimal strings, so the full unsigned range survives JSON.
namespace motrack::test
{

inline QJsonValue dumpNumber(double value)
{
    if (std::isnan(value))
    {
        return QStringLiteral("NaN");
    }
    if (std::isinf(value))
    {
        return value > 0 ? QStringLiteral("Infinity") : QStringLiteral("-Infinity");
    }
    return value;
}

inline QJsonValue dumpId(std::uint64_t value)
{
    return QString::number(value);
}

inline QJsonValue dumpField(const motrack::model::FieldValue& value)
{
    return std::visit(
        [](const auto& item) -> QJsonValue
        {
            using Item = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<Item, std::monostate>)
            {
                return QJsonValue::Null;
            }
            else if constexpr (std::is_same_v<Item, double>)
            {
                return dumpNumber(item);
            }
            else if constexpr (std::is_same_v<Item, std::string>)
            {
                return QString::fromStdString(item);
            }
            else if constexpr (std::is_same_v<Item, std::vector<std::string>>)
            {
                QJsonArray array;
                for (const auto& text : item)
                {
                    array.append(QString::fromStdString(text));
                }
                return array;
            }
            else if constexpr (std::is_same_v<Item, std::vector<std::int32_t>>)
            {
                QJsonArray array;
                for (const auto number : item)
                {
                    array.append(number);
                }
                return array;
            }
            else
            {
                return item;
            }
        },
        value);
}

template <typename T, typename Values> QJsonArray dumpSeries(const motrack::model::FrameSeries<T>& series, Values values)
{
    QJsonArray rows;
    series.forEach(
        [&](std::int32_t frame, const motrack::model::FrameElement<T>& element)
        {
            QJsonArray row{frame};
            for (const auto number : values(element.value))
            {
                row.append(dumpNumber(number));
            }
            row.append(element.node ? QStringLiteral("bound") : QStringLiteral("native"));
            rows.append(row);
        });
    return rows;
}

inline QJsonValue dumpPath(const motrack::model::TrackerDocument& document, const std::optional<motrack::osp::NodeId>& node)
{
    if (!node)
    {
        return QJsonValue::Null;
    }
    QString text;
    for (const auto& step : motrack::model::baselinePath(*document.baseline(), *node))
    {
        text += QStringLiteral("/%1:%2").arg(step.ordinal).arg(QString::fromStdString(step.name));
    }
    return text.isEmpty() ? QStringLiteral("/") : text;
}

inline QJsonObject dumpFields(const motrack::model::ObjectSlot& object)
{
    using namespace motrack::model;
    QJsonObject fields;
    for (const auto& field : fieldDescriptors(object.family()))
    {
        if (field.kind == FieldKind::Scalar)
        {
            fields.insert(QString::fromUtf8(field.id.data(), static_cast<qsizetype>(field.id.size())), dumpField(scalarValue(*object.value, field)));
        }
    }
    if (const auto* mass = std::get_if<PointMassState>(object.value.get()))
    {
        fields.insert("framedata", dumpSeries(mass->marks, [](const motrack::model::Mark& mark) { return std::array{mark.x, mark.y}; }));
    }
    else if (const auto* vector = std::get_if<VectorState>(object.value.get()))
    {
        fields.insert("framedata",
                      dumpSeries(vector->samples,
                                 [](const motrack::model::VectorSample& sample) { return std::array{sample.xTail, sample.yTail, sample.xComponent, sample.yComponent}; }));
    }
    else if (const auto* coords = std::get_if<CoordinatesState>(object.value.get()))
    {
        fields.insert("framedata",
                      dumpSeries(coords->keys, [](const motrack::model::CoordKey& key) { return std::array{key.xOrigin, key.yOrigin, key.angle, key.xScale, key.yScale}; }));
    }
    else if (const auto* unknown = std::get_if<UnknownState>(object.value.get()))
    {
        fields.insert("label", unknown->label ? QJsonValue(QString::fromStdString(*unknown->label)) : QJsonValue::Null);
    }
    return fields;
}

inline QString facetText(motrack::model::Facet facet)
{
    switch (facet)
    {
    case motrack::model::Facet::Definition:
        return QStringLiteral("definition");
    case motrack::model::Facet::Geometry:
        return QStringLiteral("geometry");
    case motrack::model::Facet::Data:
        break;
    }
    return QStringLiteral("data");
}

inline QJsonObject documentDump(const motrack::model::TrackerDocument& document)
{
    using namespace motrack::model;
    QJsonArray objects;
    QJsonArray dependencies;
    for (const auto& object : document.objects())
    {
        objects.append(QJsonObject{{"id", dumpId(object.id.value)},
                                   {"range", QString::fromUtf8(allocationRangeName(object.range).data())},
                                   {"role", QString::fromUtf8(roleName(object.role).data())},
                                   {"family", QString::fromUtf8(familyName(object.family()).data())},
                                   {"class", QString::fromStdString(object.className)},
                                   {"binding", dumpPath(document, object.binding)},
                                   {"state", object.state == ObjectState::Live ? "live" : "removed"},
                                   {"fields", dumpFields(object)}});
        for (const auto facet : {Facet::Definition, Facet::Geometry, Facet::Data})
        {
            for (const auto& dependency : document.graph().dependencies({.object = object.id, .facet = facet}))
            {
                dependencies.append(QJsonObject{
                    {"owner", dumpId(object.id.value)},
                    {"facet", facetText(facet)},
                    {"kind", QString::fromUtf8(dependencyKindName(dependency.kind).data())},
                    {"slot", static_cast<qint64>(dependency.slot)},
                    {"target", dependency.target ? dumpId(dependency.target->value) : QJsonValue::Null},
                    {"targetFacet", facetText(dependency.targetFacet)},
                    {"savedName", QString::fromStdString(dependency.savedName)},
                    {"live", dependency.target && document.graph().contains(*dependency.target)},
                });
            }
        }
    }
    QJsonArray order;
    for (const auto id : document.trackOrder())
    {
        order.append(dumpId(id.value));
    }
    QJsonArray references;
    for (const auto& reference : document.media().references)
    {
        references.append(QString::fromStdString(reference));
    }
    QJsonArray issues;
    for (const auto& issue : document.referenceIssues())
    {
        issues.append(QJsonObject{{"owner", dumpId(issue.owner.value)},
                                  {"field", QString::fromStdString(issue.field)},
                                  {"reason", QString::fromStdString(issue.reason)}});
    }
    const auto& media = document.media();
    return {{"associations", QString::fromUtf8(associationsName(document.associations()).data())},
            {"baselineBytes", document.baseline() ? QJsonValue(static_cast<qint64>(document.baseline()->bytes().size())) : QJsonValue::Null},
            {"nextObjectId", dumpId(document.nextObjectId())},
            {"countdownLowWater", dumpId(document.countdownLowWater())},
            {"trackOrder", order},
            {"media", QJsonObject{{"references", references},
                                  {"sourceFrameCount", media.sourceFrameCount ? QJsonValue(*media.sourceFrameCount) : QJsonValue::Null}}},
            {"objects", objects},
            {"dependencies", dependencies},
            {"referenceIssues", issues}};
}

inline std::string documentDumpText(const motrack::model::TrackerDocument& document)
{
    return QJsonDocument(documentDump(document)).toJson(QJsonDocument::Indented).toStdString();
}

} // namespace motrack::test
