#include "workspace_document.hpp"

#include "archive_fixture.hpp"
#include "main_window.hpp"
#include "measurements.hpp"
#include "paired_save.hpp"
#include "track_edits.hpp"
#include "tracker_archive.hpp"
#include "tracker_import.hpp"
#include "tracker_writer.hpp"
#include "workspace_storage.hpp"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QStatusBar>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace
{

/// A Tracker document in OSP's line layout with CRLF endings, a prolog comment and content Tracker ignores.
QByteArray sourceXml()
{
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
           "<!-- retain spelling, whitespace and encoding -->\r\n"
           "<object class=\"org.opensourcephysics.cabrillo.tracker.TrackerPanel\">\r\n"
           " <property name=\"description\" type=\"string\"><![CDATA[caf\xC3\xA9 & data]]></property>\r\n"
           " <property name=\"future\" type=\"object\">\r\n"
           " <object class=\"FutureSettings\">\r\n"
           "  <property name=\"nested\" type=\"string\"><![CDATA[<unknown>]]></property>\r\n"
           " </object>\r\n"
           " </property>\r\n"
           " <property name=\"tracks\" type=\"collection\" class=\"java.util.ArrayList\">\r\n"
           "  <property name=\"item\" type=\"object\">\r\n"
           "  <object class=\"FutureTrack\">\r\n"
           "   <property name=\"link\" type=\"string\">same name</property>\r\n"
           "  </object>\r\n"
           "  </property>\r\n"
           " </property>\r\n"
           "</object>\r\n";
}

QByteArray boundXml()
{
    return R"(<?xml version="1.0"?>
      <object class="org.opensourcephysics.cabrillo.tracker.TrackerPanel">
        <!-- Original names and positions must not be used as editable identity. -->
        <property name="tracks" type="collection" class="java.util.ArrayList">
          <property name="[10]" type="object">
            <object class="org.opensourcephysics.cabrillo.tracker.PointMass">
              <property name="name" type="string">same</property>
            </object>
          </property>
          <property name="[20]" type="object">
            <object class="Uneditable">
              <property name="custom" type="string">keep</property>
            </object>
          </property>
          <property name="[30]" type="object">null</property>
          <property name="[40]" type="object">
            <object class="org.opensourcephysics.cabrillo.tracker.Vector">
              <property name="name" type="string">same</property>
            </object>
          </property>
          <property name="[50]" type="object">
            <object class="org.opensourcephysics.cabrillo.tracker.PointMass">
              <property name="name" type="string">same</property>
            </object>
          </property>
        </property>
      </object>)";
}

bool writeBytes(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QByteArray readBytes(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        return {};
    }
    return file.readAll();
}

TEST(WorkspaceDocument, RecoversTimeUnitFromOlderNativeSourceAndPersistsLaterEdits)
{
    QTemporaryDir temporary;
    const auto path = temporary.filePath("time-unit.motrack");
    for (const bool source : {false, true})
    {
        motrack::WorkspaceDocument document;
        if (source)
        {
            const auto imported = motrack::parseTrackerProject("<object class=\"org.opensourcephysics.cabrillo.tracker.TrackerPanel\">\n"
                                                               "    <property name=\"time_unit\" type=\"string\">tick</property>\n"
                                                               "</object>\n");
            document.project = imported.project;
            document.trackerSource = imported.source;
        }
        motrack::writeWorkspaceDocument(path, document);
        auto old = QJsonDocument::fromJson(readBytes(path)).object();
        old.remove("timeUnit");
        ASSERT_TRUE(writeBytes(path, QJsonDocument(old).toJson()));
        auto reopened = motrack::readWorkspaceDocument(path);
        EXPECT_EQ(reopened.project.timeUnit, source ? "tick" : "s");
        reopened.project.timeUnit = "beat";
        motrack::writeWorkspaceDocument(path, reopened);
        EXPECT_EQ(motrack::readWorkspaceDocument(path).project.timeUnit, "beat");
        old.insert("timeUnit", 42);
        ASSERT_TRUE(writeBytes(path, QJsonDocument(old).toJson()));
        EXPECT_THROW((void)motrack::readWorkspaceDocument(path), motrack::ProjectError);
    }
}

TEST(WorkspaceDocument, RetainsEncodedUnknownXmlAfterImportRemovalAndRepeatedSave)
{
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const auto original = temporary.filePath(QString::fromUtf8("caf\xC3\xA9.trk"));
    const auto native = temporary.filePath("experiment.motrack");
    const auto xml = sourceXml();
    ASSERT_TRUE(writeBytes(original, xml));
    auto document = motrack::importTrackerDocument(original);
    if (!document.trackerSource.has_value())
    {
        FAIL() << "Expected preserved Tracker source.";
    }
    EXPECT_EQ(document.trackerSource->xml, xml.toStdString());
    EXPECT_EQ(document.project.description, "caf\xC3\xA9 & data");
    EXPECT_EQ(document.project.unsupportedTrackTypes, std::vector<std::string>{"FutureTrack"});
    document.project.description = "Edited in MoTrack";
    motrack::writeWorkspaceDocument(native, document);
    ASSERT_TRUE(QFile::remove(original));
    auto reopened = motrack::readWorkspaceDocument(native);
    if (!reopened.trackerSource.has_value())
    {
        FAIL() << "Expected preserved Tracker source.";
    }
    EXPECT_EQ(reopened.trackerSource->xml, xml.toStdString());
    EXPECT_EQ(reopened.project.description, "Edited in MoTrack");
    motrack::writeWorkspaceDocument(native, reopened);
    const auto resaved = motrack::readWorkspaceDocument(native);
    if (!resaved.trackerSource.has_value())
    {
        FAIL() << "Expected preserved Tracker source.";
    }
    EXPECT_EQ(resaved.trackerSource->xml, xml.toStdString());
}

TEST(WorkspaceDocument, MigratesVersionOneWithoutInventingSourceContent)
{
    QTemporaryDir temporary;
    const auto path = temporary.filePath("old.motrack");
    motrack::WorkspaceDocument document;
    document.project.description = "Old native document";
    motrack::writeWorkspaceDocument(path, document);
    auto object = QJsonDocument::fromJson(readBytes(path)).object();
    EXPECT_EQ(object.value("version").toInt(), 5);
    object.insert("version", 1);
    object.remove("trackerSource");
    object.remove("nextTrackId");
    object.remove("embeddedArchive");
    object.remove("archiveMember");
    ASSERT_TRUE(writeBytes(path, QJsonDocument(object).toJson()));
    const auto migrated = motrack::readWorkspaceDocument(path);
    EXPECT_FALSE(migrated.trackerSource);
    EXPECT_EQ(migrated.project.description, document.project.description);
    motrack::writeWorkspaceDocument(path, migrated);
    const auto updated = QJsonDocument::fromJson(readBytes(path)).object();
    EXPECT_EQ(updated.value("version").toInt(), 5);
    EXPECT_TRUE(updated.value("trackerSource").isNull());
}

TEST(WorkspaceDocument, RejectsMalformedOrAmbiguousSourceEnvelopes)
{
    QTemporaryDir temporary;
    const auto path = temporary.filePath("invalid.motrack");
    motrack::writeWorkspaceDocument(path, {});
    const auto valid = QJsonDocument::fromJson(readBytes(path)).object();
    const auto encoded = QString::fromLatin1(sourceXml().toBase64());
    const std::vector<QJsonValue> invalid{
        QJsonValue::Undefined,
        true,
        "xml",
        QJsonObject{},
        QJsonObject{{"xmlBase64", 1}},
        QJsonObject{{"xmlBase64", "%%%="}},
        QJsonObject{{"xmlBase64", encoded + "\n"}},
        QJsonObject{{"xmlBase64", QString::fromUtf8("\xE2\x98\x83")}},
        QJsonObject{{"xmlBase64", QString::fromLatin1(QByteArray("<object/>").toBase64())}, {"bindings", QJsonValue::Null}},
        // MoTrack reads only UTF-8 Tracker XML (OSP ignores a declared encoding and decodes UTF-8 itself).
        QJsonObject{
            {"xmlBase64", QString::fromLatin1(QByteArray(sourceXml()).replace("encoding=\"UTF-8\"", "encoding=\"ISO-8859-1\"").toBase64())},
            {"bindings", QJsonValue::Null}}};
    for (const auto& source : invalid)
    {
        auto object = valid;
        object.insert("trackerSource", source);
        ASSERT_TRUE(writeBytes(path, QJsonDocument(object).toJson()));
        EXPECT_THROW(static_cast<void>(motrack::readWorkspaceDocument(path)), motrack::ProjectError);
    }
    auto object = valid;
    object.insert("version", 1);
    ASSERT_TRUE(writeBytes(path, QJsonDocument(object).toJson()));
    EXPECT_THROW(static_cast<void>(motrack::readWorkspaceDocument(path)), motrack::ProjectError);
    object.insert("version", 6);
    EXPECT_TRUE(writeBytes(path, QJsonDocument(object).toJson()));
    EXPECT_THROW(static_cast<void>(motrack::readWorkspaceDocument(path)), motrack::ProjectError);
}

