#include "track_edits.hpp"
#include "tracker_archive.hpp"
#include "tracker_writer.hpp"
#include "workspace_document.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <gtest/gtest.h>

namespace
{
QString fixture(const QString& name = "linked-images.trk")
{
    return QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/linked-images/" + name;
}

QByteArray bytes(const QString& path)
{
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly))
        throw motrack::ProjectError("Could not read package fixture.");
    return input.readAll();
}

void put(const QString& path, const QByteArray& value)
{
    QFile output(path);
    if (!output.open(QIODevice::WriteOnly) || output.write(value) != value.size())
        throw motrack::ProjectError("Could not write package fixture.");
}

TEST(TrackerImagePackage, RetainsEditedDataAndMediaAfterNativeReopenAndSourceRemoval)
{
    QTemporaryDir temporary;
    ASSERT_TRUE(QDir(temporary.path()).mkdir("images"));
    const auto original = temporary.filePath("input.trk");
    ASSERT_TRUE(QFile::copy(fixture(), original));
    for (int frame = 0; frame < 4; ++frame)
    {
        const auto name = "images/frame" + QString::number(frame) + ".png";
        ASSERT_TRUE(QFile::copy(fixture(name), temporary.filePath(name)));
    }
    auto document = motrack::importTrackerDocument(original);
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.setPointMassValue(document.project, 0, 5));
    ASSERT_TRUE(edits.markPointMass(document.project, 0, {.frame = 1, .x = 8, .y = 12}));
    ASSERT_TRUE(edits.setClipRange(document.project, 2, 2, 1));
    document.project.clip.sourceFrameCount = 4;
    const auto native = temporary.filePath("saved.motrack");
    motrack::writeWorkspaceDocument(native, document);
    document = {};
    ASSERT_TRUE(QFile::remove(original));
    document = motrack::readWorkspaceDocument(native);
    const auto before = document.project;
    if (!document.trackerSource)
        FAIL() << "Native reopen lost its source.";
    const auto sourceXml = document.trackerSource->xml;
    const auto target = temporary.filePath(QString::fromUtf8("portable é & images.trz"));
    motrack::writeTrackerImagePackage(target, document, native);
    EXPECT_EQ(document.project, before);
    EXPECT_EQ(document.trackerSource->xml, sourceXml);
    ASSERT_TRUE(QFile::remove(native));
    for (int frame = 0; frame < 4; ++frame)
        ASSERT_TRUE(QFile::remove(temporary.filePath("images/frame" + QString::number(frame) + ".png")));
    ASSERT_TRUE(QDir(temporary.path()).mkdir("relocated"));
    const auto relocated = temporary.filePath("relocated/experiment.trz");
    ASSERT_TRUE(QFile::rename(target, relocated));
    auto archive = std::make_shared<motrack::TrackerArchive>(motrack::TrackerArchive::open(relocated));
    ASSERT_EQ(archive->projects(), QStringList{"experiment.trk"});
    auto reopened = motrack::importTrackerDocument(archive->projectPath("experiment.trk"));
    archive->resolveMedia(reopened.project, "experiment.trk");
    auto expected = before;
    expected.clip.sourceFrameCount.reset();
    for (int frame = 0; frame < 4; ++frame)
    {
        const auto member = QStringLiteral("images/frame-%1.png").arg(frame, 6, 10, QLatin1Char('0'));
        expected.clip.mediaReferences.at(static_cast<std::size_t>(frame)) = member.toStdString();
        EXPECT_EQ(bytes(QDir(archive->root()).filePath(member)), bytes(fixture("images/frame" + QString::number(frame) + ".png")));
    }
    EXPECT_EQ(reopened.project, expected);
    EXPECT_EQ(reopened.project.clip.firstFrame, 2);
    EXPECT_EQ(reopened.project.clip.stepCount, 1);
    reopened.archivePayload = std::move(archive);
    reopened.archiveMember = "experiment.trk";
    const auto portableNative = temporary.filePath("portable.motrack");
    motrack::writeWorkspaceDocument(portableNative, reopened);
    reopened = {};
    ASSERT_TRUE(QFile::remove(relocated));
    reopened = motrack::readWorkspaceDocument(portableNative);
    EXPECT_EQ(reopened.project, expected);
    ASSERT_TRUE(edits.setPointMassValue(reopened.project, 0, 7));
    const auto resaved = temporary.filePath("resaved.trz");
    motrack::writeTrackerArchiveDocument(resaved, reopened);
    const auto again = motrack::TrackerArchive::open(resaved);
    EXPECT_DOUBLE_EQ(motrack::importTrackerDocument(again.projectPath("experiment.trk")).project.pointMasses.at(0).mass, 7);
}

