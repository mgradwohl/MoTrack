#include "workspace_document.hpp"

#include <QByteArray>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <gtest/gtest.h>

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
    ASSERT_TRUE(document.trackerSource);
    EXPECT_EQ(document.trackerSource->xml, xml.toStdString());
    EXPECT_EQ(document.project.description, "caf\xC3\xA9 & data");
    EXPECT_EQ(document.project.unsupportedTrackTypes, std::vector<std::string>{"FutureTrack"});
    document.project.description = "Edited in MoTrack";
    motrack::writeWorkspaceDocument(native, document);
    ASSERT_TRUE(QFile::remove(original));
    auto reopened = motrack::readWorkspaceDocument(native);
    ASSERT_TRUE(reopened.trackerSource);
    EXPECT_EQ(reopened.trackerSource->xml, xml.toStdString());
    EXPECT_EQ(reopened.project.description, "Edited in MoTrack");
    motrack::writeWorkspaceDocument(native, reopened);
    const auto resaved = motrack::readWorkspaceDocument(native);
    ASSERT_TRUE(resaved.trackerSource);
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
    EXPECT_EQ(object.value("version").toInt(), 2);
    object.insert("version", 1);
    object.remove("trackerSource");
    ASSERT_TRUE(writeBytes(path, QJsonDocument(object).toJson()));
    const auto migrated = motrack::readWorkspaceDocument(path);
    EXPECT_FALSE(migrated.trackerSource);
    EXPECT_EQ(migrated.project.description, document.project.description);
    motrack::writeWorkspaceDocument(path, migrated);
    const auto updated = QJsonDocument::fromJson(readBytes(path)).object();
    EXPECT_EQ(updated.value("version").toInt(), 2);
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
    object.insert("version", 3);
    EXPECT_TRUE(writeBytes(path, QJsonDocument(object).toJson()));
    EXPECT_THROW(static_cast<void>(motrack::readWorkspaceDocument(path)), motrack::ProjectError);
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