TEST(WorkspaceDocument, MigratesOldIdentitiesAndKeepsThemAcrossEditedNativeReopens)
{
    for (const int version : {1, 2, 3, 4})
    {
        SCOPED_TRACE(version);
        QTemporaryDir temporary;
        const auto path = temporary.filePath("identities.motrack");
        motrack::WorkspaceDocument document;
        document.trackerSource = motrack::TrackerSource{.xml = sourceXml().toStdString()};
        motrack::TrackEdits edits;
        ASSERT_TRUE(edits.createPointMass(document.project, "same"));
        ASSERT_TRUE(edits.createPointMass(document.project, "same"));
        ASSERT_TRUE(edits.createVector(document.project, "same"));
        motrack::writeWorkspaceDocument(path, document);
        auto old = QJsonDocument::fromJson(readBytes(path)).object();
        old.insert("version", version);
        old.remove("embeddedArchive");
        old.remove("archiveMember");
        if (version < 3)
        {
            old.remove("nextTrackId");
        }
        if (version == 1)
        {
            old.remove("trackerSource");
        }
        else if (version < 4)
        {
            auto source = old.value("trackerSource").toObject();
            source.remove("bindings");
            old.insert("trackerSource", source);
        }
        for (const auto* key : {"pointMasses", "vectors"})
        {
            QJsonArray tracks;
            for (const auto& entry : old.value(key).toArray())
            {
                auto track = entry.toObject();
                if (version < 3)
                {
                    track.remove("id");
                }
                tracks.append(track);
            }
            old.insert(key, tracks);
        }
        ASSERT_TRUE(writeBytes(path, QJsonDocument(old).toJson()));
        auto migrated = motrack::readWorkspaceDocument(path);
        EXPECT_EQ(migrated.trackerSource.has_value(), version >= 2);
        if (migrated.trackerSource)
        {
            EXPECT_FALSE(migrated.trackerSource->bindings);
        }
        const auto first = migrated.project.pointMasses.front().id;
        const auto second = migrated.project.pointMasses.back().id;
        const auto vector = migrated.project.vectors.front().id;
        EXPECT_NE(first, second);
        EXPECT_NE(second, vector);
        EXPECT_GT(first, 0u);
        edits.clear();
        ASSERT_TRUE(edits.removePointMass(migrated.project, 0));
        ASSERT_TRUE(edits.renamePointMass(migrated.project, 0, "renamed"));
        ASSERT_TRUE(edits.removeVector(migrated.project, 0));
        motrack::writeWorkspaceDocument(path, migrated);
        auto reopened = motrack::readWorkspaceDocument(path);
        EXPECT_EQ(reopened.project.pointMasses.front().id, second);
        EXPECT_EQ(reopened.project.pointMasses.front().name, "renamed");
        edits.clear();
        ASSERT_TRUE(edits.createVector(reopened.project, "new"));
        const auto newVector = reopened.project.vectors.front().id;
        EXPECT_GT(newVector, vector);
        motrack::writeWorkspaceDocument(path, reopened);
        reopened = motrack::readWorkspaceDocument(path);
        EXPECT_EQ(reopened.project.vectors.front().id, newVector);
        EXPECT_GT(reopened.project.nextTrackId, newVector);
    }
}

TEST(WorkspaceDocument, RetainsSourceBindingsAfterRenameReorderDeleteUndoAndReopen)
{
    QTemporaryDir temporary;
    const auto original = temporary.filePath("original.trk");
    const auto native = temporary.filePath("bound.motrack");
    ASSERT_TRUE(writeBytes(original, boundXml()));
    auto document = motrack::importTrackerDocument(original);
    if (!document.trackerSource || !document.trackerSource->bindings)
    {
        FAIL() << "Expected complete import bindings.";
    }
    const auto expected = *document.trackerSource->bindings;
    ASSERT_EQ(expected.size(), 3u);
    EXPECT_EQ(expected.at(0).sourceIndex, 0u);
    EXPECT_EQ(expected.at(1).sourceIndex, 3u);
    EXPECT_EQ(expected.at(2).sourceIndex, 4u);
    EXPECT_EQ(expected.at(0).trackId, document.project.pointMasses.front().id);
    EXPECT_EQ(expected.at(1).trackId, document.project.vectors.front().id);
    EXPECT_EQ(expected.at(2).trackId, document.project.pointMasses.back().id);
    std::swap(document.project.pointMasses.front(), document.project.pointMasses.back());
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.renamePointMass(document.project, 0, "renamed"));
    ASSERT_TRUE(edits.removePointMass(document.project, 1));
    ASSERT_TRUE(edits.undo(document.project));
    ASSERT_TRUE(edits.redo(document.project));
    ASSERT_TRUE(edits.removeVector(document.project, 0));
    ASSERT_TRUE(edits.createVector(document.project, "same"));
    EXPECT_GT(document.project.vectors.front().id, expected.at(2).trackId);
    motrack::writeWorkspaceDocument(native, document);
    auto versionFour = QJsonDocument::fromJson(readBytes(native)).object();
    versionFour.insert("version", 4);
    versionFour.remove("embeddedArchive");
    versionFour.remove("archiveMember");
    ASSERT_TRUE(writeBytes(native, QJsonDocument(versionFour).toJson()));
    ASSERT_TRUE(QFile::remove(original));
    auto reopened = motrack::readWorkspaceDocument(native);
    if (!reopened.trackerSource || !reopened.trackerSource->bindings)
    {
        FAIL() << "Expected persisted bindings.";
    }
    EXPECT_EQ(*reopened.trackerSource->bindings, expected);
    EXPECT_EQ(reopened.trackerSource->xml, boundXml().toStdString());
    EXPECT_EQ(reopened.project.pointMasses.front().id, expected.at(2).trackId);
    motrack::writeWorkspaceDocument(native, reopened);
    reopened = motrack::readWorkspaceDocument(native);
    if (!reopened.trackerSource || !reopened.trackerSource->bindings)
    {
        FAIL() << "Expected resaved bindings.";
    }
    EXPECT_EQ(*reopened.trackerSource->bindings, expected);
}

TEST(WorkspaceDocument, RejectsIncompleteAmbiguousAndWrongTypeSourceBindings)
{
    QTemporaryDir temporary;
    const auto original = temporary.filePath("original.trk");
    const auto native = temporary.filePath("bound.motrack");
    ASSERT_TRUE(writeBytes(original, boundXml()));
    const auto document = motrack::importTrackerDocument(original);
    motrack::writeWorkspaceDocument(native, document);
    const auto bytes = readBytes(native);
    const auto root = QJsonDocument::fromJson(bytes).object();
    const auto source = root.value("trackerSource").toObject();
    const auto bindings = source.value("bindings").toArray();
    const auto rejects = [&](const QJsonValue& invalid)
    {
        auto changed = root;
        auto envelope = source;
        envelope.insert("bindings", invalid);
        changed.insert("trackerSource", envelope);
        ASSERT_TRUE(writeBytes(native, QJsonDocument(changed).toJson()));
        EXPECT_THROW(static_cast<void>(motrack::readWorkspaceDocument(native)), motrack::ProjectError);
    };
    // Bindings that contradict only the source's track objects are dropped on open, as stale bindings recorded by
    // MoTrack's former DOM reader are (RecoversStaleSourceBindingsAndExplainsUnreadableSources).
    const auto recovers = [&](const QJsonValue& stale)
    {
        auto changed = root;
        auto envelope = source;
        envelope.insert("bindings", stale);
        changed.insert("trackerSource", envelope);
        ASSERT_TRUE(writeBytes(native, QJsonDocument(changed).toJson()));
        const auto reopened = motrack::readWorkspaceDocument(native);
        EXPECT_TRUE(reopened.trackerBindingsReset);
        EXPECT_TRUE(reopened.trackerSource && !reopened.trackerSource->bindings);
    };
    rejects(QJsonValue::Undefined);
    rejects(true);
    recovers(QJsonArray{});
    rejects(QJsonArray{bindings.at(0), bindings.at(0), bindings.at(2)});
    for (const auto& replacement : std::vector<QJsonObject>{QJsonObject{{"trackId", "1"}, {"sourceIndex", 1}, {"kind", "pointMass"}},
                                                            QJsonObject{{"trackId", "1"}, {"sourceIndex", 999}, {"kind", "pointMass"}}})
    {
        auto stale = bindings;
        stale.replace(0, replacement);
        recovers(stale);
    }
    for (const auto& replacement : std::vector<QJsonObject>{QJsonObject{},
                                                            QJsonObject{{"trackId", "1"}, {"sourceIndex", -1}, {"kind", "pointMass"}},
                                                            QJsonObject{{"trackId", "1"}, {"sourceIndex", 0.5}, {"kind", "pointMass"}},
                                                            QJsonObject{{"trackId", "1"}, {"sourceIndex", 0}, {"kind", "vector"}},
                                                            QJsonObject{{"trackId", "2"}, {"sourceIndex", 0}, {"kind", "pointMass"}},
                                                            QJsonObject{{"trackId", "4"}, {"sourceIndex", 0}, {"kind", "pointMass"}},
                                                            QJsonObject{{"trackId", "1"}, {"sourceIndex", 0}, {"kind", "unknown"}}})
    {
        auto invalid = bindings;
        invalid.replace(0, replacement);
        rejects(invalid);
    }
    ASSERT_TRUE(writeBytes(native, bytes));
    auto invalid = document;
    if (!invalid.trackerSource || !invalid.trackerSource->bindings)
    {
        FAIL() << "Expected import bindings.";
    }
    invalid.trackerSource->bindings->pop_back();
    EXPECT_THROW(motrack::writeWorkspaceDocument(native, invalid), motrack::ProjectError);
    EXPECT_EQ(readBytes(native), bytes);
    invalid = document;
    invalid.project.pointMasses.clear();
    invalid.project.vectors.clear();
    invalid.project.nextTrackId = 1; // Deleted IDs must stay reserved for their source objects.
    EXPECT_THROW(motrack::writeWorkspaceDocument(native, invalid), motrack::ProjectError);
    EXPECT_EQ(readBytes(native), bytes);
}

