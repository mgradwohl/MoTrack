#include "../corpus_documents.hpp"
#include "archive_group.hpp"
#include "document_dump.hpp"
#include "legacy_view.hpp"
#include "track_edits.hpp"
#include "tracker_archive.hpp"
#include "tracker_import.hpp"
#include "tracker_writer.hpp"
#include "workspace_document.hpp"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{

/// A readable rendering of the reduced model, so a parity failure shows the differing field.
QJsonObject describe(const motrack::LegacyProject& project)
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

std::string text(const motrack::LegacyProject& project)
{
    return QJsonDocument(describe(project)).toJson().toStdString();
}

std::string text(const std::optional<std::vector<motrack::TrackSourceBinding>>& bindings)
{
    if (!bindings)
    {
        return "null";
    }
    std::string result;
    for (const auto& binding : *bindings)
    {
        result += std::to_string(binding.trackId) + "@" + std::to_string(binding.sourceIndex) +
                  (binding.kind == motrack::TrackKind::PointMass ? " mass\n" : " vector\n");
    }
    return result;
}

TEST(LegacyViewParity, EveryCorpusFileAndFixtureReadsAsB1ImportsIt)
{
    std::size_t accepted = 0;
    std::size_t refused = 0;
    for (const auto& document : motrack::test::corpusDocuments())
    {
        SCOPED_TRACE(document.label);
        std::optional<motrack::ImportedTrackerProject> legacy;
        std::string legacyError;
        try
        {
            legacy = motrack::parseTrackerProject(document.bytes);
        }
        catch (const motrack::ProjectError& error)
        {
            legacyError = error.what();
        }
        std::optional<motrack::model::TrackerDocument> typed;
        std::string typedError;
        try
        {
            typed = motrack::importTypedTracker(document.bytes);
        }
        catch (const motrack::ProjectError& error)
        {
            typedError = error.what();
        }
        ASSERT_EQ(typed.has_value(), legacy.has_value()) << "B1: " << legacyError << "; typed: " << typedError;
        if (!legacy)
        {
            EXPECT_EQ(typedError, legacyError);
            ++refused;
            continue;
        }
        ++accepted;
        EXPECT_EQ(text(motrack::legacyProject(*typed)), text(legacy->project));
        EXPECT_EQ(motrack::legacyProject(*typed), legacy->project);
        const auto source = motrack::trackerSourceView(*typed);
        ASSERT_TRUE(source);
        EXPECT_EQ(source->xml, legacy->source.xml);
        EXPECT_EQ(text(source->bindings), text(legacy->source.bindings));
    }
    // The comparison covers the corpus, not a vacuous subset: Tracker experiments and the reader's refusals.
    EXPECT_GT(accepted, 50u) << accepted;
    EXPECT_GT(refused, 5u) << refused;
}

TEST(LegacyViewParity, TheCacheIsKeyedByOwnerAndRevisionAndNeverSharedBetweenCopies)
{
    const auto document =
        motrack::importTypedTracker(motrack::test::readCorpusFile(motrack::utf8Path(MOTRACK_FIXTURES) / "point-mass.trk"));
    motrack::LegacyViewCache cache;
    EXPECT_FALSE(cache.stamp());
    const auto first = cache.view(document, {.runtime = 1, .revision = 1});
    EXPECT_EQ(cache.view(document, {.runtime = 1, .revision = 1}), first);
    EXPECT_EQ(*first, motrack::legacyProject(document));
    // A new revision, or the same revision of another owner, never reuses the view.
    const auto second = cache.view(document, {.runtime = 1, .revision = 2});
    EXPECT_NE(second, first);
    EXPECT_EQ(*second, *first);
    EXPECT_NE(cache.view(document, {.runtime = 2, .revision = 2}), second);
    const auto copy = cache;
    EXPECT_FALSE(copy.stamp());
    motrack::LegacyViewCache assigned;
    assigned.prime({.runtime = 9, .revision = 9}, first);
    EXPECT_EQ(assigned.view(document, {.runtime = 9, .revision = 9}), first);
    assigned = cache;
    EXPECT_FALSE(assigned.stamp());
    auto moved = std::move(cache);
    EXPECT_EQ(moved.stamp(), (motrack::DocumentStamp{.runtime = 2, .revision = 2}));
    moved.clear();
    EXPECT_FALSE(moved.stamp());
}

