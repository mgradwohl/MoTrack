#include "../corpus_documents.hpp"
#include "command_checks.hpp"
#include "command_json.hpp"
#include "document_dump.hpp"
#include "legacy_view.hpp"
#include "model/commands.hpp"
#include "model/document_history.hpp"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include <vector>

/// Oracle O6, MoTrack side (docs/tracker-document.md §5): seeded random command sequences on every corpus document,
/// checking that refusals change nothing, that revisions only grow, that the incrementally maintained graph equals a
/// rebuilt one, that every command survives its JSON form and that undoing everything returns an equal document; and
/// `motrack_corpus_probe --apply`.
namespace
{

namespace model = motrack::model;
using model::Command;
using model::CommandValue;
using model::DocumentHistory;
using model::Family;
using model::ObjectId;
using model::Role;
using model::SetField;
using model::TrackerDocument;
using motrack::test::required;

/// Proposes new commands from the document's current state. Many are refused (stale or invalid values, timing
/// invariants, exhausted ranges); a refusal is checked like an applied command.
class Generator
{
  public:
    explicit Generator(std::uint32_t seed) : m_Random(seed)
    {}

    int pick(int count)
    {
        return std::uniform_int_distribution<int>(0, count - 1)(m_Random);
    }

    double real(double low, double high)
    {
        return std::uniform_real_distribution<double>(low, high)(m_Random);
    }

    std::optional<Command> next(const TrackerDocument& document)
    {
        switch (pick(10))
        {
        case 0:
            return rename(document);
        case 1:
            return mass(document);
        case 2:
        case 3:
            return mark(document);
        case 4:
            return sample(document);
        case 5:
            return create(document);
        case 6:
            return remove(document);
        case 7:
            return label(document);
        case 8:
            return coordinates(document);
        default:
            break;
        }
        if (pick(4) == 0)
        {
            auto first = next(document);
            auto second = next(document);
            if (first && second)
            {
                return model::Batch{.commands = {*std::move(first), *std::move(second)}};
            }
        }
        return clip(document);
    }

  private:
    /// A live track of the families given, or none.
    const model::ObjectSlot* track(const TrackerDocument& document, std::initializer_list<Family> families)
    {
        std::vector<const model::ObjectSlot*> candidates;
        for (const auto& object : document.objects())
        {
            if (object.role == Role::Track && object.state == model::ObjectState::Live && std::ranges::contains(families, object.family()))
            {
                candidates.push_back(&object);
            }
        }
        return candidates.empty() ? nullptr : candidates[static_cast<std::size_t>(pick(static_cast<int>(candidates.size())))];
    }

    static SetField set(const model::ObjectSlot& object, std::string field, CommandValue after)
    {
        auto before = required(model::fieldValue(*object.value, field));
        return {.object = object.id, .field = std::move(field), .before = std::move(before), .after = std::move(after)};
    }

    std::optional<Command> rename(const TrackerDocument& document)
    {
        const auto* object = track(document, {Family::PointMass, Family::Vector});
        if (object == nullptr)
        {
            return create(document);
        }
        return set(*object, "name", "R" + std::to_string(pick(6)));
    }

    std::optional<Command> mass(const TrackerDocument& document)
    {
        const auto* object = track(document, {Family::PointMass});
        if (object == nullptr)
        {
            return create(document);
        }
        return set(*object, "mass", pick(8) == 0 ? 0.0 : real(0.1, 10.0));
    }

    std::optional<Command> mark(const TrackerDocument& document)
    {
        const auto* object = track(document, {Family::PointMass});
        if (object == nullptr)
        {
            return create(document);
        }
        const auto frame = pick(24);
        std::optional<model::Mark> value;
        if (pick(3) != 0)
        {
            value = model::Mark{.x = real(0, 640), .y = real(0, 480)};
        }
        auto command = model::setMark(document, object->id, frame, value);
        return command ? std::optional(*std::move(command)) : std::nullopt;
    }