TEST(WorkspaceDocument, RecoversStaleSourceBindingsAndExplainsUnreadableSources)
{
    // A native file saved before B1 holds bindings that MoTrack's former DOM reader recorded. OSP's line reader,
    // like Tracker, ends the tracks collection at a blank line, so it sees fewer track objects than they name.
    QTemporaryDir temporary;
    const auto original = temporary.filePath("original.trk");
    const auto native = temporary.filePath("bound.motrack");
    ASSERT_TRUE(writeBytes(original, boundXml()));
    const auto document = motrack::importTrackerDocument(original);
    motrack::writeWorkspaceDocument(native, document);
    const auto root = QJsonDocument::fromJson(readBytes(native)).object();
    const auto withSource = [&](const QByteArray& xml)
    {
        auto changed = root;
        auto envelope = root.value("trackerSource").toObject();
        envelope.insert("xmlBase64", QString::fromLatin1(xml.toBase64()));
        changed.insert("trackerSource", envelope);
        return writeBytes(native, QJsonDocument(changed).toJson());
    };
    auto blankLine = boundXml();
    blankLine.replace(R"(          <property name="[50]")",
                      "\n"
                      R"(          <property name="[50]")");
    ASSERT_TRUE(withSource(blankLine));
    const auto recovered = motrack::readWorkspaceDocument(native);
    EXPECT_TRUE(recovered.trackerBindingsReset);
    if (!recovered.trackerSource)
    {
        FAIL() << "The preserved source was dropped.";
    }
    EXPECT_FALSE(recovered.trackerSource->bindings);
    EXPECT_EQ(recovered.trackerSource->xml, blankLine.toStdString());
    EXPECT_EQ(recovered.project.pointMasses.size(), document.project.pointMasses.size());
    EXPECT_EQ(recovered.project.vectors.size(), document.project.vectors.size());

    // Standard export refuses instead of guessing associations; a native save keeps measurements and source.
    try
    {
        motrack::writeTrackerDocument(temporary.filePath("export.trk"), recovered, {});
        ADD_FAILURE() << "Standard export guessed the dropped associations.";
    }
    catch (const motrack::ProjectError& error)
    {
        EXPECT_STREQ(error.what(), "Tracker export requires original object associations.");
    }
    const auto resaved = temporary.filePath("resaved.motrack");
    motrack::writeWorkspaceDocument(resaved, recovered);
    const auto reopened = motrack::readWorkspaceDocument(resaved);
    EXPECT_FALSE(reopened.trackerBindingsReset);
    EXPECT_TRUE(reopened.trackerSource && !reopened.trackerSource->bindings && reopened.trackerSource->xml == blankLine.toStdString());

    // The window says so persistently, and the note goes away with the next document.
    motrack::MainWindow window;
    ASSERT_TRUE(window.loadProject(native));
    const auto* notice = window.findChild<QLabel*>("sourceNotice");
    ASSERT_NE(notice, nullptr);
    EXPECT_FALSE(notice->isHidden());
    EXPECT_TRUE(notice->text().startsWith("Tracker export unavailable:")) << notice->text().toStdString();
    ASSERT_TRUE(window.loadProject(resaved));
    EXPECT_TRUE(notice->isHidden());

    // A source the current reader refuses is explained rather than reported as bare XML. Here a blank line ends the
    // collection, so the null element that follows belongs to the panel, which Tracker refuses too (#196).
    auto refused = boundXml();
    refused.replace(R"(          <property name="[30]")",
                    "\n"
                    R"(          <property name="[30]")");
    ASSERT_TRUE(withSource(refused));
    try
    {
        static_cast<void>(motrack::readWorkspaceDocument(native));
        ADD_FAILURE() << "An unreadable preserved source was accepted.";
    }
    catch (const motrack::ProjectError& error)
    {
        EXPECT_TRUE(std::string_view(error.what())
                        .starts_with("The copy of the original Tracker file kept in this MoTrack project can no longer be read (Invalid "
                                     "Tracker XML"))
            << error.what();
    }
    EXPECT_FALSE(window.loadProject(native));
    EXPECT_TRUE(window.statusBar()->currentMessage().startsWith("Could not open this project: The copy of the original Tracker file"))
        << window.statusBar()->currentMessage().toStdString();
}

std::vector<int> markFrames(const motrack::PointMass& track)
{
    std::vector<int> result;
    result.reserve(track.marks.size());
    for (const auto& mark : track.marks)
        result.push_back(mark.frame);
    return result;
}

QString preFixFixture(const char* name)
{
    // Saved by MoTrack's reader before #313/#314 (origin/train-3 cc25638), which wrote no `trackerReading`.
    return QString::fromUtf8(MOTRACK_FIXTURES) + "/" + name;
}

TEST(WorkspaceDocument, MigratesValuesTheFirstReaderSavedForFrameShiftsAndDuplicateKeys)
{
    // The preserved source saves frameshift 2, a clip from frame 3, marks at [2] [3] [5] [6] and [4] [7], and two [0]
    // coordinate keys. The first reader stored the marks at those indices, the clip from 3 and no calibration. Tracker
    // shows the marks two frames earlier, the clip from frame 1 and the last [0] key.
    const auto path = preFixFixture("pre-fix-shifted-duplicate-coords.motrack");
    const auto stored = QJsonDocument::fromJson(readBytes(path)).object();
    ASSERT_FALSE(stored.contains("trackerReading"));
    ASSERT_EQ(stored.value("clip").toObject().value("firstFrame"), 3);
    ASSERT_TRUE(stored.value("calibration").isNull());
    const auto document = motrack::readWorkspaceDocument(path);
    EXPECT_FALSE(document.trackerReadingOutdated);
    EXPECT_TRUE(document.trackerReadingMigrated);
    EXPECT_EQ(document.trackerReading, motrack::CURRENT_TRACKER_READING);
    ASSERT_TRUE(document.trackerSource && document.trackerSource->bindings);
    const auto& project = document.project;
    EXPECT_EQ(project.clip.firstFrame, 1);
    ASSERT_EQ(project.pointMasses.size(), 2u);
    EXPECT_EQ(project.pointMasses[0].marks,
              (std::vector<motrack::Mark>{{.frame = 0, .x = 30, .y = 30},
                                          {.frame = 1, .x = 40, .y = 35},
                                          {.frame = 3, .x = 60, .y = 45},
                                          {.frame = 4, .x = 70, .y = 50}}));
    EXPECT_EQ(markFrames(project.pointMasses[1]), (std::vector<int>{2, 5}));
    if (!project.calibration)
    {
        FAIL() << "The calibration the first reader dropped was not migrated.";
    }
    EXPECT_EQ(
        project.calibration->keyframes,
        (std::vector<motrack::CalibrationKey>{{.frame = 0, .originX = 100, .originY = 50, .angleDegrees = 0, .scaleX = 5, .scaleY = 10}}));
    const auto world = motrack::worldPosition(*project.calibration, project.pointMasses[0].marks.front());
    ASSERT_TRUE(world);
    EXPECT_NEAR(world->x, (30.0 - 100.0) / 5.0, 1e-12);
    EXPECT_NEAR(world->y, (50.0 - 30.0) / 10.0, 1e-12);
    // Nothing else changed: the migrated document equals a fresh import of its preserved source.
    EXPECT_EQ(project, motrack::parseLegacyProject(document.trackerSource->xml));

    // Standard export copies the source unchanged, and an edit keeps the frame shift and the saved indices.
    EXPECT_EQ(motrack::renderTrackerProject(project, *document.trackerSource), document.trackerSource->xml);
    QTemporaryDir temporary;
    auto edited = document;
    edited.project.pointMasses[1].mass = 2.5;
    const auto output = temporary.filePath("migrated.trk");
    motrack::writeTrackerDocument(output, edited, {});
    const auto saved = readBytes(output);
    EXPECT_TRUE(saved.contains(R"(<property name="frameshift" type="int">2</property>)"));
    EXPECT_TRUE(saved.contains(R"(<property name="[7]" type="object">)"));
    EXPECT_EQ(motrack::parseLegacyProject(saved.toStdString()), edited.project);

    // A native save records the current reading, so its values are taken as they are on the next open.
    const auto native = temporary.filePath("migrated.motrack");
    motrack::writeWorkspaceDocument(native, document);
    EXPECT_EQ(QJsonDocument::fromJson(readBytes(native)).object().value("trackerReading"),
              static_cast<int>(motrack::CURRENT_TRACKER_READING));
    const auto reopened = motrack::readWorkspaceDocument(native);
    EXPECT_EQ(reopened.project, project);
    EXPECT_FALSE(reopened.trackerReadingOutdated);
    EXPECT_FALSE(reopened.trackerReadingMigrated);

    // The window says once that the measurements changed; the saved project opens without the note.
    motrack::MainWindow window;
    ASSERT_TRUE(window.loadProject(path));
    const auto* notice = window.findChild<QLabel*>("sourceNotice");
    ASSERT_NE(notice, nullptr);
    EXPECT_FALSE(notice->isHidden());
    EXPECT_TRUE(notice->text().startsWith("Measurements an earlier MoTrack saved were updated")) << notice->text().toStdString();
    ASSERT_TRUE(window.loadProject(native));
    EXPECT_TRUE(notice->isHidden());
}