TEST(LegacyViewParity, EmptyDocumentsAndEmptyProjectsCorrespond)
{
    EXPECT_EQ(motrack::legacyProject(motrack::model::TrackerDocument{}), motrack::LegacyProject{});
    EXPECT_FALSE(motrack::trackerSourceView(motrack::model::TrackerDocument{}));
    EXPECT_EQ(motrack::typedDocumentFromReduced({}, std::nullopt), motrack::model::TrackerDocument{});
}

/// The native fixtures' media-free baseline (every editable family and coordinate axes) with a center of mass of both
/// point masses, so that deleting mass B leaves a reference dangling.
std::string boundSource()
{
    auto xml = motrack::test::readCorpusFile(motrack::utf8Path(MOTRACK_FIXTURES) / "native" / "baseline.trk");
    const std::string massB = "        <property name=\"item\" type=\"object\">\n            <object "
                              "class=\"org.opensourcephysics.cabrillo.tracker.PointMass\">\n"
                              "                <property name=\"mass\" type=\"double\">1.0</property>";
    xml.insert(xml.find(massB),
               "        <property name=\"item\" type=\"object\">\n"
               "            <object class=\"org.opensourcephysics.cabrillo.tracker.CenterOfMass\">\n"
               "                <property name=\"name\" type=\"string\">center</property>\n"
               "                <property name=\"masses\" type=\"collection\" class=\"java.util.ArrayList\">\n"
               "                    <property name=\"item\" type=\"string\">mass A</property>\n"
               "                    <property name=\"item\" type=\"string\">mass B</property>\n"
               "                </property>\n"
               "            </object>\n"
               "        </property>\n");
    return xml;
}

motrack::ImportedTrackerProject importedBaseline()
{
    return motrack::parseTrackerProject(boundSource());
}

/// B1's export of a reduced model, or its refusal.
std::string exported(const motrack::LegacyProject& project, const motrack::TrackerSource& source)
{
    try
    {
        return motrack::renderTrackerProject(project, source);
    }
    catch (const motrack::ProjectError& error)
    {
        return std::string("refused: ") + error.what();
    }
}