    std::optional<Command> sample(const TrackerDocument& document)
    {
        const auto* object = track(document, {Family::Vector});
        if (object == nullptr)
        {
            return create(document);
        }
        const auto frame = pick(24);
        auto before = required(model::fieldValue(*object->value, "framedata", frame));
        if (auto* element = std::get_if<model::FrameElement<model::VectorSample>>(&before))
        {
            element->node.reset();
        }
        CommandValue after = std::monostate{};
        if (pick(3) != 0)
        {
            after = model::FrameElement<model::VectorSample>{
                .value = {.xTail = real(0, 640), .yTail = real(0, 480), .xComponent = real(-50, 50), .yComponent = real(-50, 50)}};
        }
        return SetField{.object = object->id, .field = "framedata", .frame = frame, .before = std::move(before), .after = std::move(after)};
    }

    std::optional<Command> create(const TrackerDocument& document)
    {
        auto insert = model::createTrack(document, pick(2) == 0 ? Family::PointMass : Family::Vector, "C" + std::to_string(pick(4)));
        return insert ? std::optional<Command>(*std::move(insert)) : std::nullopt;
    }

    std::optional<Command> remove(const TrackerDocument& document)
    {
        const auto* object = track(document, {Family::PointMass, Family::Vector, Family::Unknown});
        if (object == nullptr)
        {
            return create(document);
        }
        return model::Remove{.object = object->id};
    }

    std::optional<Command> label(const TrackerDocument& document)
    {
        static constexpr std::array<std::string_view, 3> FIELDS{"mass_unit", "time_unit", "length_unit"};
        static constexpr std::array<std::string_view, 6> LABELS{"kg", "g", "s", "ms", "cm", "px"};
        return set(*document.singleton(Role::Panel),
                   std::string(FIELDS.at(static_cast<std::size_t>(pick(3)))),
                   std::string(LABELS.at(static_cast<std::size_t>(pick(6)))));
    }

    std::optional<Command> coordinates(const TrackerDocument& document)
    {
        const auto* coords = document.singleton(Role::Coordinates);
        const model::CoordKey key{
            .xOrigin = real(0, 640), .yOrigin = real(0, 480), .angle = real(-180, 180), .xScale = real(0.5, 4), .yScale = real(0.5, 4)};
        if (coords == nullptr || coords->family() != Family::Coordinates)
        {
            model::CoordinatesState state{.fixedOrigin = pick(2) == 0, .fixedAngle = pick(2) == 0, .fixedScale = pick(2) == 0};
            state.keys.set(0, {.value = key});
            auto insert = model::createSingleton(document, Role::Coordinates, std::move(state));
            return insert ? std::optional<Command>(*std::move(insert)) : std::nullopt;
        }
        const auto& keys = std::get<model::CoordinatesState>(*coords->value).keys;
        const auto frame = pick(12);
        const auto* current = keys.find(frame);
        CommandValue before =
            current != nullptr ? CommandValue(model::FrameElement<model::CoordKey>{.value = current->value}) : CommandValue{};
        // A calibration keeps at least one key.
        const CommandValue after = current != nullptr && keys.size() > 1 && pick(3) == 0
                                       ? CommandValue{}
                                       : CommandValue(model::FrameElement<model::CoordKey>{.value = key});
        return SetField{.object = coords->id, .field = "framedata", .frame = frame, .before = std::move(before), .after = after};
    }

    std::optional<Command> clip(const TrackerDocument& document)
    {
        const auto* control = document.singleton(Role::ClipControl);
        if (pick(2) == 0)
        {
            if (control == nullptr || control->family() != Family::ClipControl)
            {
                auto insert = model::createSingleton(document, Role::ClipControl, model::ClipControlState{.deltaT = real(10, 100)});
                return insert ? std::optional<Command>(*std::move(insert)) : std::nullopt;
            }
            return set(*control, "delta_t", real(10, 100));
        }
        const auto* clip = document.singleton(Role::Clip);
        if (clip == nullptr || clip->family() != Family::Clip)
        {
            auto insert = model::createSingleton(document, Role::Clip, model::ClipState{.startFrame = 0, .stepSize = 1, .stepCount = 10});
            return insert ? std::optional<Command>(*std::move(insert)) : std::nullopt;
        }
        switch (pick(4))
        {
        case 0:
            return set(*clip, "startframe", pick(5));
        case 1:
            return set(*clip, "stepsize", 1 + pick(3));
        case 2:
            return set(*clip, "stepcount", 1 + pick(20));
        default:
            break;
        }
        return set(*clip, "starttime", real(-1000, 1000));
    }