TEST(WorkspaceDocument, KeepsEditedOrLossyFirstReadingValuesAndExplainsWhy)
{
    // An edit after import cannot be told from the first reader's value, so a stored mark that the first reader would
    // not have produced keeps every frame of the document where it was saved.
    auto root = QJsonDocument::fromJson(readBytes(preFixFixture("pre-fix-shifted-duplicate-coords.motrack"))).object();
    auto masses = root.value("pointMasses").toArray();
    auto mass = masses.at(0).toObject();
    auto marks = mass.value("marks").toArray();
    marks.replace(0, QJsonArray{2, 31, 30});
    mass.insert("marks", marks);
    masses.replace(0, mass);
    root.insert("pointMasses", masses);
    QTemporaryDir temporary;
    const auto edited = temporary.filePath("edited.motrack");
    ASSERT_TRUE(writeBytes(edited, QJsonDocument(root).toJson()));
    const auto document = motrack::readWorkspaceDocument(edited);
    EXPECT_TRUE(document.trackerReadingOutdated);
    EXPECT_EQ(document.trackerReading, motrack::TrackerReading::SavedIndices);
    ASSERT_TRUE(document.trackerSource);
    EXPECT_FALSE(document.trackerSource->bindings);
    EXPECT_EQ(document.project.clip.firstFrame, 3);
    EXPECT_EQ(document.project.pointMasses[0].marks.front(), (motrack::Mark{.frame = 2, .x = 31, .y = 30}));
    EXPECT_EQ(markFrames(document.project.pointMasses[0]), (std::vector<int>{2, 3, 5, 6}));
    EXPECT_EQ(markFrames(document.project.pointMasses[1]), (std::vector<int>{4, 7}));
    // Migration is all or nothing: the calibration the first reader dropped stays dropped with the marks.
    EXPECT_FALSE(document.project.calibration.has_value());
    const auto refused = [&](const motrack::WorkspaceDocument& outdated)
    {
        try
        {
            motrack::writeTrackerDocument(temporary.filePath("export.trk"), outdated, {});
            ADD_FAILURE() << "Standard export wrote values of an earlier reading.";
        }
        catch (const motrack::ProjectError& error)
        {
            EXPECT_TRUE(std::string_view(error.what()).starts_with("Tracker export is unavailable: an earlier MoTrack")) << error.what();
        }
        EXPECT_FALSE(QFileInfo::exists(temporary.filePath("export.trk")));
    };
    refused(document);

    // The note persists: a native save keeps the earlier reading, and every open explains it again.
    const auto resaved = temporary.filePath("resaved.motrack");
    motrack::writeWorkspaceDocument(resaved, document);
    EXPECT_EQ(QJsonDocument::fromJson(readBytes(resaved)).object().value("trackerReading"), 0);
    const auto reopened = motrack::readWorkspaceDocument(resaved);
    EXPECT_TRUE(reopened.trackerReadingOutdated);
    EXPECT_EQ(reopened.project, document.project);
    motrack::MainWindow window;
    ASSERT_TRUE(window.loadProject(resaved));
    const auto* notice = window.findChild<QLabel*>("sourceNotice");
    ASSERT_NE(notice, nullptr);
    EXPECT_FALSE(notice->isHidden());
    EXPECT_TRUE(notice->text().startsWith("Tracker export unavailable: an earlier MoTrack")) << notice->text().toStdString();

    // Never drop data: in the corpus frame-shift fixture the shift drops saved [0] and [1] of mass A, all of "dropped"
    // and the vector's [1], so its first-reader values stay as saved.
    const auto lossy = motrack::readWorkspaceDocument(preFixFixture("pre-fix-frame-shift.motrack"));
    EXPECT_TRUE(lossy.trackerReadingOutdated);
    EXPECT_EQ(lossy.project.clip.firstFrame, 3);
    ASSERT_EQ(lossy.project.pointMasses.size(), 3u);
    EXPECT_EQ(markFrames(lossy.project.pointMasses[0]), (std::vector<int>{0, 1, 2, 3, 5, 7}));
    EXPECT_EQ(markFrames(lossy.project.pointMasses[2]), (std::vector<int>{1}));
    ASSERT_EQ(lossy.project.vectors.size(), 1u);
    EXPECT_EQ(lossy.project.vectors.front().samples.size(), 2u);
    refused(lossy);
    ASSERT_TRUE(window.loadProject(preFixFixture("pre-fix-frame-shift.motrack")));
    EXPECT_FALSE(notice->isHidden());
}

/// A native file as MoTrack's reader of `revision` saved it: its values are that reading of `xml` (optionally edited)
/// and it records that revision, or nothing for revision 0. FirstReaderDocumentsMatchThePreFixFixtures checks this
/// against real pre-fix files.
QString earlierReaderDocument(const QTemporaryDir& directory,
                              const QString& name,
                              const std::string& xml,
                              motrack::TrackerReading revision,
                              const std::function<void(motrack::WorkspaceDocument&)>& edit = {})
{
    const auto earlier = motrack::detail::importTracker(motrack::detail::parseTrackerXml(xml), revision);
    motrack::WorkspaceDocument document{.project = earlier.project,
                                        .movieFrameTimes = {},
                                        .trackerSource = motrack::TrackerSource{.xml = xml, .bindings = earlier.bindings}};
    if (edit)
        edit(document);
    const auto path = directory.filePath(name);
    motrack::writeWorkspaceDocument(path, document);
    auto root = QJsonDocument::fromJson(readBytes(path)).object();
    if (revision == motrack::TrackerReading::SavedIndices)
        root.remove("trackerReading");
    else
        root.insert("trackerReading", static_cast<int>(revision));
    EXPECT_TRUE(writeBytes(path, QJsonDocument(root).toJson()));
    return path;
}

QString firstReaderDocument(const QTemporaryDir& directory,
                            const QString& name,
                            const std::string& xml,
                            const std::function<void(motrack::WorkspaceDocument&)>& edit = {})
{
    return earlierReaderDocument(directory, name, xml, motrack::TrackerReading::SavedIndices, edit);
}
TEST(WorkspaceDocument, FirstReaderDocumentsMatchThePreFixFixtures)
{
    QTemporaryDir temporary;
    for (const auto* name : {"pre-fix-shifted-duplicate-coords.motrack", "pre-fix-frame-shift.motrack"})
    {
        const auto authentic = QJsonDocument::fromJson(readBytes(preFixFixture(name))).object();
        const auto source = QByteArray::fromBase64(authentic.value("trackerSource").toObject().value("xmlBase64").toString().toLatin1());
        const auto reproduced =
            QJsonDocument::fromJson(readBytes(firstReaderDocument(temporary, "first.motrack", source.toStdString()))).object();
        // Every field the pre-fix writer saved is equal; fields added to version 5 since (such as #331's movieTimeBase)
        // are absent from the fixture and must be unset here.
        for (const auto& key : authentic.keys())
            EXPECT_EQ(reproduced.value(key), authentic.value(key)) << name << ' ' << key.toStdString();
        for (const auto& key : reproduced.keys())
            if (!authentic.contains(key))
                EXPECT_TRUE(reproduced.value(key).isNull()) << name << ' ' << key.toStdString();
    }
}