TEST(TrackerImagePackage, RetainedMediaBindingsAreReplacedByPrivatePackageMembers)
{
    for (const bool matchingNames : {false, true})
    {
        SCOPED_TRACE(matchingNames);
        QTemporaryDir temporary;
        ASSERT_TRUE(QDir(temporary.path()).mkdir("import"));
        ASSERT_TRUE(QDir(temporary.path()).mkdir("images"));
        auto xml = bytes(fixture());
        for (int frame = 0; frame < 4; ++frame)
        {
            const auto original = "images/frame" + QString::number(frame) + ".png";
            const auto member = QStringLiteral("images/frame-%1.png").arg(frame, 6, 10, QLatin1Char('0'));
            ASSERT_TRUE(QFile::copy(fixture(original), temporary.filePath(member)));
            xml.replace(original.toUtf8(), (matchingNames ? member : "../" + member).toUtf8());
        }
        const auto input = temporary.filePath(matchingNames ? "input.trk" : "import/input.trk");
        put(input, xml);
        auto document = motrack::importTrackerDocument(input);
        motrack::retainWorkspaceMediaLocations(document, input);
        const auto bindings = document.mediaLocations;
        ASSERT_EQ(bindings.size(), 4u);
        ASSERT_TRUE(QFile::remove(input));
        ASSERT_TRUE(QDir(temporary.path()).rmdir("import"));
        const auto target = temporary.filePath("portable.trz");
        motrack::writeTrackerImagePackage(target, document, input);
        EXPECT_EQ(document.mediaLocations, bindings);
        const auto archive = motrack::TrackerArchive::open(target);
        const auto reopened = motrack::importTrackerDocument(archive.projectPath("experiment.trk"));
        ASSERT_EQ(reopened.project.clip.mediaReferences.size(), 4u);
        for (int frame = 0; frame < 4; ++frame)
        {
            const auto member = QStringLiteral("images/frame-%1.png").arg(frame, 6, 10, QLatin1Char('0'));
            EXPECT_EQ(reopened.project.clip.mediaReferences[static_cast<std::size_t>(frame)], member.toStdString());
            EXPECT_EQ(bytes(QDir(archive.root()).filePath(member)), bytes(fixture("images/frame" + QString::number(frame) + ".png")));
        }
    }
}

TEST(TrackerImagePackage, RefusesUnsupportedDocumentsMissingMediaAndCancellationWithoutReplacingOutput)
{
    for (int kind = 0; kind < 11; ++kind)
    {
        SCOPED_TRACE(kind);
        QTemporaryDir temporary;
        auto document = motrack::importTrackerDocument(fixture());
        std::atomic_bool cancelled{kind == 0};
        QString source = fixture();
        switch (kind)
        {
        case 1:
            document.trackerSource.reset();
            break;
        case 2:
            document.project.clip.mediaReferences.clear();
            break;
        case 3:
            document.project.clip.mediaReferences.resize(4096, "images/frame0.png");
            break;
        case 4:
            document.project.clip.mediaReferences[0] = "missing.png";
            break;
        case 5:
            document.project.clip.startTimeMs = 45;
            break;
        case 6:
            document.movie = true;
            break;
        case 7:
            document.archiveMember = "experiment.trk";
            break;
        case 8:
            source = "relative.trk";
            break;
        case 9:
            document.project.clip.mediaReferences[0] = "https://example.invalid/image.png";
            break;
        default:
            break;
        }
        const auto target = temporary.filePath(kind == 10 ? "wrong.trk" : "saved.trz");
        put(target, "original destination");
        EXPECT_THROW(motrack::writeTrackerImagePackage(target, document, source, &cancelled), motrack::ProjectError);
        EXPECT_EQ(bytes(target), "original destination");
        EXPECT_EQ(QDir(temporary.path()).entryList(QDir::Files), QStringList{QFileInfo(target).fileName()});
    }
}