    std::mt19937 m_Random;
};

/// The document's dump with these identity allocators: undo restores everything except the allocators, which never
/// rewind.
std::string dumpWithAllocators(const TrackerDocument& document, std::uint64_t nextObjectId, std::uint64_t countdownLowWater)
{
    auto parts = document.parts();
    parts.nextObjectId = nextObjectId;
    parts.countdownLowWater = countdownLowWater;
    return motrack::test::documentDumpText(*TrackerDocument::assemble(parts));
}

struct Revisions
{
    std::uint64_t history = 0;
    std::uint64_t document = 0;
    std::uint64_t graph = 0;
};

Revisions revisions(const TrackerDocument& document, const DocumentHistory& history)
{
    return {.history = history.revision(), .document = history.documentRevision(), .graph = document.graph().revision().value};
}

/// One seeded sequence on a document, with every check after every step.
void runSequence(const TrackerDocument& original, std::uint32_t seed, int steps)
{
    Generator generator(seed);
    auto document = original;
    DocumentHistory history;
    std::size_t applied = 0;
    for (int step = 0; step < steps; ++step)
    {
        SCOPED_TRACE("step " + std::to_string(step));
        const auto before = revisions(document, history);
        const auto dump = motrack::test::documentDumpText(document);
        std::expected<void, model::CommandError> result;
        const auto choice = generator.pick(10);
        if (choice < 2 && history.canUndo())
        {
            result = history.undo(document);
        }
        else if (choice < 3 && history.canRedo())
        {
            result = history.redo(document);
        }
        else
        {
            const auto command = generator.next(document);
            if (!command)
            {
                continue;
            }
            EXPECT_EQ(motrack::test::decodeCommand(motrack::test::encodeCommand(*command)), *command);
            result = history.apply(document, *command);
        }
        const auto after = revisions(document, history);
        if (result)
        {
            ++applied;
            EXPECT_GT(after.history, before.history);
            EXPECT_GT(after.document, before.document);
            EXPECT_GE(after.graph, before.graph);
        }
        else
        {
            // A refusal leaves the document, both revisions and the graph unchanged.
            EXPECT_EQ(motrack::test::documentDumpText(document), dump) << result.error().message;
            EXPECT_EQ(after.history, before.history);
            EXPECT_EQ(after.document, before.document);
            EXPECT_EQ(after.graph, before.graph);
        }
        ASSERT_TRUE(motrack::test::graphMatchesRebuild(document));
        EXPECT_NO_THROW(static_cast<void>(motrack::legacyProject(document)));
    }
    const auto top = motrack::test::documentDumpText(document);
    const bool atTop = !history.canRedo();
    const auto graphRevision = document.graph().revision();
    while (history.canUndo())
    {
        ASSERT_TRUE(history.undo(document));
    }
    EXPECT_EQ(motrack::test::documentDumpText(document),
              dumpWithAllocators(original, document.nextObjectId(), document.countdownLowWater()));
    EXPECT_GE(document.graph().revision(), graphRevision);
    ASSERT_TRUE(motrack::test::graphMatchesRebuild(document));
    // Redoing everything returns to where the sequence ended, unless its last steps were undos.
    while (history.canRedo())
    {
        ASSERT_TRUE(history.redo(document));
    }
    if (atTop)
    {
        EXPECT_EQ(motrack::test::documentDumpText(document), top);
    }
    EXPECT_TRUE(motrack::test::graphMatchesRebuild(document));
    EXPECT_GT(applied, 0u);
}

TEST(CommandSequences, SeededSequencesOnEveryCorpusDocumentUndoToAnEqualDocument)
{
    std::size_t documents = 0;
    std::uint32_t seed = 2026;
    runSequence(TrackerDocument(), seed++, 60);
    for (const auto& corpus : motrack::test::corpusDocuments())
    {
        std::optional<TrackerDocument> imported;
        try
        {
            imported = motrack::importTypedTracker(corpus.bytes);
        }
        catch (const motrack::ProjectError&)
        {
            continue;
        }
        SCOPED_TRACE(corpus.label);
        runSequence(*imported, seed++, 30);
        ++documents;
    }
    EXPECT_GE(documents, 20u);
}

TEST(CommandSequences, TheJsonFormRefusesWhatItCannotHold)
{
    using motrack::test::CommandJsonError;
    const auto document = TrackerDocument();
    auto insert = required(model::createTrack(document, Family::PointMass, "A"));
    auto restoration = insert;
    restoration.restores = model::RemovedObject{.id = insert.object.id};
    EXPECT_THROW(static_cast<void>(motrack::test::encodeCommand(restoration)), CommandJsonError);
    EXPECT_THROW(static_cast<void>(motrack::test::encodeCommand(model::Remove{.object = ObjectId{1}, .restoreAt = model::Placement{}})),
                 CommandJsonError);
    EXPECT_THROW(static_cast<void>(motrack::test::encodeValue(model::FrameElement<model::Mark>{.node = motrack::osp::NodeId{1}})),
                 CommandJsonError);
    for (const auto* text : {R"({"set": {"object": "0", "field": "name", "before": null, "after": null}})",
                             R"({"set": {"object": "1", "field": "name", "before": {"string": 1}, "after": null}})",
                             R"({"set": {"object": "1", "field": "framedata", "frame": 1.5, "before": null, "after": null}})",
                             R"({"set": {"object": "1", "field": "name", "before": {"unread": "lost"}, "after": null}})",
                             R"({"set": {"object": "1", "field": "framedata", "frame": 1, "before": {"mark": [1]}, "after": null}})",
                             R"({"insert": {"object": {"id": "2", "range": "lowest", "class": "x", "fields": {}}, "position": 0}})",
                             R"({"insert": {"object": {"id": "2", "range": "counter", "role": "track", "family": "point-mass", "class": "x",
                                 "fields": {"color": null}}, "position": 0}})",
                             R"({"insert": {"object": {"id": "2", "range": "counter", "role": "track", "family": "point-mass", "class": "x",
                                 "fields": {}}, "position": -1}})",
                             R"({"rotate": {}})"})
    {
        SCOPED_TRACE(text);
        EXPECT_THROW(static_cast<void>(motrack::test::decodeCommand(QJsonDocument::fromJson(text).object())), CommandJsonError);
    }
    // Numbers that JSON cannot hold travel as text.
    const SetField infinite{.object = ObjectId{1}, .field = "mass", .before = std::numeric_limits<double>::infinity(), .after = -0.5};
    EXPECT_EQ(motrack::test::decodeCommand(motrack::test::encodeCommand(infinite)), Command{infinite});
    const auto nan = std::get<SetField>(motrack::test::decodeCommand(
        motrack::test::encodeCommand(SetField{.object = ObjectId{1}, .field = "mass", .before = std::nan(""), .after = 1.0})));
    EXPECT_TRUE(model::sameValue(nan.before, std::nan("")));
}