/// A media-free clip of six steps from `start` with frameshift `shift`, then `tracks`, in OSP's line layout.
std::string shiftedSource(int start, int shift, std::string_view tracks)
{
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           "<object class=\"org.opensourcephysics.cabrillo.tracker.TrackerPanel\">\n"
           "    <property name=\"videoclip\" type=\"object\">\n"
           "    <object class=\"org.opensourcephysics.media.core.VideoClip\">\n"
           "        <property name=\"startframe\" type=\"int\">" +
           std::to_string(start) +
           "</property>\n"
           "        <property name=\"stepsize\" type=\"int\">1</property>\n"
           "        <property name=\"stepcount\" type=\"int\">6</property>\n"
           "        <property name=\"frameshift\" type=\"int\">" +
           std::to_string(shift) +
           "</property>\n"
           "    </object>\n"
           "    </property>\n"
           "    <property name=\"tracks\" type=\"collection\" class=\"java.util.ArrayList\">\n" +
           std::string(tracks) +
           "    </property>\n"
           "</object>\n";
}

/// A point mass or vector whose `framedata` holds one element per saved index in `frames` (one digit each).
std::string trackXml(std::string_view kind, std::string_view frames)
{
    const auto type = "org.opensourcephysics.cabrillo.tracker." + std::string(kind);
    std::string result = "        <property name=\"item\" type=\"object\">\n        <object class=\"" + type +
                         "\">\n            <property name=\"framedata\" type=\"array\" class=\"[L" + type + "$FrameData;\">\n";
    const auto value = [&result](std::string_view name, std::string_view number)
    {
        result += R"(                    <property name=")";
        result += name;
        result += R"(" type="double">)";
        result += number;
        result += "</property>\n";
    };
    for (const auto frame : frames)
    {
        const auto index = std::string(1, frame);
        result += R"(                <property name="[)";
        result += index;
        result += R"(]" type="object">)";
        result += "\n                <object class=\"";
        result += type;
        result += "$FrameData\">\n";
        if (kind == "Vector")
        {
            value("xtail", "1.0");
            value("ytail", "2.0");
            value("xcomponent", "3.0");
            value("ycomponent", "4.0");
        }
        else
        {
            value("x", index + ".0");
            value("y", "2.0");
        }
        result += "                </object>\n                </property>\n";
    }
    return result + "            </property>\n        </object>\n        </property>\n";
}

TEST(WorkspaceDocument, MigratesFirstReaderFramesOnlyWhenEverySampleKeepsItsTrackAndStep)
{
    QTemporaryDir temporary;
    // Vectors migrate like marks: saved [2] [4] load at frames 0 and 2, and the clip from 3 starts at 1.
    const auto vectors = motrack::readWorkspaceDocument(
        firstReaderDocument(temporary, "vectors.motrack", shiftedSource(3, 2, trackXml("Vector", "24") + trackXml("PointMass", "35"))));
    EXPECT_FALSE(vectors.trackerReadingOutdated);
    EXPECT_EQ(vectors.project.clip.firstFrame, 1);
    ASSERT_EQ(vectors.project.vectors.size(), 1u);
    EXPECT_EQ(vectors.project.vectors.front().samples.front().frame, 0);
    EXPECT_EQ(vectors.project.vectors.front().samples.back().frame, 2);
    EXPECT_EQ(markFrames(vectors.project.pointMasses.front()), (std::vector<int>{1, 3}));

    // Everything else keeps the stored values and explains why.
    const auto kept = [&](const QString& path, int firstFrame, const std::vector<int>& marks)
    {
        const auto document = motrack::readWorkspaceDocument(path);
        EXPECT_TRUE(document.trackerReadingOutdated) << path.toStdString();
        EXPECT_EQ(document.trackerReading, motrack::TrackerReading::SavedIndices);
        EXPECT_EQ(document.project.clip.firstFrame, firstFrame) << path.toStdString();
        ASSERT_FALSE(document.project.pointMasses.empty());
        EXPECT_EQ(markFrames(document.project.pointMasses.front()), marks) << path.toStdString();
    };
    // The clip start 1 is held at frame 0 while the marks move by 2, so they would leave their steps 1 and 2.
    kept(firstReaderDocument(temporary, "clamped.motrack", shiftedSource(1, 2, trackXml("PointMass", "23"))), 1, {2, 3});
    // A negative shift moves only the clip start, from 1 to 2, so the marks would move from steps 1 and 2.
    kept(firstReaderDocument(temporary, "negative.motrack", shiftedSource(1, -1, trackXml("PointMass", "23"))), 1, {2, 3});
    const auto source = shiftedSource(3, 2, trackXml("PointMass", "35") + trackXml("PointMass", "46"));
    // A track created or deleted after import, or a document without source associations, cannot be verified.
    kept(firstReaderDocument(temporary,
                             "created.motrack",
                             source,
                             [](auto& document)
                             {
                                 auto created = motrack::PointMass{.name = "new", .marks = {{.frame = 4, .x = 1, .y = 1}}, .mass = 1};
                                 created.id = document.project.nextTrackId++;
                                 document.project.pointMasses.push_back(created);
                             }),
         3,
         {3, 5});
    kept(firstReaderDocument(temporary, "deleted.motrack", source, [](auto& document) { document.project.pointMasses.pop_back(); }),
         3,
         {3, 5});
    kept(firstReaderDocument(temporary, "unbound.motrack", source, [](auto& document) { document.trackerSource->bindings.reset(); }),
         3,
         {3, 5});
}

TEST(WorkspaceDocument, KeepsEditsOfTheCurrentReadingAndRejectsUnknownReadings)
{
    // A document written with the current reading is never migrated, edited or not.
    QTemporaryDir temporary;
    const auto original = temporary.filePath("shifted.trk");
    const auto source = motrack::readWorkspaceDocument(preFixFixture("pre-fix-shifted-duplicate-coords.motrack")).trackerSource;
    ASSERT_TRUE(source);
    ASSERT_TRUE(writeBytes(original, QByteArray::fromStdString(source->xml)));
    auto document = motrack::importTrackerDocument(original);
    document.project.pointMasses[0].marks.front().x = 33;
    const auto native = temporary.filePath("edited.motrack");
    motrack::writeWorkspaceDocument(native, document);
    const auto reopened = motrack::readWorkspaceDocument(native);
    EXPECT_FALSE(reopened.trackerReadingOutdated);
    EXPECT_EQ(reopened.project, document.project);
    EXPECT_TRUE(reopened.trackerSource && reopened.trackerSource->bindings);

    const auto root = QJsonDocument::fromJson(readBytes(native)).object();
    for (const auto& invalid : {QJsonValue(-1), QJsonValue("1"), QJsonValue(1.5)})
    {
        auto changed = root;
        changed.insert("trackerReading", invalid);
        ASSERT_TRUE(writeBytes(native, QJsonDocument(changed).toJson()));
        EXPECT_THROW(static_cast<void>(motrack::readWorkspaceDocument(native)), motrack::ProjectError);
    }
    // A revision this build does not know names the newer MoTrack instead of failing as invalid.
    auto newer = root;
    newer.insert("trackerReading", static_cast<int>(motrack::CURRENT_TRACKER_READING) + 1);
    ASSERT_TRUE(writeBytes(native, QJsonDocument(newer).toJson()));
    try
    {
        static_cast<void>(motrack::readWorkspaceDocument(native));
        ADD_FAILURE() << "A newer reading revision was accepted.";
    }
    catch (const motrack::ProjectError& error)
    {
        EXPECT_TRUE(std::string_view(error.what()).starts_with("This project was saved by a newer MoTrack")) << error.what();
    }
}