TEST(TrackerImagePackage, NamesMissingOrNonFileSourcesWithoutReplacingOutput)
{
    for (const bool directory : {false, true})
    {
        SCOPED_TRACE(directory);
        QTemporaryDir temporary;
        const auto missing = temporary.filePath(QString::fromUtf8("missing é image.png"));
        if (directory)
            ASSERT_TRUE(QDir().mkdir(missing));
        auto document = motrack::importTrackerDocument(fixture());
        document.project.clip.mediaReferences[0] = missing.toStdString();
        const auto target = temporary.filePath("existing.trz");
        put(target, "original archive");
        try
        {
            motrack::writeTrackerImagePackage(target, document, fixture());
            FAIL() << "Packaging an unavailable image should fail.";
        }
        catch (const motrack::ProjectError& error)
        {
            const auto diagnostic = QString::fromUtf8(error.what());
            EXPECT_TRUE(QDir::fromNativeSeparators(diagnostic).contains(missing)) << diagnostic.toStdString();
        }
        EXPECT_EQ(bytes(target), "original archive");
    }
}

TEST(TrackerImagePackage, CannotReplaceLinkedMediaEvenWithAnArchiveSuffix)
{
    QTemporaryDir temporary;
    const auto target = temporary.filePath("media.trz");
    ASSERT_TRUE(QFile::copy(fixture("images/frame0.png"), target));
    const auto before = bytes(target);
    auto document = motrack::importTrackerDocument(fixture());
    document.project.clip.mediaReferences[0] = target.toStdString();
    EXPECT_THROW(motrack::writeTrackerImagePackage(target, document, fixture()), motrack::ProjectError);
    EXPECT_EQ(bytes(target), before);
}

TEST(TrackerImagePackage, RejectsImageNamesWithoutAPortableExtension)
{
    QTemporaryDir temporary;
    for (const auto* name : {"image", "image.tooLongExtension", "image.bad-ext"})
    {
        const auto input = temporary.filePath(name);
        ASSERT_TRUE(QFile::copy(fixture("images/frame0.png"), input));
        auto document = motrack::importTrackerDocument(fixture());
        document.project.clip.mediaReferences[0] = input.toStdString();
        const auto target = temporary.filePath("saved.trz");
        EXPECT_THROW(motrack::writeTrackerImagePackage(target, document, fixture()), motrack::ProjectError);
        EXPECT_FALSE(QFile::exists(target));
    }
}

#ifndef _WIN32
TEST(TrackerImagePackage, ResolvesMediaAliasesAndRejectsOutputFileAliases)
{
    QTemporaryDir temporary;
    const auto physical = temporary.filePath("physical.motrack");
    ASSERT_TRUE(QFile::copy(fixture("images/frame0.png"), physical));
    const auto alias = temporary.filePath("logical.png");
    ASSERT_TRUE(QFile::link(physical, alias));
    auto document = motrack::importTrackerDocument(fixture());
    document.project.clip.mediaReferences[0] = alias.toStdString();
    const auto target = temporary.filePath("saved.trz");
    motrack::writeTrackerImagePackage(target, document, fixture());
    const auto archive = motrack::TrackerArchive::open(target);
    EXPECT_EQ(bytes(QDir(archive.root()).filePath("images/frame-000000.png")), bytes(physical));
    const auto outputAlias = temporary.filePath("alias.trz");
    ASSERT_TRUE(QFile::link(target, outputAlias));
    const auto before = bytes(target);
    EXPECT_THROW(motrack::writeTrackerImagePackage(outputAlias, document, fixture()), motrack::ProjectError);
    EXPECT_EQ(bytes(target), before);
}
#endif
} // namespace
