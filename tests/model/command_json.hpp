#pragma once

#include "model/commands.hpp"
#include "model/descriptors.hpp"
#include "model/families.hpp"
#include "model/tracker_document.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

/// The JSON form of commands for oracle O6 (docs/tracker-document.md §5). `motrack_corpus_probe --apply ops.json` applies
/// an operations file to a Tracker document, and B4's Java applier is to apply the same file in Tracker. The form holds
/// new commands, undo and redo; the records a restoration carries (removed graph state, frame nodes) are runtime state
/// and have none. Identities are decimal strings, and numbers JSON cannot hold are "NaN", "Infinity" and "-Infinity".
///
/// An operations file is `{"schema_version": 1, "operations": [...]}`. Each operation has one key, `set`, `insert`,
/// `remove`, `batch`, `undo` or `redo`, and optionally `"expect": "refused"`. Values are `null` (an absent element),
/// `{"unread": "missing"}` and the other reasons a property reads as none, or one of `bool`, `int`, `double`, `string`,
/// `strings`, `ints`, `mark` ([x, y]), `sample` ([xtail, ytail, xcomponent, ycomponent]) and `key` ([xorigin, yorigin,
/// angle, xscale, yscale]), in image pixels and degrees.
namespace motrack::test
{

class CommandJsonError : public std::runtime_error
{
  public:
    using std::runtime_error::runtime_error;
};

inline constexpr std::array<std::string_view, 5> DECODE_ERROR_NAMES{"missing", "wrong-type", "malformed", "out-of-range", "unsupported"};

inline QJsonValue jsonNumber(double value)
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

inline double readNumber(const QJsonValue& value)
{
    if (value.isDouble())
    {
        return value.toDouble();
    }
    const auto text = value.toString();
    if (text == "NaN")
    {
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (text == "Infinity" || text == "-Infinity")
    {
        return text.startsWith('-') ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
    }
    throw CommandJsonError("Expected a number.");
}

inline std::int32_t readInt(const QJsonValue& value)
{
    const auto number = value.toDouble(std::numeric_limits<double>::quiet_NaN());
    if (!value.isDouble() || number != std::trunc(number) || number < std::numeric_limits<std::int32_t>::min() ||
        number > std::numeric_limits<std::int32_t>::max())
    {
        throw CommandJsonError("Expected a 32-bit integer.");
    }
    return static_cast<std::int32_t>(number);
}

inline std::size_t readIndex(const QJsonValue& value)
{
    const auto number = value.toDouble(-1.0);
    if (!value.isDouble() || number < 0 || number != std::trunc(number) || number > 9007199254740992.0)
    {
        throw CommandJsonError("Expected a position.");
    }
    return static_cast<std::size_t>(number);
}

inline QJsonValue jsonId(model::ObjectId id)
{
    return QString::number(id.value);
}

inline model::ObjectId readId(const QJsonValue& value)
{
    bool ok = false;
    const auto id = value.toString().toULongLong(&ok);
    if (!value.isString() || !ok || id == 0)
    {
        throw CommandJsonError("Expected an object identity.");
    }
    return model::ObjectId{id};
}

template <std::size_t N> QJsonArray jsonNumbers(const std::array<double, N>& numbers)
{
    QJsonArray array;
    for (const auto number : numbers)
    {
        array.append(jsonNumber(number));
    }
    return array;
}

template <std::size_t N> std::array<double, N> readNumbers(const QJsonValue& value)
{
    const auto array = value.toArray();
    if (!value.isArray() || array.size() != static_cast<qsizetype>(N))
    {
        throw CommandJsonError("Expected " + std::to_string(N) + " numbers.");
    }
    std::array<double, N> numbers{};
    for (std::size_t index = 0; index < N; ++index)
    {
        numbers[index] = readNumber(array[static_cast<qsizetype>(index)]);
    }
    return numbers;
}

inline QJsonValue encodeValue(const model::CommandValue& value)
{
    return std::visit(
        [](const auto& item) -> QJsonValue
        {
            using Item = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<Item, std::monostate>)
            {
                return QJsonValue::Null;
            }
            else if constexpr (std::is_same_v<Item, osp::DecodeError>)
            {
                return QJsonObject{{"unread", QString::fromUtf8(DECODE_ERROR_NAMES.at(static_cast<std::size_t>(item)).data())}};
            }
            else if constexpr (std::is_same_v<Item, bool>)
            {
                return QJsonObject{{"bool", item}};
            }
            else if constexpr (std::is_same_v<Item, std::int32_t>)
            {
                return QJsonObject{{"int", item}};
            }
            else if constexpr (std::is_same_v<Item, double>)
            {
                return QJsonObject{{"double", jsonNumber(item)}};
            }
            else if constexpr (std::is_same_v<Item, std::string>)
            {
                return QJsonObject{{"string", QString::fromStdString(item)}};
            }
            else if constexpr (std::is_same_v<Item, std::vector<std::string>>)
            {
                QJsonArray array;
                for (const auto& text : item)
                {
                    array.append(QString::fromStdString(text));
                }
                return QJsonObject{{"strings", array}};
            }
            else if constexpr (std::is_same_v<Item, std::vector<std::int32_t>>)
            {
                QJsonArray array;
                for (const auto number : item)
                {
                    array.append(number);
                }
                return QJsonObject{{"ints", array}};
            }
            else
            {
                if (item.node)
                {
                    throw CommandJsonError("A frame element's baseline node has no JSON form.");
                }
                const auto& element = item.value;
                using Element = std::decay_t<decltype(element)>;
                if constexpr (std::is_same_v<Element, model::Mark>)
                {
                    return QJsonObject{{"mark", jsonNumbers(std::array{element.x, element.y})}};
                }
                else if constexpr (std::is_same_v<Element, model::VectorSample>)
                {
                    return QJsonObject{
                        {"sample", jsonNumbers(std::array{element.xTail, element.yTail, element.xComponent, element.yComponent})}};
                }
                else
                {
                    return QJsonObject{
                        {"key", jsonNumbers(std::array{element.xOrigin, element.yOrigin, element.angle, element.xScale, element.yScale})}};
                }
            }
        },
        value);
}

inline model::CommandValue decodeValue(const QJsonValue& value)
{
    if (value.isNull())
    {
        return std::monostate{};
    }
    const auto object = value.toObject();
    if (!value.isObject() || object.size() != 1)
    {
        throw CommandJsonError("Expected one typed value.");
    }
    const auto kind = object.begin().key();
    const auto item = object.begin().value();
    if (kind == "unread")
    {
        for (std::size_t index = 0; index < DECODE_ERROR_NAMES.size(); ++index)
        {
            if (item.toString().toStdString() == DECODE_ERROR_NAMES.at(index))
            {
                return static_cast<osp::DecodeError>(index);
            }
        }
        throw CommandJsonError("Unknown unread reason.");
    }
    if (kind == "bool" && item.isBool())
    {
        return item.toBool();
    }
    if (kind == "int")
    {
        return readInt(item);
    }
    if (kind == "double")
    {
        return readNumber(item);
    }
    if (kind == "string" && item.isString())
    {
        return item.toString().toStdString();
    }
    if ((kind == "strings" || kind == "ints") && item.isArray())
    {
        if (kind == "ints")
        {
            std::vector<std::int32_t> numbers;
            for (const auto entry : item.toArray())
            {
                numbers.push_back(readInt(entry));
            }
            return numbers;
        }
        std::vector<std::string> texts;
        for (const auto entry : item.toArray())
        {
            if (!entry.isString())
            {
                throw CommandJsonError("Expected strings.");
            }
            texts.push_back(entry.toString().toStdString());
        }
        return texts;
    }
    if (kind == "mark")
    {
        const auto [x, y] = readNumbers<2>(item);
        return model::FrameElement<model::Mark>{.value = {.x = x, .y = y}};
    }
    if (kind == "sample")
    {
        const auto [xTail, yTail, xComponent, yComponent] = readNumbers<4>(item);
        return model::FrameElement<model::VectorSample>{
            .value = {.xTail = xTail, .yTail = yTail, .xComponent = xComponent, .yComponent = yComponent}};
    }
    if (kind == "key")
    {
        const auto [xOrigin, yOrigin, angle, xScale, yScale] = readNumbers<5>(item);
        return model::FrameElement<model::CoordKey>{
            .value = {.xOrigin = xOrigin, .yOrigin = yOrigin, .angle = angle, .xScale = xScale, .yScale = yScale}};
    }
    throw CommandJsonError("Unknown value kind " + kind.toStdString() + ".");
}

/// A default value of a family: every field unsaved and no frame elements.
inline model::TypedValue emptyValue(model::Family family)
{
    switch (family)
    {
    case model::Family::Panel:
        return model::PanelState{};
    case model::Family::Clip:
        return model::ClipState{};
    case model::Family::Video:
        return model::VideoState{};
    case model::Family::ClipControl:
        return model::ClipControlState{};
    case model::Family::Coordinates:
        return model::CoordinatesState{};
    case model::Family::PointMass:
        return model::PointMassState{};
    case model::Family::Vector:
        return model::VectorState{};
    case model::Family::Unknown:
        break;
    }
    return model::UnknownState{};
}

/// The frames of a value's frame-keyed field, in ascending order.
inline std::vector<std::int32_t> elementFrames(const model::TypedValue& value)
{
    std::vector<std::int32_t> frames;
    const auto collect = [&](const auto& series)
    {
        series.forEach([&](std::int32_t frame, const auto& /*element*/) { frames.push_back(frame); });
    };
    if (const auto* mass = std::get_if<model::PointMassState>(&value))
    {
        collect(mass->marks);
    }
    else if (const auto* vector = std::get_if<model::VectorState>(&value))
    {
        collect(vector->samples);
    }
    else if (const auto* coords = std::get_if<model::CoordinatesState>(&value))
    {
        collect(coords->keys);
    }
    return frames;
}

inline model::Family readFamily(const QJsonValue& value)
{
    using model::Family;
    for (const auto family : {Family::Panel,
                              Family::Clip,
                              Family::Video,
                              Family::ClipControl,
                              Family::Coordinates,
                              Family::PointMass,
                              Family::Vector,
                              Family::Unknown})
    {
        if (value.toString().toStdString() == model::familyName(family))
        {
            return family;
        }
    }
    throw CommandJsonError("Unknown family.");
}

inline model::Role readRole(const QJsonValue& value)
{
    using model::Role;
    for (const auto role : {Role::Panel, Role::Clip, Role::Video, Role::ClipControl, Role::Coordinates, Role::Track})
    {
        if (value.toString().toStdString() == model::roleName(role))
        {
            return role;
        }
    }
    throw CommandJsonError("Unknown role.");
}

inline model::CommandValue currentValue(const model::TypedValue& value, std::string_view field, std::optional<std::int32_t> frame)
{
    auto current = model::fieldValue(value, field, frame);
    if (!current)
    {
        throw CommandJsonError("The object has no field " + std::string(field) + ".");
    }
    return *std::move(current);
}

/// A new, unbound object: its identity, allocation range, role, class and every typed field.
inline QJsonObject encodeObject(const model::ObjectSlot& object)
{
    if (object.binding || object.state != model::ObjectState::Live)
    {
        throw CommandJsonError("Only new, live objects have a JSON form.");
    }
    QJsonObject fields;
    for (const auto& field : model::fieldDescriptors(object.family()))
    {
        const auto id = QString::fromUtf8(field.id.data(), static_cast<qsizetype>(field.id.size()));
        if (field.kind == model::FieldKind::Scalar)
        {
            fields.insert(id, encodeValue(currentValue(*object.value, field.id, std::nullopt)));
            continue;
        }
        QJsonArray elements;
        for (const auto frame : elementFrames(*object.value))
        {
            elements.append(QJsonArray{frame, encodeValue(currentValue(*object.value, field.id, frame))});
        }
        fields.insert(id, elements);
    }
    return {{"id", jsonId(object.id)},
            {"range", QString::fromUtf8(model::allocationRangeName(object.range).data())},
            {"role", QString::fromUtf8(model::roleName(object.role).data())},
            {"family", QString::fromUtf8(model::familyName(object.family()).data())},
            {"class", QString::fromStdString(object.className)},
            {"fields", fields}};
}

inline model::ObjectSlot decodeObject(const QJsonValue& value)
{
    const auto json = value.toObject();
    const auto range = json["range"].toString();
    if (!value.isObject() || (range != "counter" && range != "countdown") || !json["class"].isString() || !json["fields"].isObject())
    {
        throw CommandJsonError("Expected a new object.");
    }
    const auto family = readFamily(json["family"]);
    auto typed = emptyValue(family);
    const auto fields = json["fields"].toObject();
    for (auto entry = fields.begin(); entry != fields.end(); ++entry)
    {
        const auto id = entry.key().toStdString();
        const auto* field = model::findField(family, id);
        if (field == nullptr)
        {
            throw CommandJsonError("Unknown field " + id + ".");
        }
        if (field->kind == model::FieldKind::Scalar)
        {
            if (!model::setFieldValue(typed, id, std::nullopt, decodeValue(entry.value())))
            {
                throw CommandJsonError("Mistyped field " + id + ".");
            }
            continue;
        }
        for (const auto element : entry.value().toArray())
        {
            const auto pair = element.toArray();
            if (pair.size() != 2 || !model::setFieldValue(typed, id, readInt(pair[0]), decodeValue(pair[1])))
            {
                throw CommandJsonError("Mistyped element of " + id + ".");
            }
        }
    }
    return {.id = readId(json["id"]),
            .range = range == "counter" ? model::AllocationRange::Counter : model::AllocationRange::Countdown,
            .role = readRole(json["role"]),
            .className = json["class"].toString().toStdString(),
            .binding = std::nullopt,
            .state = model::ObjectState::Live,
            .value = std::make_shared<const model::TypedValue>(std::move(typed))};
}

inline QJsonObject encodeCommand(const model::Command& command)
{
    return std::visit(
        [](const auto& item) -> QJsonObject
        {
            using Item = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<Item, model::SetField>)
            {
                QJsonObject set{{"object", jsonId(item.object)},
                                {"field", QString::fromStdString(item.field)},
                                {"before", encodeValue(item.before)},
                                {"after", encodeValue(item.after)}};
                if (item.frame)
                {
                    set.insert("frame", *item.frame);
                }
                return {{"set", set}};
            }
            else if constexpr (std::is_same_v<Item, model::Insert>)
            {
                if (item.restores || !item.dependencies.empty() || !item.issues.empty())
                {
                    throw CommandJsonError("Only the insertion of a new object without dependencies has a JSON form.");
                }
                QJsonObject insert{{"object", encodeObject(item.object)}, {"position", static_cast<qint64>(item.position)}};
                if (item.trackPosition)
                {
                    insert.insert("track_position", static_cast<qint64>(*item.trackPosition));
                }
                return {{"insert", insert}};
            }
            else if constexpr (std::is_same_v<Item, model::Remove>)
            {
                if (item.restoreAt)
                {
                    throw CommandJsonError("A recorded placement has no JSON form.");
                }
                return {{"remove", QJsonObject{{"object", jsonId(item.object)}}}};
            }
            else
            {
                QJsonArray members;
                for (const auto& member : item.commands)
                {
                    members.append(encodeCommand(member));
                }
                return {{"batch", members}};
            }
        },
        command);
}

