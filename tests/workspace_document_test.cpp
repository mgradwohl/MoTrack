#include "workspace_document.hpp"

#include "track_edits.hpp"

#include <QByteArray>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include <limits>
#include <string>

namespace
{

QByteArray sourceXml()
{
    return "<?xml version='1.0' encoding='ISO-8859-1'?>\r\n"
           "<!-- retain spelling, whitespace and encoding -->\r\n"
           "<object class='org.opensourcephysics.cabrillo.tracker.TrackerPanel'>\r\n"
           " <property name='description'>caf\xE9 &amp; data</property>\r\n"
           " <property name='future'><object class='FutureSettings'><property name='nested'>"
           "<![CDATA[<unknown>]]></property></object></property>\r\n"
           " <property name='tracks'><property name='item'><object class='FutureTrack'>"
           "<property name='link'>same name</property></object></property></property>\r\n"
           "</object>\r\n";
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
    EXPECT_EQ(object.value("version").toInt(), 3);
    object.insert("version", 1);
    object.remove("trackerSource");
    object.remove("nextTrackId");
    ASSERT_TRUE(writeBytes(path, QJsonDocument(object).toJson()));
    const auto migrated = motrack::readWorkspaceDocument(path);
    EXPECT_FALSE(migrated.trackerSource);
    EXPECT_EQ(migrated.project.description, document.project.description);
    motrack::writeWorkspaceDocument(path, migrated);
    const auto updated = QJsonDocument::fromJson(readBytes(path)).object();
    EXPECT_EQ(updated.value("version").toInt(), 3);
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
        QJsonObject{{"xmlBase64", QString::fromLatin1(QByteArray("<object/>").toBase64())}},
        QJsonObject{{"xmlBase64", QString::fromLatin1((QByteArray("<!DOCTYPE object>") + sourceXml()).toBase64())}}};
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
    object.insert("version", 4);
    EXPECT_TRUE(writeBytes(path, QJsonDocument(object).toJson()));
    EXPECT_THROW(static_cast<void>(motrack::readWorkspaceDocument(path)), motrack::ProjectError);
}

TEST(WorkspaceDocument, MigratesOldIdentitiesAndKeepsThemAcrossEditedNativeReopens)
{
    for (const int version : {1, 2})
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
        old.remove("nextTrackId");
        if (version == 1)
        {
            old.remove("trackerSource");
        }
        for (const auto* key : {"pointMasses", "vectors"})
        {
            QJsonArray tracks;
            for (const auto& entry : old.value(key).toArray())
            {
                auto track = entry.toObject();
                track.remove("id");
                tracks.append(track);
            }
            old.insert(key, tracks);
        }
        ASSERT_TRUE(writeBytes(path, QJsonDocument(old).toJson()));
        auto migrated = motrack::readWorkspaceDocument(path);
        EXPECT_EQ(migrated.trackerSource.has_value(), version == 2);
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

} // namespace