QString runProbe(const QStringList& arguments)
{
    QProcess probe;
    probe.start(QString::fromUtf8(MOTRACK_CORPUS_PROBE), arguments);
    EXPECT_TRUE(probe.waitForStarted(5000));
    EXPECT_TRUE(probe.waitForFinished(30000));
    return probe.exitStatus() == QProcess::NormalExit && probe.exitCode() == 0 ? QString()
                                                                               : QString::fromUtf8(probe.readAllStandardError());
}

QJsonObject readJson(const QString& path)
{
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::ReadOnly));
    return QJsonDocument::fromJson(file.readAll()).object();
}

TEST(CommandSequences, TheProbeAppliesAnOperationsFileToATrackerDocument)
{
    const auto input = QDir(QString::fromUtf8(MOTRACK_CORPUS_FIXTURES)).filePath("center-of-mass.trk");
    QFile source(input);
    ASSERT_TRUE(source.open(QIODevice::ReadOnly));
    const auto document = motrack::importTypedTracker(source.readAll().toStdString());
    const auto& first = *document.object(ObjectId{1});
    const auto name = std::get<std::string>(required(model::fieldValue(*first.value, "name")));
    QJsonArray operations{
        motrack::test::encodeCommand(SetField{.object = ObjectId{1}, .field = "name", .before = name, .after = std::string("Renamed")}),
        motrack::test::encodeCommand(*model::setMark(document, ObjectId{1}, 40, model::Mark{.x = 12.5, .y = 7.25})),
    };
    auto stale =
        motrack::test::encodeCommand(SetField{.object = ObjectId{1}, .field = "name", .before = name, .after = std::string("Again")});
    stale.insert("expect", "refused");
    operations.append(stale);
    QTemporaryDir directory;
    const auto write = [&](const QJsonArray& list)
    {
        QFile file(directory.filePath("operations.json"));
        EXPECT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(QJsonDocument(QJsonObject{{"schema_version", 1}, {"operations", list}}).toJson());
    };
    write(operations);
    const auto output = directory.filePath("applied.json");
    ASSERT_EQ(runProbe({"--apply", directory.filePath("operations.json"), input, output}), QString());
    const auto tracks = readJson(output)["panel"].toObject()["tracks"].toArray();
    ASSERT_FALSE(tracks.isEmpty());
    EXPECT_EQ(tracks[0].toObject()["name"].toString(), "Renamed");
    const auto steps = tracks[0].toObject()["steps"].toArray();
    EXPECT_TRUE(std::ranges::any_of(steps, [](const auto& step) { return step.toObject()["frame"].toInt() == 40; }));

    // Undoing every operation gives the state snapshot of the unedited file.
    operations.append(QJsonObject{{"undo", QJsonObject{}}});
    operations.append(QJsonObject{{"undo", QJsonObject{}}});
    operations.append(QJsonObject{{"undo", QJsonObject{}}, {"expect", "refused"}});
    write(operations);
    ASSERT_EQ(runProbe({"--apply", directory.filePath("operations.json"), input, output}), QString());
    const auto unedited = directory.filePath("state.json");
    ASSERT_EQ(runProbe({"--state", input, unedited}), QString());
    EXPECT_EQ(readJson(output), readJson(unedited));
    operations.append(QJsonObject{{"redo", QJsonObject{}}});
    write(operations);
    ASSERT_EQ(runProbe({"--apply", directory.filePath("operations.json"), input, output}), QString());

    // An outcome other than the expected one fails the run.
    operations.append(QJsonObject{{"redo", QJsonObject{}}, {"expect", "refused"}});
    write(operations);
    EXPECT_NE(runProbe({"--apply", directory.filePath("operations.json"), input, output}), QString());
    write(QJsonArray{QJsonObject{{"undo", QJsonObject{}}}});
    EXPECT_TRUE(runProbe({"--apply", directory.filePath("operations.json"), input, output}).contains("refused"));
    write(QJsonArray{QJsonObject{{"undo", QJsonObject{}}, {"expect", "maybe"}}});
    EXPECT_NE(runProbe({"--apply", directory.filePath("operations.json"), input, output}), QString());
    QFile broken(directory.filePath("operations.json"));
    ASSERT_TRUE(broken.open(QIODevice::WriteOnly | QIODevice::Truncate));
    broken.write("{\"schema_version\": 2}");
    broken.close();
    EXPECT_NE(runProbe({"--apply", directory.filePath("operations.json"), input, output}), QString());
    EXPECT_NE(runProbe({"--apply", directory.filePath("missing.json"), input, output}), QString());
}

} // namespace