TEST(LegacyViewParity, ConvertsEditedReducedModelsWithoutTurningUnchangedReadingsIntoEdits)
{
    auto imported = importedBaseline();
    auto& project = imported.project;
    const auto unchanged = motrack::typedDocumentFromReduced(project, imported.source);
    EXPECT_EQ(unchanged, motrack::importTypedTracker(boundSource()));

    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.renamePointMass(project, 0, "renamed"));
    ASSERT_TRUE(edits.markPointMass(project, 0, {.frame = 1, .x = 9.0, .y = 9.5}));
    ASSERT_TRUE(edits.markPointMass(project, 0, {.frame = 7, .x = 1.0, .y = 1.5}));
    ASSERT_TRUE(edits.removeMark(project, 0, 0));
    ASSERT_TRUE(edits.setPointMassValue(project, 0, 4.0));
    ASSERT_TRUE(edits.setMassUnit(project, "g"));
    ASSERT_TRUE(edits.removePointMass(project, 1));
    ASSERT_TRUE(edits.createPointMass(project, "created"));
    ASSERT_TRUE(edits.createVector(project, "arrow"));
    ASSERT_TRUE(edits.markVector(project, 1, {.frame = 2, .tailX = 1.0, .tailY = 2.0, .componentX = 3.0, .componentY = 4.0}));
    ASSERT_TRUE(edits.markVector(project, 0, {.frame = 3, .tailX = 5.0, .tailY = 6.0, .componentX = 7.0, .componentY = 8.0}));
    ASSERT_TRUE(edits.calibrateDistance(project,
                                        {.frame = 0,
                                         .imageDeltaX = 10.0,
                                         .imageDeltaY = 0.0,
                                         .knownLength = 2.0,
                                         .defaultOriginX = 0.0,
                                         .defaultOriginY = 0.0,
                                         .unit = "cm"}));
    ASSERT_TRUE(edits.setClipRange(project, 2, 8, 2));
    // Timing edits need a media-free experiment without other track classes; a native file can still hold new timing.
    project.clip.frameDurationMs = 50.0;
    project.clip.startTimeMs = 10.0;
    const auto document = motrack::typedDocumentFromReduced(project, imported.source);
    EXPECT_EQ(motrack::legacyProject(document), project);
    EXPECT_EQ(document.associations(), motrack::model::Associations::Known);

    const auto* edited = document.object(motrack::model::ObjectId{project.pointMasses[0].id});
    ASSERT_NE(edited, nullptr);
    EXPECT_TRUE(edited->binding);
    const auto& mass = std::get<motrack::model::PointMassState>(*edited->value);
    EXPECT_EQ(mass.name, std::string("renamed"));
    EXPECT_EQ(mass.mass, 4.0);
    ASSERT_NE(mass.marks.find(1), nullptr);
    EXPECT_TRUE(mass.marks.find(1)->node) << "A retained frame keeps its node.";
    EXPECT_FALSE(mass.marks.find(7)->node) << "An inserted frame has none.";
    EXPECT_EQ(mass.marks.find(0), nullptr);
    // The removed track keeps its binding, and references to it dangle.
    const auto removed = std::ranges::find(document.objects(), motrack::model::ObjectState::Removed, &motrack::model::ObjectSlot::state);
    ASSERT_NE(removed, document.objects().end());
    EXPECT_TRUE(removed->binding);
    EXPECT_FALSE(document.graph().contains(removed->id));
    EXPECT_FALSE(document.graph().referencesTo(removed->id).empty());
    // Created tracks are unbound counter identities.
    const auto* created = document.object(motrack::model::ObjectId{project.pointMasses.back().id});
    ASSERT_NE(created, nullptr);
    EXPECT_FALSE(created->binding);
    EXPECT_EQ(created->range, motrack::model::AllocationRange::Counter);
    // Only edited fields changed: the unchanged length unit keeps the baseline's reading (absent, so Tracker's "m").
    const auto& panel = std::get<motrack::model::PanelState>(*document.singleton(motrack::model::Role::Panel)->value);
    EXPECT_EQ(panel.massUnit, std::string("g"));
    EXPECT_EQ(panel.lengthUnit, std::string("cm"));
    EXPECT_EQ(panel.description.error_or(motrack::osp::DecodeError::Unsupported), motrack::osp::DecodeError::Unsupported);
    const auto& control = std::get<motrack::model::ClipControlState>(*document.singleton(motrack::model::Role::ClipControl)->value);
    EXPECT_EQ(control.deltaT, 50.0);
    // Standard export of the view makes B1's decision for the reduced model.
    EXPECT_EQ(exported(motrack::legacyProject(document), *motrack::trackerSourceView(document)), exported(project, imported.source));
}

TEST(LegacyViewParity, ConvertsSourcesWithoutAssociationsToNativeOnlyObjects)
{
    auto imported = importedBaseline();
    imported.source.bindings.reset();
    imported.project.unsupportedTrackTypes.push_back("org.example.Placeholder");
    imported.project.clip.mediaReferences = {"movie.mp4"};
    imported.project.clip.sourceFrameCount = 40;
    for (const bool reset : {false, true})
    {
        const auto document = motrack::typedDocumentFromReduced(imported.project, imported.source, reset);
        EXPECT_EQ(document.associations(), reset ? motrack::model::Associations::Stale : motrack::model::Associations::Unknown);
        EXPECT_TRUE(std::ranges::none_of(document.objects(), [](const auto& object) { return object.binding.has_value(); }));
        EXPECT_EQ(motrack::legacyProject(document), imported.project);
        EXPECT_EQ(document.media().sourceFrameCount, 40);
        const auto source = motrack::trackerSourceView(document);
        ASSERT_TRUE(source);
        EXPECT_FALSE(source->bindings);
        EXPECT_TRUE(document.graph().plan().references.empty());
    }
    motrack::LegacyProject project = imported.project;
    project.clip.frameDurationMs.reset();
    project.calibration.reset();
    EXPECT_EQ(motrack::legacyProject(motrack::typedDocumentFromReduced(project, std::nullopt)), project);
}