/// A command from its JSON form; an operation's `expect` is ignored here.
inline model::Command decodeCommand(const QJsonObject& json)
{
    if (const auto set = json["set"]; set.isObject())
    {
        const auto object = set.toObject();
        return model::SetField{.object = readId(object["object"]),
                               .field = object["field"].toString().toStdString(),
                               .frame = object.contains("frame") ? std::optional(readInt(object["frame"])) : std::nullopt,
                               .before = decodeValue(object["before"]),
                               .after = decodeValue(object["after"])};
    }
    if (const auto insert = json["insert"]; insert.isObject())
    {
        const auto object = insert.toObject();
        return model::Insert{.object = decodeObject(object["object"]),
                             .position = readIndex(object["position"]),
                             .trackPosition =
                                 object.contains("track_position") ? std::optional(readIndex(object["track_position"])) : std::nullopt};
    }
    if (const auto remove = json["remove"]; remove.isObject())
    {
        return model::Remove{.object = readId(remove.toObject()["object"])};
    }
    if (const auto batch = json["batch"]; batch.isArray())
    {
        model::Batch members;
        for (const auto member : batch.toArray())
        {
            members.commands.push_back(decodeCommand(member.toObject()));
        }
        return members;
    }
    throw CommandJsonError("Expected a set, insert, remove or batch command.");
}

} // namespace motrack::test