TEST(WorkspaceDocument, MigratesTrackSlotsOfRevisionOneOnlyWhenNoMarkIsDropped)
{
    QTemporaryDir temporary;
    // Revision 1 kept file order, so [2] then a null [2] left a mark that Java's slots remove: never dropped on open.
    const auto overwritten = std::string(R"(<?xml version="1.0" encoding="UTF-8"?>
<object class="org.opensourcephysics.cabrillo.tracker.TrackerPanel">
    <property name="tracks" type="collection" class="java.util.ArrayList">
        <property name="item" type="object">
        <object class="org.opensourcephysics.cabrillo.tracker.PointMass">
            <property name="framedata" type="array" class="[Lorg.opensourcephysics.cabrillo.tracker.PointMass$FrameData;">
                <property name="[0]" type="object">
                <object class="org.opensourcephysics.cabrillo.tracker.PointMass$FrameData">
                    <property name="x" type="double">0.0</property>
                    <property name="y" type="double">0.0</property>
                </object>
                </property>
                <property name="[2]" type="object">
                <object class="org.opensourcephysics.cabrillo.tracker.PointMass$FrameData">
                    <property name="x" type="double">2.0</property>
                    <property name="y" type="double">2.0</property>
                </object>
                </property>
                <property name="[2]" type="object">null</property>
                <property name="[4]" type="object">
                <object class="org.opensourcephysics.cabrillo.tracker.PointMass$FrameData">
                    <property name="x" type="double">4.0</property>
                    <property name="y" type="double">4.0</property>
                </object>
                </property>
            </property>
        </object>
        </property>
    </property>
</object>
)");
    const auto kept = motrack::readWorkspaceDocument(
        earlierReaderDocument(temporary, "overwritten.motrack", overwritten, motrack::TrackerReading::FrameShift));
    EXPECT_TRUE(kept.trackerReadingOutdated);
    EXPECT_EQ(kept.trackerReading, motrack::TrackerReading::FrameShift);
    EXPECT_EQ(markFrames(kept.project.pointMasses.front()), (std::vector<int>{0, 2, 4}));
    EXPECT_EQ(markFrames(motrack::parseLegacyProject(overwritten).pointMasses.front()), (std::vector<int>{0, 4}));
    // Where the readings agree, a revision-1 file simply takes the current revision and nothing changes.
    const auto agreed = motrack::readWorkspaceDocument(earlierReaderDocument(
        temporary, "agreed.motrack", shiftedSource(3, 2, trackXml("PointMass", "35")), motrack::TrackerReading::FrameShift));
    EXPECT_FALSE(agreed.trackerReadingOutdated);
    EXPECT_FALSE(agreed.trackerReadingMigrated);
    EXPECT_EQ(agreed.trackerReading, motrack::CURRENT_TRACKER_READING);
    EXPECT_EQ(markFrames(agreed.project.pointMasses.front()), (std::vector<int>{1, 3}));
}

TEST(WorkspaceDocument, RejectsInvalidOptionalFrameDurationsBeforeReplacingExistingFile)
{
    QTemporaryDir temporary;
    const auto path = temporary.filePath("duration.motrack");
    motrack::WorkspaceDocument document;
    document.project.clip.frameDurationMs = 125.5;
    motrack::writeWorkspaceDocument(path, document);
    const auto before = readBytes(path);
    EXPECT_EQ(motrack::readWorkspaceDocument(path).project.clip.frameDurationMs, 125.5);
    for (const auto invalid : {std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::infinity(),
                               -std::numeric_limits<double>::infinity(),
                               0.0,
                               -1.0})
    {
        SCOPED_TRACE(invalid);
        document.project.clip.frameDurationMs = invalid;
        EXPECT_THROW(motrack::writeWorkspaceDocument(path, document), motrack::ProjectError);
        EXPECT_TRUE(readBytes(path) == before);
    }
    document.project.clip.frameDurationMs.reset();
    motrack::writeWorkspaceDocument(path, document);
    EXPECT_FALSE(motrack::readWorkspaceDocument(path).project.clip.frameDurationMs);
}

TEST(WorkspaceDocument, RejectsInvalidIdentitiesAndPreservesExistingFile)
{
    QTemporaryDir temporary;
    const auto path = temporary.filePath("identities.motrack");
    motrack::WorkspaceDocument document;
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.createPointMass(document.project, "A"));
    ASSERT_TRUE(edits.createVector(document.project, "A"));
    motrack::writeWorkspaceDocument(path, document);
    const auto valid = readBytes(path);
    const auto root = QJsonDocument::fromJson(valid).object();
    const std::vector<QJsonValue> badIds{QJsonValue::Undefined,
                                         QJsonValue::Null,
                                         1,
                                         "0",
                                         "-1",
                                         "+1",
                                         "01",
                                         "1 ",
                                         "1.0",
                                         "18446744073709551616",
                                         "999999999999999999999999999999999999999999"};
    for (const auto& id : badIds)
    {
        auto invalid = root;
        auto tracks = invalid.value("pointMasses").toArray();
        auto track = tracks.at(0).toObject();
        track.insert("id", id);
        tracks.replace(0, track);
        invalid.insert("pointMasses", tracks);
        ASSERT_TRUE(writeBytes(path, QJsonDocument(invalid).toJson()));
        EXPECT_THROW(static_cast<void>(motrack::readWorkspaceDocument(path)), motrack::ProjectError);
        invalid = root;
        invalid.insert("nextTrackId", id);
        ASSERT_TRUE(writeBytes(path, QJsonDocument(invalid).toJson()));
        EXPECT_THROW(static_cast<void>(motrack::readWorkspaceDocument(path)), motrack::ProjectError);
    }
    for (const auto* counter : {"1", "2"})
    {
        auto invalid = root;
        invalid.insert("nextTrackId", counter);
        ASSERT_TRUE(writeBytes(path, QJsonDocument(invalid).toJson()));
        EXPECT_THROW(static_cast<void>(motrack::readWorkspaceDocument(path)), motrack::ProjectError);
    }
    auto duplicate = root;
    auto vectors = duplicate.value("vectors").toArray();
    auto vector = vectors.at(0).toObject();
    vector.insert("id", "1");
    vectors.replace(0, vector);
    duplicate.insert("vectors", vectors);
    ASSERT_TRUE(writeBytes(path, QJsonDocument(duplicate).toJson()));
    EXPECT_THROW(static_cast<void>(motrack::readWorkspaceDocument(path)), motrack::ProjectError);
    ASSERT_TRUE(writeBytes(path, valid));
    document.project.vectors.front().id = document.project.pointMasses.front().id;
    EXPECT_THROW(motrack::writeWorkspaceDocument(path, document), motrack::ProjectError);
    EXPECT_EQ(readBytes(path), valid);
    document.project.vectors.front().id = 0;
    EXPECT_THROW(motrack::writeWorkspaceDocument(path, document), motrack::ProjectError);
    EXPECT_EQ(readBytes(path), valid);
}

TEST(WorkspaceDocument, PreservesIdentitiesAboveTheExactJsonNumberRange)
{
    QTemporaryDir temporary;
    const auto path = temporary.filePath("large.motrack");
    motrack::WorkspaceDocument document;
    document.project.nextTrackId = std::numeric_limits<motrack::TrackId>::max() - 1;
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.createVector(document.project, "last"));
    motrack::writeWorkspaceDocument(path, document);
    const auto reopened = motrack::readWorkspaceDocument(path);
    EXPECT_EQ(reopened.project.vectors.front().id, document.project.vectors.front().id);
    EXPECT_EQ(reopened.project.nextTrackId, document.project.nextTrackId);
    EXPECT_FALSE(edits.createPointMass(document.project, "no reuse"));
}

TEST(WorkspaceDocument, RejectsInvalidOrOversizedSourceBeforeReplacingExistingFile)
{
    QTemporaryDir temporary;
    const auto path = temporary.filePath("existing.motrack");
    motrack::WorkspaceDocument document;
    motrack::writeWorkspaceDocument(path, document);
    const auto original = readBytes(path);
    document.trackerSource = motrack::TrackerSource{.xml = "not XML"};
    EXPECT_THROW(motrack::writeWorkspaceDocument(path, document), motrack::ProjectError);
    EXPECT_EQ(readBytes(path), original);
    document.trackerSource->xml.assign(motrack::MAX_TRACKER_XML_BYTES + 1, ' ');
    EXPECT_THROW(motrack::writeWorkspaceDocument(path, document), motrack::ProjectError);
    EXPECT_EQ(readBytes(path), original);
}