TEST(LegacyViewParity, RefusesConversionsThatWouldChangeStoredValues)
{
    auto imported = importedBaseline();
    // Removing a calibration that the baseline keeps is not an edit MoTrack makes; it cannot be represented.
    imported.project.calibration.reset();
    EXPECT_THROW((void)motrack::typedDocumentFromReduced(imported.project, imported.source), motrack::ProjectError);
    imported = importedBaseline();
    imported.source.bindings->front().sourceIndex = 99;
    EXPECT_THROW((void)motrack::typedDocumentFromReduced(imported.project, imported.source), motrack::ProjectError);
    imported = importedBaseline();
    imported.project.nextTrackId = 1;
    EXPECT_THROW((void)motrack::typedDocumentFromReduced(imported.project, imported.source), motrack::ProjectError);
    EXPECT_THROW((void)motrack::importTypedTracker("<object class=\"Other\"/>\n"), motrack::ProjectError);
}

// Native migration fixtures (tests/fixtures/native, provenance in tests/fixtures/README.md). Each opens with today's
// reader, converts into a typed document that matches its committed dump, and exports as its committed B1 export or
// with its committed refusal. They pin today's reading before B2.4a switches the native reader to typed documents.

QString nativeFixture(const QString& name)
{
    return QString::fromUtf8(MOTRACK_FIXTURES) + "/native/" + name;
}

