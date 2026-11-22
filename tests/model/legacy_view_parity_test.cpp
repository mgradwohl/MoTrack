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
    RecordProperty("accepted", static_cast<int>(accepted));
    RecordProperty("refused", static_cast<int>(refused));
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
    const auto view = motrack::trackerSourceView(document);
    if (!view)
    {
        FAIL() << "Expected a source view.";
    }
    EXPECT_EQ(exported(motrack::legacyProject(document), *view), exported(project, imported.source));
}

TEST(LegacyViewParity, ConvertsSourcesWithoutAssociationsToNativeOnlyObjects)
{
    auto imported = importedBaseline();
    imported.source.bindings.reset();
    imported.project.unsupportedTrackTypes.emplace_back("org.example.Placeholder");
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

TEST(LegacyViewParity, KeepsStoredValuesWithStaleAssociationsWhenTheBaselineCannotHoldThemAndRefusesOtherwise)
{
    // Bound values that the baseline's typed import cannot hold (here a calibration removed, which no MoTrack edit
    // does, or bindings naming no track) are kept native-only, with stale associations, as §3 opens any such file.
    auto imported = importedBaseline();
    imported.project.calibration.reset();
    auto document = motrack::typedDocumentFromReduced(imported.project, imported.source);
    EXPECT_EQ(document.associations(), motrack::model::Associations::Stale);
    EXPECT_EQ(motrack::legacyProject(document), imported.project);
    imported = importedBaseline();
    if (!imported.source.bindings)
    {
        FAIL() << "Expected the import's bindings.";
    }
    imported.source.bindings->front().sourceIndex = 99;
    document = motrack::typedDocumentFromReduced(imported.project, imported.source);
    EXPECT_EQ(document.associations(), motrack::model::Associations::Stale);
    const auto view = motrack::trackerSourceView(document);
    if (!view)
    {
        FAIL() << "Expected the preserved source.";
    }
    EXPECT_FALSE(view->bindings);
    // Values no typed document can hold are refused: here identities at or above the counter.
    imported = importedBaseline();
    imported.project.nextTrackId = 1;
    EXPECT_THROW((void)motrack::typedDocumentFromReduced(imported.project, imported.source), motrack::ProjectError);
    EXPECT_THROW((void)motrack::importTypedTracker("<object class=\"Other\"/>\n"), motrack::ProjectError);
    // The reduced model stores samples in frame order; anything else is refused rather than merged.
    motrack::LegacyProject unordered;
    unordered.pointMasses.push_back({.name = "m", .marks = {{.frame = 2, .x = 0, .y = 0}, {.frame = 1, .x = 0, .y = 0}}, .id = 1});
    unordered.nextTrackId = 2;
    EXPECT_THROW((void)motrack::typedDocumentFromReduced(unordered, std::nullopt), motrack::ProjectError);
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

motrack::model::TrackerDocument converted(const motrack::WorkspaceDocument& document)
{
    return motrack::typedDocumentFromReduced(document.project,
                                             document.trackerSource,
                                             document.trackerBindingsReset || document.trackerReadingOutdated,
                                             document.trackerReading);
}

/// The documents of one native fixture, each with the stem of its goldens: one per member for a group.
std::vector<std::pair<QString, motrack::WorkspaceDocument>> nativeDocuments(const QString& name)
{
    std::vector<std::pair<QString, motrack::WorkspaceDocument>> documents;
    const auto stem = name.chopped(QStringLiteral(".motrack").size());
    if (name.startsWith("v6-"))
    {
        for (auto& member : motrack::readArchiveGroup(nativeFixture(name)).members)
        {
            documents.emplace_back(stem + "." + QString::fromStdString(member.member).section('/', -1).chopped(4),
                                   std::move(member.document));
        }
        return documents;
    }
    documents.emplace_back(stem, motrack::readWorkspaceDocument(nativeFixture(name)));
    return documents;
}

/// The goldens of one native fixture: its typed document dump and B1's export, per member for a group.
std::map<QString, QByteArray> nativeGoldens(const QString& name)
{
    std::map<QString, QByteArray> goldens;
    for (const auto& [stem, document] : nativeDocuments(name))
    {
        goldens.emplace(stem + ".document.json", QByteArray::fromStdString(motrack::test::documentDumpText(converted(document))));
        goldens.emplace(stem + ".export.txt", nativeExport(document));
    }
    return goldens;
}

/// Tracker's key frames (PointMass.Loader's reading) of every live point mass, by name.
std::map<std::string, std::vector<std::int32_t>> loadedKeys(const motrack::model::TrackerDocument& document)
{
    std::map<std::string, std::vector<std::int32_t>> keys;
    for (const auto& object : document.objects())
    {
        if (const auto* mass = std::get_if<motrack::model::PointMassState>(object.value.get());
            mass && object.state == motrack::model::ObjectState::Live)
        {
            keys.emplace(motrack::model::loadedName(mass->name), motrack::model::loadedKeyFrames(*mass));
        }
    }
    return keys;
}
const std::vector<QString>& nativeFixtures()
{
    static const std::vector<QString> names{"v1-2dd1d25.motrack",
                                            "v1-f740830.motrack",
                                            "v1-e9599bf.motrack",
                                            "v1-cd710be.motrack",
                                            "v2-ecd43dd.motrack",
                                            "v3-69be876.motrack",
                                            "v4-5367eaf.motrack",
                                            "v4-stale.motrack",
                                            "v4-frame-shift.motrack",
                                            "v4-duplicate-keys.motrack",
                                            "v4-empty-mass-unit.motrack",
                                            "v5-0847a4d.motrack",
                                            "v5-plain.motrack",
                                            "v5-dom-unknown-track.motrack",
                                            "v5-archive.motrack",
                                            "v6-group.motrack"};
    return names;
}

TEST(LegacyViewParity, NativeFixturesOpenToTheirDocumentDumpsAndExports)
{
    int exports = 0;
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
        // The converted document's key frames are the ones B1's export writes, edited, created and unedited alike.
        for (const auto& [stem, document] : nativeDocuments(name))
        {
            SCOPED_TRACE(stem.toStdString());
            const auto exported = nativeExport(document);
            if (exported.startsWith("refused: "))
            {
                continue;
            }
            const auto expected = loadedKeys(motrack::importTypedTracker(exported.toStdString()));
            EXPECT_FALSE(expected.empty());
            EXPECT_EQ(loadedKeys(converted(document)), expected);
            ++exports;
        }
    }
    EXPECT_GE(exports, 8);
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

TEST(LegacyViewParity, ReportsABoundConversionFailureOfAFileThisBuildSaved)
{
    // A save whose stored model the baseline's typed import cannot hold (a calibration removed, which no MoTrack edit
    // does) still converts, stale, but a round-trip check that this build's own save kept its associations fails.
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto source = temporary.filePath("bound.trk");
    ASSERT_TRUE(writeBytes(source, QByteArray::fromStdString(boundSource())));
    const auto native = temporary.filePath("bound.motrack");
    const auto reopen = [&](const motrack::WorkspaceDocument& document)
    {
        motrack::writeWorkspaceDocument(native, document);
        return motrack::readWorkspaceDocument(native);
    };
    const auto imported = motrack::importTrackerDocument(source);
    auto doctored = imported;
    doctored.project.calibration.reset();
    const auto broken = reopen(doctored);
    ASSERT_TRUE(broken.trackerSource && broken.trackerSource->bindings);
    ASSERT_FALSE(broken.trackerBindingsReset || broken.trackerReadingOutdated);
    const auto stale = converted(broken);
    EXPECT_EQ(stale.associations(), motrack::model::Associations::Stale);
    EXPECT_FALSE(motrack::keepsRecordedAssociations(stale, broken.trackerSource, false, broken.trackerReading));
    // An unchanged save keeps its associations; a file whose associations opening dropped, or whose values follow an
    // earlier reading, records none to keep.
    const auto intact = reopen(imported);
    EXPECT_TRUE(motrack::keepsRecordedAssociations(converted(intact), intact.trackerSource, false, intact.trackerReading));
    EXPECT_TRUE(motrack::keepsRecordedAssociations(stale, broken.trackerSource, true, broken.trackerReading));
    EXPECT_TRUE(motrack::keepsRecordedAssociations(stale, broken.trackerSource, false, motrack::TrackerReading::SavedIndices));
    EXPECT_TRUE(motrack::keepsRecordedAssociations(motrack::model::TrackerDocument{}, std::nullopt));
}

// Writes the native fixtures and their goldens. Run once with MOTRACK_WRITE_NATIVE_FIXTURES set to the fixture
// directory, then review every file. The current v5 and v6 files come from this build's native writers. The older
// ones are the same writer's output reduced to the exact key sets of the build that introduced each version or field
// (read with `git show <build>:src/workspace_document.cpp`), so each is shaped as that build wrote it.

/// The keys one native writer wrote: at the top level, in the clip, in each point mass and vector, and in the source.
struct NativeSchema
{
    const char* build;
    int version;
    std::vector<QString> top;
    std::vector<QString> clip;
    std::vector<QString> mass;
    std::vector<QString> vector;
    std::vector<QString> source;
};

const std::vector<NativeSchema>& nativeSchemas()
{
    const std::vector<QString> top1{"format",
                                    "version",
                                    "trackerVersion",
                                    "description",
                                    "lengthUnit",
                                    "unsupportedTrackTypes",
                                    "calibration",
                                    "pointMasses",
                                    "movie",
                                    "movieFrameTimes",
                                    "clip"};
    const std::vector<QString> clip1{"firstFrame", "stepSize", "stepCount", "media", "frameDurationMs", "startTimeMs"};
    const auto with = [](std::vector<QString> keys, std::initializer_list<const char*> more)
    {
        for (const auto* key : more)
        {
            keys.emplace_back(QString::fromLatin1(key));
        }
        return keys;
    };
    const auto top2 = with(top1, {"massUnit", "displayOrientation", "archive"});
    const auto top3 = with(top2, {"alignMovieToClip"});
    const auto top4 = with(top3, {"vectors"});
    const auto top5 = with(top4, {"trackerSource"});
    const auto top6 = with(top5, {"nextTrackId"});
    const auto clip2 = with(clip1, {"sourceFrameCount"});
    static const std::vector<NativeSchema> schemas{
        // v1 as first written: no mass unit, track masses, source frame count, orientation or vectors.
        {.build = "2dd1d25", .version = 1, .top = top1, .clip = clip1, .mass = {"name", "marks"}},
        {.build = "f740830", .version = 1, .top = top2, .clip = clip2, .mass = {"name", "marks", "mass"}},
        {.build = "e9599bf", .version = 1, .top = top3, .clip = clip2, .mass = {"name", "marks", "mass"}},
        {.build = "cd710be", .version = 1, .top = top4, .clip = clip2, .mass = {"name", "marks", "mass"}, .vector = {"name", "samples"}},
        {.build = "ecd43dd",
         .version = 2,
         .top = top5,
         .clip = clip2,
         .mass = {"name", "marks", "mass"},
         .vector = {"name", "samples"},
         .source = {"xmlBase64"}},
        {.build = "69be876",
         .version = 3,
         .top = top6,
         .clip = clip2,
         .mass = {"name", "marks", "mass", "id"},
         .vector = {"name", "samples", "id"},
         .source = {"xmlBase64"}},
        {.build = "5367eaf",
         .version = 4,
         .top = top6,
         .clip = clip2,
         .mass = {"name", "marks", "mass", "id"},
         .vector = {"name", "samples", "id"},
         .source = {"xmlBase64", "bindings"}},
        // v5 as first written: the container fields, but not yet `timeUnit`, `movieTimeBase` (#331) or `trackerReading`
        // (#330), so it counts as reading revision 0.
        {.build = "0847a4d",
         .version = 5,
         .top = with(top6, {"embeddedArchive", "archiveMember"}),
         .clip = clip2,
         .mass = {"name", "marks", "mass", "id"},
         .vector = {"name", "samples", "id"},
         .source = {"xmlBase64", "bindings"}},
    };
    return schemas;
}

const NativeSchema& nativeSchema(std::string_view build)
{
    return *std::ranges::find(nativeSchemas(), build, &NativeSchema::build);
}

QJsonObject only(const QJsonObject& object, const std::vector<QString>& keys)
{
    QJsonObject result;
    for (const auto& key : keys)
    {
        if (object.contains(key))
        {
            result.insert(key, object.value(key));
        }
    }
    return result;
}

QJsonArray onlyEach(const QJsonValue& tracks, const std::vector<QString>& keys)
{
    QJsonArray result;
    for (const auto& track : tracks.toArray())
    {
        result.append(only(track.toObject(), keys));
    }
    return result;
}

/// The current writer's output with exactly the keys `schema`'s build wrote.
QByteArray shaped(const QByteArray& json, const NativeSchema& schema)
{
    const auto current = QJsonDocument::fromJson(json).object();
    auto object = only(current, schema.top);
    object.insert("version", schema.version);
    object.insert("clip", only(current.value("clip").toObject(), schema.clip));
    object.insert("pointMasses", onlyEach(current.value("pointMasses"), schema.mass));
    if (object.contains("vectors"))
    {
        object.insert("vectors", onlyEach(current.value("vectors"), schema.vector));
    }
    if (object.contains("trackerSource") && current.value("trackerSource").isObject())
    {
        object.insert("trackerSource", only(current.value("trackerSource").toObject(), schema.source));
    }
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
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
    const auto v4 = shaped(current, nativeSchema("5367eaf"));
    write("v5-plain.motrack", current);
    write("v5-0847a4d.motrack", shaped(current, nativeSchema("0847a4d")));
    write("v4-5367eaf.motrack", v4);
    // Without associations nothing is exported, so these also hold a vector created with a sample (vectors are stored
    // from cd710be on).
    auto withVector = edited;
    motrack::TrackEdits vectorEdits;
    ASSERT_TRUE(vectorEdits.createVector(withVector.project, "arrow"));
    ASSERT_TRUE(
        vectorEdits.markVector(withVector.project, 1, {.frame = 1, .tailX = 1.0, .tailY = 2.0, .componentX = 3.0, .componentY = 4.0}));
    const auto unassociated = nativeJson(withVector, scratch);
    for (const auto* build : {"2dd1d25", "f740830", "e9599bf", "cd710be", "ecd43dd", "69be876"})
    {
        const auto& schema = nativeSchema(build);
        write(QStringLiteral("v%1-%2.motrack").arg(schema.version).arg(QString::fromLatin1(build)), shaped(unassociated, schema));
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
    write("v4-stale.motrack", withBaseline(v4, blankLine));
    // The same quirk where the bindings still match: the DOM reader saw the coordinate axes, last in `tracks` after a
    // blank line, which OSP's reader leaves out of the tracks. B1 keeps the bindings; the typed document is stale.
    const QByteArray axes = "        <property name=\"item\" type=\"object\">\n            <object "
                            "class=\"org.opensourcephysics.cabrillo.tracker.CoordAxes\">\n                <property name=\"name\" "
                            "type=\"string\">axes</property>\n            </object>\n        </property>\n";
    auto axesLast = baselineXml;
    axesLast.replace(axes, "");
    const QByteArray tracksEnd = "    </property>\n    <property name=\"selectedtrack\"";
    auto axesAtEnd = axesLast;
    axesAtEnd.replace(tracksEnd, QByteArray(axes).append(tracksEnd));
    auto axesBehindBlank = axesLast;
    axesBehindBlank.replace(tracksEnd, QByteArray("\n").append(axes).append(tracksEnd));
    ASSERT_NE(axesAtEnd, axesLast);
    ASSERT_NE(axesBehindBlank, axesAtEnd);
    auto domReading = importBaseline(axesAtEnd);
    editBaseline(domReading.project);
    write("v5-dom-unknown-track.motrack", withBaseline(nativeJson(domReading, scratch), axesBehindBlank));
    // A source the current reader refuses: the blank line ends the panel's tracks before a null element (#196).
    auto refused = baselineXml;
    refused.replace(
        "        <property name=\"item\" type=\"object\">\n            <object class=\"org.opensourcephysics.cabrillo.tracker.CoordAxes\">",
        "\n        <property name=\"item\" type=\"object\">null</property>\n        <property name=\"item\" type=\"object\">\n            "
        "<object class=\"org.opensourcephysics.cabrillo.tracker.CoordAxes\">");
    ASSERT_NE(refused, baselineXml);
    write("v4-refused-baseline.motrack", withBaseline(v4, refused));
    // Sources whose reading the loader-fidelity fixes change (#313-#315), saved as the reader before them (revision 0)
    // read them: unedited imports, which the current reader migrates where that is safe.
    const auto variant = [&](const QString& name, const QByteArray& xml)
    {
        ASSERT_NE(xml, baselineXml);
        auto earlier =
            motrack::detail::importTracker(motrack::detail::parseTrackerXml(xml.toStdString()), motrack::TrackerReading::SavedIndices);
        motrack::WorkspaceDocument document;
        document.project = std::move(earlier.project);
        document.trackerSource = motrack::TrackerSource{.xml = xml.toStdString(), .bindings = std::move(earlier.bindings)};
        document.trackerReading = motrack::TrackerReading::SavedIndices;
        write(name, shaped(nativeJson(document, scratch), nativeSchema("5367eaf")));
    };
    auto shifted = baselineXml;
    shifted.replace(R"(            <property name="startframe" type="int">0</property>)",
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
    emptyUnit.replace(R"(    <property name="description")",
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