TEST(WorkspaceDocument, RefusesNativeWritesInsideOwnedArchiveDirectories)
{
    for (const int target : {0, 1, 2, 3})
    {
        SCOPED_TRACE(target);
        QTemporaryDir temporary;
        const auto original = temporary.filePath("original.trz");
        const QByteArray retained("Unchanged archive attachment");
        ASSERT_TRUE(motrack::test::writeArchive(
            original, {{.name = "selected.trk", .bytes = boundXml()}, {.name = "support/retained.motrack", .bytes = retained}}));
        auto payload = std::make_shared<motrack::TrackerArchive>(motrack::TrackerArchive::open(original));
        auto document = motrack::importTrackerDocument(payload->projectPath("selected.trk"));
        document.archivePayload = payload;
        document.archiveMember = "selected.trk";
        const auto originalBytes = readBytes(payload->sourcePath());
        const auto attachment = QDir(payload->root()).filePath("support/retained.motrack");
        const auto inside = target == 0   ? payload->sourcePath()
                            : target == 1 ? attachment
                            : target == 2 ? QDir(QFileInfo(payload->sourcePath()).absolutePath()).filePath("saved.motrack")
                                          : QDir(payload->root()).filePath("support/new.motrack");
        try
        {
            motrack::writeWorkspaceDocument(inside, document);
            FAIL() << "Native saving accepted an owned archive destination.";
        }
        catch (const motrack::ProjectError& error)
        {
            EXPECT_STREQ(error.what(), "Choose a save location outside the imported archive's temporary directories.");
        }
        if (target != 0)
            EXPECT_THROW(static_cast<void>(motrack::writeNativeDocument(inside, document)), motrack::ProjectError);
        EXPECT_EQ(readBytes(payload->sourcePath()), originalBytes);
        EXPECT_EQ(readBytes(attachment), retained);
        if (target >= 2)
            EXPECT_FALSE(QFileInfo::exists(inside));
        const auto alternate = inside.toUpper();
        if (target == 1 && alternate != inside && QFileInfo::exists(alternate))
        {
            EXPECT_THROW(motrack::writeWorkspaceDocument(alternate, document), motrack::ProjectError);
            EXPECT_THROW(static_cast<void>(motrack::writeNativeDocument(alternate, document)), motrack::ProjectError);
            EXPECT_EQ(readBytes(attachment), retained);
        }
        const auto external = temporary.filePath(QString::fromUtf8("saved-élève.motrack"));
        motrack::TrackEdits edits;
        ASSERT_TRUE(edits.setPointMassValue(document.project, 0, 7.0));
        static_cast<void>(motrack::writeNativeDocument(external, document));
        const auto reopened = motrack::readWorkspaceDocument(external);
        EXPECT_EQ(reopened.project, document.project);
        ASSERT_TRUE(reopened.archivePayload);
        EXPECT_EQ(readBytes(reopened.archivePayload->sourcePath()), originalBytes);
        EXPECT_EQ(readBytes(QDir(reopened.archivePayload->root()).filePath("support/retained.motrack")), retained);
    }
}

#ifndef Q_OS_WIN
TEST(WorkspaceDocument, RefusesNativeWritesThroughAliasesIntoOwnedArchiveDirectories)
{
    QTemporaryDir temporary;
    const auto original = temporary.filePath("original.trz");
    const QByteArray retained("Unchanged archive attachment");
    ASSERT_TRUE(motrack::test::writeArchive(
        original, {{.name = "selected.trk", .bytes = boundXml()}, {.name = "support/retained.motrack", .bytes = retained}}));
    const auto payload = std::make_shared<motrack::TrackerArchive>(motrack::TrackerArchive::open(original));
    auto document = motrack::importTrackerDocument(payload->projectPath("selected.trk"));
    document.archivePayload = payload;
    document.archiveMember = "selected.trk";
    const auto originalBytes = readBytes(payload->sourcePath());
    for (const auto& owned : {payload->root(), QFileInfo(payload->sourcePath()).absolutePath()})
    {
        SCOPED_TRACE(owned.toStdString());
        const auto directoryAlias = temporary.filePath("directory-alias");
        ASSERT_TRUE(QFile::link(owned, directoryAlias));
        const auto throughDirectory = QDir(directoryAlias).filePath("new.motrack");
        EXPECT_THROW(motrack::writeWorkspaceDocument(throughDirectory, document), motrack::ProjectError);
        EXPECT_THROW(static_cast<void>(motrack::writeNativeDocument(throughDirectory, document)), motrack::ProjectError);
        EXPECT_FALSE(QFileInfo::exists(QDir(owned).filePath("new.motrack")));
        ASSERT_TRUE(QFile::remove(directoryAlias));
    }
    const auto attachment = QDir(payload->root()).filePath("support/retained.motrack");
    const auto missing = QDir(payload->root()).filePath("support/new.motrack");
    for (const auto& target : {attachment, missing})
    {
        const auto fileAlias = temporary.filePath("file-alias.motrack");
        ASSERT_TRUE(QFile::link(target, fileAlias));
        EXPECT_THROW(motrack::writeWorkspaceDocument(fileAlias, document), motrack::ProjectError);
        EXPECT_THROW(static_cast<void>(motrack::writeNativeDocument(fileAlias, document)), motrack::ProjectError);
        EXPECT_TRUE(QFileInfo(fileAlias).isSymLink());
        EXPECT_EQ(readBytes(attachment), retained);
        EXPECT_FALSE(QFileInfo::exists(missing));
        ASSERT_TRUE(QFile::remove(fileAlias));
    }
    const auto outside = temporary.filePath("outside.motrack");
    const auto validAlias = temporary.filePath("external-alias.motrack");
    ASSERT_TRUE(QFile::link(outside, validAlias));
    motrack::writeWorkspaceDocument(validAlias, document);
    EXPECT_EQ(motrack::readWorkspaceDocument(outside).project, document.project);
    EXPECT_TRUE(QFileInfo(validAlias).isSymLink());
    EXPECT_EQ(readBytes(payload->sourcePath()), originalBytes);
    EXPECT_EQ(readBytes(attachment), retained);
}
#endif

TEST(WorkspaceDocument, RetainsEveryArchiveByteAndSelectedTabAfterOriginalRemoval)
{
    QTemporaryDir temporary;
    const auto original = temporary.filePath(QString::fromUtf8("élève.trz"));
    const auto native = temporary.filePath(QString::fromUtf8("élève.motrack"));
    const QByteArray unknown(100000, '\xA5');
    ASSERT_TRUE(motrack::test::writeArchive(original,
                                            {{.name = "nested/selected.trk", .bytes = boundXml()},
                                             {.name = "other.trk", .bytes = sourceXml()},
                                             {.name = "media/opaque.bin", .bytes = unknown},
                                             {.name = "source.trz", .bytes = "unrelated member with the snapshot filename"}}));
    const auto bytes = readBytes(original);
    auto payload = std::make_shared<motrack::TrackerArchive>(motrack::TrackerArchive::open(original));
    auto document = motrack::importTrackerDocument(payload->projectPath("nested/selected.trk"));
    const auto expectedBindings = document.trackerSource;
    document.archivePayload = payload;
    document.archiveMember = "nested/selected.trk";
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.renamePointMass(document.project, 0, "edited"));
    ASSERT_TRUE(QFile::remove(original));
    motrack::writeWorkspaceDocument(native, document);
    // Release the import session: reopening must obtain an independent payload from the saved file.
    document = {};
    payload.reset();
    auto reopened = motrack::readWorkspaceDocument(native);
    ASSERT_TRUE(reopened.archivePayload);
    EXPECT_TRUE(reopened.archiveReference.empty());
    EXPECT_EQ(reopened.archiveMember, "nested/selected.trk");
    EXPECT_EQ(reopened.project.pointMasses.front().name, "edited");
    if (!expectedBindings || !reopened.trackerSource)
    {
        FAIL() << "Expected preserved original XML and bindings.";
    }
    EXPECT_EQ(reopened.trackerSource->xml, expectedBindings->xml);
    EXPECT_EQ(reopened.trackerSource->bindings, expectedBindings->bindings);
    EXPECT_EQ(readBytes(reopened.archivePayload->sourcePath()), bytes);
    EXPECT_EQ(reopened.archivePayload->projects(), (QStringList{"nested/selected.trk", "other.trk"}));
    EXPECT_EQ(readBytes(reopened.archivePayload->projectPath("other.trk")), sourceXml());
    EXPECT_EQ(readBytes(reopened.archivePayload->root() + "/media/opaque.bin"), unknown);
    EXPECT_EQ(readBytes(reopened.archivePayload->root() + "/source.trz"), "unrelated member with the snapshot filename");
    const auto copy = temporary.filePath("resaved.motrack");
    motrack::writeWorkspaceDocument(copy, reopened);
    reopened = {};
    ASSERT_TRUE(QFile::remove(native));
    const auto resaved = motrack::readWorkspaceDocument(copy);
    ASSERT_TRUE(resaved.archivePayload);
    EXPECT_EQ(readBytes(resaved.archivePayload->sourcePath()), bytes);
    EXPECT_EQ(resaved.project.pointMasses.front().name, "edited");
    const std::atomic_bool cancelled{true};
    EXPECT_THROW(static_cast<void>(motrack::readWorkspaceDocument(copy, &cancelled)), motrack::ProjectError);
}

TEST(WorkspaceDocument, CancelledWritesPreserveExistingPlainAndArchiveProjects)
{
    for (const bool archived : {false, true})
    {
        SCOPED_TRACE(archived);
        QTemporaryDir temporary;
        const auto input = temporary.filePath("source.trz");
        ASSERT_TRUE(motrack::test::writeArchive(input, {{.name = "selected.trk", .bytes = boundXml()}}));
        const auto payload = std::make_shared<motrack::TrackerArchive>(motrack::TrackerArchive::open(input));
        auto document = motrack::importTrackerDocument(payload->projectPath("selected.trk"));
        if (archived)
        {
            document.archivePayload = payload;
            document.archiveMember = "selected.trk";
        }
        const auto target = temporary.filePath("saved.motrack");
        motrack::writeWorkspaceDocument(target, document);
        const auto original = readBytes(target);
        motrack::TrackEdits edits;
        ASSERT_TRUE(edits.renamePointMass(document.project, 0, "changed"));
        const std::atomic_bool cancelled{true};
        EXPECT_THROW(motrack::writeWorkspaceDocument(target, document, &cancelled), motrack::ProjectError);
        EXPECT_EQ(readBytes(target), original);
        EXPECT_NE(motrack::readWorkspaceDocument(target).project.pointMasses.front().name, "changed");
    }
}