QByteArray readBytes(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

bool writeBytes(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

/// B1's standard export of one native document: the exported Tracker XML (an archive document's exported member), or
/// "refused: " and the reason.
QByteArray nativeExport(const motrack::WorkspaceDocument& document)
{
    QTemporaryDir temporary;
    try
    {
        if (document.archivePayload && document.archiveMember)
        {
            const auto path = temporary.filePath("export.trz");
            motrack::writeTrackerArchiveDocument(path, document);
            const auto label = "export.trz!" + *document.archiveMember;
            for (const auto& member : motrack::test::corpusArchiveMembers("export.trz", readBytes(path).toStdString()))
            {
                if (member.label == label)
                {
                    return QByteArray::fromStdString(member.bytes);
                }
            }
            return "refused: the exported archive lacks the member";
        }
        const auto path = temporary.filePath("export.trk");
        motrack::writeTrackerDocument(path, document, temporary.filePath("project.motrack"));
        return readBytes(path);
    }
    catch (const motrack::ProjectError& error)
    {
        return QByteArray("refused: ") + error.what();
    }
}

QByteArray documentGolden(const motrack::WorkspaceDocument& document)
{
    const auto typed = motrack::typedDocumentFromReduced(document.project, document.trackerSource, document.trackerBindingsReset);
    return QByteArray::fromStdString(motrack::test::documentDumpText(typed));
}

/// The goldens of one native fixture: its typed document dump and B1's export, per member for a group.
std::map<QString, QByteArray> nativeGoldens(const QString& name)
{
    std::map<QString, QByteArray> goldens;
    const auto stem = name.chopped(QStringLiteral(".motrack").size());
    if (name.startsWith("v6-"))
    {
        for (const auto& member : motrack::readArchiveGroup(nativeFixture(name)).members)
        {
            const auto memberStem = stem + "." + QString::fromStdString(member.member).section('/', -1).chopped(4);
            goldens.emplace(memberStem + ".document.json", documentGolden(member.document));
            goldens.emplace(memberStem + ".export.txt", nativeExport(member.document));
        }
        return goldens;
    }
    const auto document = motrack::readWorkspaceDocument(nativeFixture(name));
    goldens.emplace(stem + ".document.json", documentGolden(document));
    goldens.emplace(stem + ".export.txt", nativeExport(document));
    return goldens;
}

const std::vector<QString>& nativeFixtures()
{
    static const std::vector<QString> names{"v1.motrack",
                                            "v2-source.motrack",
                                            "v3-source.motrack",
                                            "v4-known.motrack",
                                            "v4-stale.motrack",
                                            "v4-frame-shift.motrack",
                                            "v4-duplicate-keys.motrack",
                                            "v4-empty-mass-unit.motrack",
                                            "v5-plain.motrack",
                                            "v5-archive.motrack",
                                            "v6-group.motrack"};
    return names;
}

TEST(LegacyViewParity, NativeFixturesOpenToTheirDocumentDumpsAndExports)
{
    for (const auto& name : nativeFixtures())
    {
        SCOPED_TRACE(name.toStdString());
        for (const auto& [file, actual] : nativeGoldens(name))
        {
            SCOPED_TRACE(file.toStdString());
            const auto expected = readBytes(nativeFixture(file));
            ASSERT_FALSE(expected.isEmpty()) << "Missing golden; regenerate with MOTRACK_WRITE_NATIVE_FIXTURES.";
            EXPECT_EQ(actual.toStdString(), expected.toStdString());
        }
    }
    // A baseline the current reader refuses still fails to open, with the message that names the cause; native
    // version 7 opens such a document native-only instead (B2.5).
    try
    {
        static_cast<void>(motrack::readWorkspaceDocument(nativeFixture("v4-refused-baseline.motrack")));
        ADD_FAILURE() << "The refused baseline opened.";
    }
    catch (const motrack::ProjectError& error)
    {
        EXPECT_TRUE(std::string_view(error.what())
                        .starts_with("The copy of the original Tracker file kept in this MoTrack project can no longer be read"))
            << error.what();
    }
}

// Writes the native fixtures and their goldens. Run once with MOTRACK_WRITE_NATIVE_FIXTURES set to the fixture
// directory, then review every file: the v5 and v6 files come from this build's native writers, and v1 to v4 are
// the same writer's output reduced to the documented older schemas (docs/native-format.md).

/// The documented schema of an older standalone version: v5 added the container fields and the optional
/// `timeUnit` and `movieTimeBase` (#331); v4 added source bindings; v3 added track and counter identities; v2 added
/// the preserved source; v1 stored independent vectors nowhere.
QJsonObject downgrade(QJsonObject object, int version)
{
    object.insert("version", version);
    for (const auto* key : {"embeddedArchive", "archiveMember", "timeUnit", "movieTimeBase"})
    {
        object.remove(key);
    }
    if (version < 4)
    {
        auto source = object.value("trackerSource").toObject();
        source.remove("bindings");
        object.insert("trackerSource", source);
    }
    if (version < 3)
    {
        object.remove("nextTrackId");
        for (const auto* key : {"pointMasses", "vectors"})
        {
            QJsonArray tracks;
            for (const auto& entry : object.value(key).toArray())
            {
                auto track = entry.toObject();
                track.remove("id");
                tracks.append(track);
            }
            object.insert(key, tracks);
        }
    }
    if (version < 2)
    {
        object.remove("trackerSource");
        object.remove("vectors");
    }
    return object;
}

QByteArray withBaseline(const QByteArray& json, const QByteArray& xml)
{
    auto object = QJsonDocument::fromJson(json).object();
    auto source = object.value("trackerSource").toObject();
    source.insert("xmlBase64", QString::fromLatin1(xml.toBase64()));
    object.insert("trackerSource", source);
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

/// The fixtures' edits, all of which B1 exports: rename, mark, unmark and weigh mass A, change the mass unit, delete
/// mass B, create and mark a point mass, rename the vector, calibrate in centimeters and change the clip range and
/// timing. (B1 exports no vector samples beside point masses, so the baseline's vector has none.)
void editBaseline(motrack::LegacyProject& project)
{
    motrack::TrackEdits edits;
    const auto require = [](bool done)
    {
        if (!done)
        {
            throw std::runtime_error("A fixture edit was refused.");
        }
    };
    require(edits.renamePointMass(project, 0, "renamed A"));
    require(edits.markPointMass(project, 0, {.frame = 2, .x = 120.0, .y = 79.0}));
    require(edits.removeMark(project, 0, 3));
    require(edits.setPointMassValue(project, 0, 3.0));
    require(edits.setMassUnit(project, "g"));
    require(edits.removePointMass(project, 1));
    require(edits.createPointMass(project, "created"));
    require(edits.markPointMass(project, 1, {.frame = 4, .x = 40.0, .y = 41.0}));
    require(edits.renameVector(project, 0, "renamed v"));
    require(edits.calibrateDistance(project,
                                    {.frame = 0,
                                     .imageDeltaX = 100.0,
                                     .imageDeltaY = 0.0,
                                     .knownLength = 1.0,
                                     .defaultOriginX = 0.0,
                                     .defaultOriginY = 0.0,
                                     .unit = "cm"}));
    require(edits.setClipRange(project, 1, 8, 1));
    require(edits.setClipTiming(project, 50.0, 10.0));
}

QByteArray nativeJson(const motrack::WorkspaceDocument& document, const QString& scratch)
{
    motrack::writeWorkspaceDocument(scratch, document);
    return readBytes(scratch);
}

TEST(LegacyViewParity, WriteNativeFixtures)
{
    const auto target = qEnvironmentVariable("MOTRACK_WRITE_NATIVE_FIXTURES");
    if (target.isEmpty())
    {
        GTEST_SKIP() << "Set MOTRACK_WRITE_NATIVE_FIXTURES to the fixture directory to regenerate the native fixtures.";
    }
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto baselineXml = readBytes(nativeFixture("baseline.trk"));
    const auto scratch = temporary.filePath("scratch.motrack");
    const auto importBaseline = [&](const QByteArray& xml)
    {
        const auto path = temporary.filePath("baseline.trk");
        EXPECT_TRUE(writeBytes(path, xml));
        return motrack::importTrackerDocument(path);
    };
    const auto write = [&](const QString& name, const QByteArray& bytes)
    {
        ASSERT_TRUE(writeBytes(QDir(target).filePath(name), bytes));
    };

    auto edited = importBaseline(baselineXml);
    editBaseline(edited.project);
    const auto current = nativeJson(edited, scratch);
    const auto current4 = QJsonDocument(downgrade(QJsonDocument::fromJson(current).object(), 4)).toJson(QJsonDocument::Compact);
    write("v5-plain.motrack", current);
    write("v4-known.motrack", current4);
    // Without associations nothing is exported, so these also hold a vector created with a sample.
    auto withVector = edited;
    motrack::TrackEdits vectorEdits;
    ASSERT_TRUE(vectorEdits.createVector(withVector.project, "arrow"));
    ASSERT_TRUE(
        vectorEdits.markVector(withVector.project, 1, {.frame = 1, .tailX = 1.0, .tailY = 2.0, .componentX = 3.0, .componentY = 4.0}));
    const auto unassociated = nativeJson(withVector, scratch);
    for (const int version : {1, 2, 3})
    {
        write(version == 1 ? QStringLiteral("v1.motrack") : QStringLiteral("v%1-source.motrack").arg(version),
              QJsonDocument(downgrade(QJsonDocument::fromJson(unassociated).object(), version)).toJson(QJsonDocument::Compact));
    }
    // Saved before B1: MoTrack's former DOM reader saw the track after a blank line that ends OSP's collection.
    auto blankLine = baselineXml;
    blankLine.replace("        <property name=\"item\" type=\"object\">\n            <object "
                      "class=\"org.opensourcephysics.cabrillo.tracker.PointMass\">\n                <property name=\"mass\" "
                      "type=\"double\">1.0</property>",
                      "\n        <property name=\"item\" type=\"object\">\n            <object "
                      "class=\"org.opensourcephysics.cabrillo.tracker.PointMass\">\n                <property name=\"mass\" "
                      "type=\"double\">1.0</property>");
    ASSERT_NE(blankLine, baselineXml);
    write("v4-stale.motrack", withBaseline(current4, blankLine));
    // A source the current reader refuses: the blank line ends the panel's tracks before a null element (#196).
    auto refused = baselineXml;
    refused.replace(
        "        <property name=\"item\" type=\"object\">\n            <object class=\"org.opensourcephysics.cabrillo.tracker.CoordAxes\">",
        "\n        <property name=\"item\" type=\"object\">null</property>\n        <property name=\"item\" type=\"object\">\n            "
        "<object class=\"org.opensourcephysics.cabrillo.tracker.CoordAxes\">");
    ASSERT_NE(refused, baselineXml);
    write("v4-refused-baseline.motrack", withBaseline(current4, refused));
    // Sources whose reading the loader-fidelity fixes change (#313-#315), saved as the reader of the time read them.
    const auto variant = [&](const QString& name, QByteArray xml)
    {
        ASSERT_NE(xml, baselineXml);
        const auto imported = importBaseline(xml);
        write(name,
              QJsonDocument(downgrade(QJsonDocument::fromJson(nativeJson(imported, scratch)).object(), 4)).toJson(QJsonDocument::Compact));
    };
    auto shifted = baselineXml;
    shifted.replace("            <property name=\"startframe\" type=\"int\">0</property>",
                    "            <property name=\"startframe\" type=\"int\">2</property>\n            <property name=\"frameshift\" "
                    "type=\"int\">2</property>");
    variant("v4-frame-shift.motrack", shifted);
    auto duplicate = baselineXml;
    const QByteArray key = "                <property name=\"[0]\" type=\"object\">\n                    <object "
                           "class=\"org.opensourcephysics.media.core.ImageCoordSystem$FrameData\">\n                        <property "
                           "name=\"xorigin\" type=\"double\">160.0</property>";
    duplicate.replace(key,
                      QByteArray(key)
                          .replace("160.0", "150.0")
                          .prepend(QByteArray(key).append(
                              "\n                        <property name=\"yorigin\" type=\"double\">120.0</property>\n                     "
                              "   <property name=\"angle\" type=\"double\">0.0</property>\n                        <property "
                              "name=\"xscale\" type=\"double\">40.0</property>\n                        <property name=\"yscale\" "
                              "type=\"double\">40.0</property>\n                    </object>\n                </property>\n")));
    variant("v4-duplicate-keys.motrack", duplicate);
    auto emptyUnit = baselineXml;
    emptyUnit.replace("    <property name=\"description\"",
                      "    <property name=\"mass_unit\" type=\"string\"></property>\n    <property name=\"description\"");
    variant("v4-empty-mass-unit.motrack", emptyUnit);

    // Archive-backed documents: one member, and a group of both members of a two-tab archive.
    const auto archive = std::make_shared<motrack::TrackerArchive>(
        motrack::TrackerArchive::open(QString::fromUtf8(MOTRACK_CORPUS_FIXTURES) + "/linked-images/multitab-images.trz"));
    const auto member = [&](const char* name, double mass)
    {
        auto document = motrack::importTrackerDocument(archive->projectPath(name));
        archive->resolveMedia(document.project, name);
        document.archivePayload = archive;
        document.archiveMember = name;
        motrack::TrackEdits edits;
        EXPECT_TRUE(edits.setPointMassValue(document.project, 0, mass));
        return document;
    };
    motrack::writeWorkspaceDocument(QDir(target).filePath("v5-archive.motrack"), member("experiment.trk", 5.0));
    auto group = motrack::makeArchiveGroup(member("experiment.trk", 6.0));
    ASSERT_FALSE(motrack::appendArchiveGroupDocument(group, member("second.trk", 7.0)).isNull());
    motrack::writeArchiveGroup(QDir(target).filePath("v6-group.motrack"), group);

    for (const auto& name : nativeFixtures())
    {
        for (const auto& [file, bytes] : nativeGoldens(name))
        {
            write(file, bytes);
        }
    }
}

} // namespace