TEST(WorkspaceDocument, CancellationDuringStorageWritePreservesExistingDestination)
{
    for (const bool archived : {false, true})
    {
        for (const bool duringCopy : {false, true})
        {
            if (duringCopy && !archived)
                continue;
            SCOPED_TRACE(archived);
            SCOPED_TRACE(duringCopy);
            QTemporaryDir temporary;
            const auto input = temporary.filePath("source.trz");
            ASSERT_TRUE(motrack::test::writeArchive(
                input,
                {{.name = "selected.trk", .bytes = boundXml()}, {.name = "notes.dat", .bytes = QByteArray(qsizetype{128} * 1024, 'x')}}));
            const auto payload = std::make_shared<motrack::TrackerArchive>(motrack::TrackerArchive::open(input));
            auto document = motrack::importTrackerDocument(payload->projectPath("selected.trk"));
            if (archived)
            {
                document.archivePayload = payload;
                document.archiveMember = "selected.trk";
            }
            const auto target = temporary.filePath("saved.motrack");
            motrack::writeWorkspaceDocument(target, document);
            const auto original = readBytes(target);
            motrack::TrackEdits edits;
            ASSERT_TRUE(edits.renamePointMass(document.project, 0, "changed"));
            const auto staged = temporary.filePath("staged.motrack");
            motrack::writeWorkspaceDocument(staged, document);
            const auto storage = motrack::readWorkspaceStorage(staged, nullptr);
            std::atomic_bool cancelled{false};
            int chunks = 0;
            bool readyToCommit = false;
            const auto progress = [&](motrack::WorkspaceWriteProgress state)
            {
                if (state == motrack::WorkspaceWriteProgress::ArchiveChunkWritten)
                {
                    ++chunks;
                    if (duringCopy && chunks == 2)
                        cancelled = true;
                }
                else
                {
                    readyToCommit = true;
                    if (!duringCopy)
                        cancelled = true;
                }
            };
            EXPECT_THROW(motrack::writeWorkspaceStorage(target, storage.json, storage.archive, &cancelled, progress),
                         motrack::ProjectError);
            EXPECT_TRUE(cancelled);
            EXPECT_EQ(readyToCommit, !duringCopy);
            if (duringCopy)
                EXPECT_EQ(chunks, 2);
            EXPECT_EQ(readBytes(target), original);
            EXPECT_NE(motrack::readWorkspaceDocument(target).project.pointMasses.front().name, "changed");
        }
    }
}

TEST(WorkspaceDocument, RejectsMismatchedArchiveMetadataAndPreservesPreviousSave)
{
    QTemporaryDir temporary;
    const auto original = temporary.filePath("original.trz");
    const auto native = temporary.filePath("experiment.motrack");
    ASSERT_TRUE(motrack::test::writeArchive(original,
                                            {{.name = "selected.trk", .bytes = boundXml()}, {.name = "other.trk", .bytes = sourceXml()}}));
    auto payload = std::make_shared<motrack::TrackerArchive>(motrack::TrackerArchive::open(original));
    auto document = motrack::importTrackerDocument(payload->projectPath("selected.trk"));
    document.archivePayload = payload;
    document.archiveMember = "selected.trk";
    motrack::writeWorkspaceDocument(native, document);
    const auto saved = readBytes(native);
    const auto storage = motrack::readWorkspaceStorage(native, nullptr);
    const auto metadata = QJsonDocument::fromJson(storage.json).object();
    const auto invalid = temporary.filePath("invalid.motrack");
    const auto rejects = [&](const char* field, const QJsonValue& value)
    {
        auto object = metadata;
        object.insert(field, value);
        motrack::writeWorkspaceStorage(invalid, QJsonDocument(object).toJson(), payload);
        EXPECT_THROW(static_cast<void>(motrack::readWorkspaceDocument(invalid)), motrack::ProjectError);
    };
    for (const auto& value : std::vector<QJsonValue>{QJsonValue::Undefined, false, 1, "true"})
    {
        rejects("embeddedArchive", value);
    }
    for (const auto& value : std::vector<QJsonValue>{QJsonValue::Undefined, 1, "", "../selected.trk", "missing.trk", "other.trk"})
    {
        rejects("archiveMember", value);
    }
    rejects("archive", "original.trz");
    auto old = metadata;
    old.insert("version", 4);
    old.remove("embeddedArchive");
    old.remove("archiveMember");
    motrack::writeWorkspaceStorage(invalid, QJsonDocument(old).toJson(), payload);
    EXPECT_THROW(static_cast<void>(motrack::readWorkspaceDocument(invalid)), motrack::ProjectError);
    // A JSON document cannot promise a payload that is absent from its storage.
    ASSERT_TRUE(writeBytes(invalid, storage.json));
    EXPECT_THROW(static_cast<void>(motrack::readWorkspaceDocument(invalid)), motrack::ProjectError);
    const std::atomic_bool cancelled{true};
    EXPECT_THROW(static_cast<void>(motrack::readWorkspaceDocument(invalid, &cancelled)), motrack::ProjectError);

    for (const auto* member : {"", "missing.trk", "other.trk"})
    {
        document.archiveMember = member;
        EXPECT_THROW(motrack::writeWorkspaceDocument(native, document), motrack::ProjectError);
        EXPECT_EQ(readBytes(native), saved);
    }
    document.archiveMember = "selected.trk";
    document.archiveReference = "original.trz";
    EXPECT_THROW(motrack::writeWorkspaceDocument(native, document), motrack::ProjectError);
    EXPECT_EQ(readBytes(native), saved);
    document.archiveReference.clear();
    document.archivePayload.reset();
    EXPECT_THROW(motrack::writeWorkspaceDocument(native, document), motrack::ProjectError);
    EXPECT_EQ(readBytes(native), saved);
    document.archivePayload = payload;
    ASSERT_TRUE(QFile::remove(payload->sourcePath()));
    EXPECT_THROW(motrack::writeWorkspaceDocument(native, document), motrack::ProjectError);
    EXPECT_EQ(readBytes(native), saved);
}

TEST(WorkspaceDocument, RejectsMalformedNativeBundles)
{
    QTemporaryDir temporary;
    const auto original = temporary.filePath("original.trz");
    const auto native = temporary.filePath("invalid.motrack");
    ASSERT_TRUE(motrack::test::writeArchive(original, {{.name = "selected.trk", .bytes = boundXml()}}));
    motrack::writeWorkspaceDocument(native, {});
    auto metadata = QJsonDocument::fromJson(readBytes(native)).object();
    metadata.insert("embeddedArchive", true);
    const auto json = QJsonDocument(metadata).toJson();
    const auto source = readBytes(original);
    using Entry = motrack::test::ArchiveEntry;
    const std::vector<std::vector<Entry>> invalid{
        {{.name = "project.json", .bytes = json}},
        {{.name = "tracker/source.trz", .bytes = source}},
        {{.name = "project.json", .bytes = json}, {.name = "tracker/source.trz", .bytes = source}, {.name = "unknown", .bytes = "extra"}},
        {{.name = "project.json", .bytes = json}, {.name = "tracker/source.trz", .bytes = source}, {.name = "project.json", .bytes = json}},
        {{.name = "project.json", .bytes = json},
         {.name = "tracker/source.trz", .bytes = source},
         {.name = "tracker/source.trz", .bytes = source}},
        {{.name = "../project.json", .bytes = json}, {.name = "tracker/source.trz", .bytes = source}},
        {{.name = "project.json", .bytes = {}, .symlink = true}, {.name = "tracker/source.trz", .bytes = source}},
        {{.name = "project.json", .bytes = json}, {.name = "tracker/source.trz", .bytes = {}, .symlink = true}},
        {{.name = "project.json", .bytes = json}, {.name = "tracker/source.trz", .bytes = "not an archive"}},
        {{.name = "project.json", .bytes = "not JSON"}, {.name = "tracker/source.trz", .bytes = source}}};
    for (const auto& members : invalid)
    {
        ASSERT_TRUE(motrack::test::writeArchive(native, members));
        EXPECT_THROW(static_cast<void>(motrack::readWorkspaceDocument(native)), motrack::ProjectError);
    }
    ASSERT_TRUE(writeBytes(native, "PK truncated ZIP"));
    EXPECT_THROW(static_cast<void>(motrack::readWorkspaceDocument(native)), motrack::ProjectError);
}

} // namespace
