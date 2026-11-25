// Non-ASCII text everywhere a path or name crosses an encoding boundary (CONTRIBUTING.md, "Text encoding and
// locale"): Tracker projects, archives and native files saved and reopened under CJK, emoji, accented Latin and
// Arabic directory and file names; archive member names; and movies and images opened by such paths.
//
// This binary has no application manifest, so on Windows it runs in the system's ANSI code page (often 1252), not
// UTF-8. Everything here must pass without activeCodePage: paths go through QString, the W APIs or
// motrack::toFilesystemPath, never a narrow string. Tracker tabsets get the same coverage with their reader (#150).

#include "archive_fixture.hpp"
#include "archive_group.hpp"
#include "archive_group_session.hpp"
#include "filesystem_path.hpp"
#include "image_sequence.hpp"
#include "legacy_project.hpp"
#include "track_edits.hpp"
#include "tracker_archive.hpp"
#include "tracker_writer.hpp"
#include "utf8_path.hpp"
#include "video_reader.hpp"
#include "workspace_document.hpp"
#include "workspace_input.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace
{

// Built from UTF-8 literals; the sources are compiled as UTF-8 (motrack_apply_text_encoding).
QString cjk()
{
    return QString::fromUtf8("实验");
}

QString emoji()
{
    return QString::fromUtf8("😀");
}

QString latin()
{
    return QString::fromUtf8("élève");
}

QString arabic()
{
    return QString::fromUtf8("تجربة");
}

QString allNames()
{
    return cjk() + ' ' + emoji() + ' ' + latin() + ' ' + arabic();
}

QString corpusFixture(const QString& name)
{
    return QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/" + name;
}

QByteArray readAll(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}

bool writeAll(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

/// The linked-images experiment, with its four frames renamed to "<imageDirectory>/<stem> N.png".
QByteArray renamedExperiment(const QString& imageDirectory, const QString& stem)
{
    auto xml = readAll(corpusFixture("linked-images/linked-images.trk"));
    for (int frame = 0; frame < 4; ++frame)
    {
        xml.replace(QStringLiteral("images/frame%1.png").arg(frame).toUtf8(),
                    QStringLiteral("%1/%2 %3.png").arg(imageDirectory, stem).arg(frame).toUtf8());
    }
    return xml;
}

QByteArray frameBytes(int frame)
{
    return readAll(corpusFixture(QStringLiteral("linked-images/images/frame%1.png").arg(frame)));
}

class NonAsciiRoundTrip : public testing::Test
{
  protected:
    void SetUp() override
    {
        ASSERT_TRUE(m_Temporary.isValid());
        m_Root = m_Temporary.filePath(allNames());
        ASSERT_TRUE(QDir().mkpath(m_Root));
        // The helper and Qt agree on the name; std::filesystem sees the same directory.
        ASSERT_TRUE(std::filesystem::is_directory(motrack::toFilesystemPath(m_Root)));
        EXPECT_EQ(motrack::fromFilesystemPath(motrack::toFilesystemPath(m_Root)), m_Root);
    }

    [[nodiscard]] QString path(const QString& name) const
    {
        return m_Root + '/' + name;
    }

  private:
    QTemporaryDir m_Temporary;
    QString m_Root;
};

TEST_F(NonAsciiRoundTrip, LinkedImagesProjectSavesAndReopensAsTrackerAndNative)
{
    const auto images = emoji() + ' ' + cjk();
    ASSERT_TRUE(QDir(path({})).mkpath(images));
    for (int frame = 0; frame < 4; ++frame)
    {
        ASSERT_TRUE(writeAll(path(QStringLiteral("%1/%2 %3.png").arg(images, arabic()).arg(frame)), frameBytes(frame)));
    }
    const auto input = path(latin() + ' ' + cjk() + ".trk");
    ASSERT_TRUE(writeAll(input, renamedExperiment(images, arabic())));

    auto prepared = motrack::prepareWorkspaceInput(input);
    ASSERT_TRUE(prepared.document);
    auto document = prepared.document->document;
    ASSERT_EQ(document.project.clip.mediaReferences.size(), 4u);
    EXPECT_EQ(QString::fromStdString(document.project.clip.mediaReferences.front()), images + '/' + arabic() + " 0.png");
    // Images open by their non-ASCII paths.
    const auto sequence = motrack::ImageSequence::open(prepared.mediaProjectPath, document.project.clip, document.mediaLocations);
    if (!sequence)
    {
        FAIL() << "The renamed images did not open.";
    }
    const auto frame = sequence->readStep(0);
    EXPECT_TRUE(frame && !frame->image.isNull());

    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.setPointMassValue(document.project, 0, 9.5));
    ASSERT_TRUE(edits.renamePointMass(document.project, 0, (arabic() + ' ' + emoji()).toStdString()));

    const auto standard = path(arabic() + ' ' + emoji() + ".trk");
    motrack::writeTrackerDocument(standard, document, prepared.mediaProjectPath);
    const auto reopened = motrack::readTrackerProject(motrack::toFilesystemPath(standard)).project;
    EXPECT_EQ(reopened.pointMasses.front().mass, 9.5);
    EXPECT_EQ(QString::fromStdString(reopened.pointMasses.front().name), arabic() + ' ' + emoji());
    EXPECT_EQ(reopened.clip.mediaReferences, document.project.clip.mediaReferences);

    const auto native = path(cjk() + ' ' + latin() + ".motrack");
    auto snapshot = document;
    motrack::rebaseWorkspaceMedia(snapshot, prepared.mediaProjectPath, native);
    motrack::writeWorkspaceDocument(native, snapshot);
    auto nativeInput = motrack::prepareWorkspaceInput(native);
    ASSERT_TRUE(nativeInput.document);
    const auto& restored = nativeInput.document->document;
    EXPECT_EQ(restored.project.pointMasses.front().mass, 9.5);
    EXPECT_EQ(restored.project.pointMasses.front().name, document.project.pointMasses.front().name);
    const auto media = motrack::workspaceMediaPath(restored, nativeInput.mediaProjectPath, restored.project.clip.mediaReferences.front());
    EXPECT_TRUE(QFileInfo(media).isFile()) << media.toStdString();
}

TEST_F(NonAsciiRoundTrip, ArchiveWithNonAsciiNameAndMembersSavesAndReopens)
{
    const auto experiment = cjk() + ' ' + latin();
    const auto images = emoji() + ' ' + arabic();
    const auto projectMember = (experiment + '/' + arabic() + ".trk").toUtf8();
    std::vector<QByteArray> names{projectMember};
    std::vector<motrack::test::ArchiveEntry> members{{.name = nullptr, .bytes = renamedExperiment(images, cjk())}};
    for (int frame = 0; frame < 4; ++frame)
    {
        names.push_back(QStringLiteral("%1/%2/%3 %4.png").arg(experiment, images, cjk()).arg(frame).toUtf8());
        members.push_back({.name = nullptr, .bytes = frameBytes(frame)});
    }
    for (std::size_t i = 0; i < members.size(); ++i)
    {
        members[i].name = names[i].constData();
    }
    const auto input = path(emoji() + ' ' + cjk() + ".trz");
    ASSERT_TRUE(motrack::test::writeArchive(input, members));

    auto prepared = motrack::prepareWorkspaceInput(input);
    ASSERT_TRUE(prepared.group);
    const auto ids = prepared.group->memberIds();
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(QString::fromStdString(prepared.group->document(ids.front()).archiveMember.value_or("")), QString::fromUtf8(projectMember));
    const auto& archived = prepared.group->document(ids.front());
    const auto archive = archived.archivePayload;
    ASSERT_TRUE(archive);
    EXPECT_TRUE(QFileInfo(QDir(archive->root()).filePath(QString::fromStdString(archived.project.clip.mediaReferences.front()))).isFile());
    ASSERT_TRUE(prepared.group->history(ids.front()).setPointMassValue(prepared.group->project(ids.front()), 0, 7.25));
    const auto snapshot = prepared.group->prepareSave().snapshot;

    // Standard save: the member names keep their UTF-8 bytes and flag, so Tracker and MoTrack read them back.
    const auto standard = path(arabic() + ' ' + latin() + ".trz");
    motrack::writeTrackerArchiveDocument(standard, snapshot.members.front().document);
    const auto reopened = motrack::TrackerArchive::open(standard);
    EXPECT_EQ(reopened.projects(), QStringList{QString::fromUtf8(projectMember)});
    EXPECT_EQ(motrack::readTrackerProject(motrack::toFilesystemPath(reopened.projectPath(QString::fromUtf8(projectMember))))
                  .project.pointMasses.front()
                  .mass,
              7.25);
    EXPECT_TRUE(reopened.memberMatchesSource(QString::fromUtf8(names.back())));

    // Native save: the bundle stores the original archive and reopens it under another non-ASCII name.
    const auto native = path(latin() + ' ' + emoji() + ".motrack");
    motrack::writeArchiveGroup(native, snapshot);
    auto nativeInput = motrack::prepareWorkspaceInput(native);
    ASSERT_TRUE(nativeInput.group);
    const auto restored = nativeInput.group->prepareSave().snapshot;
    ASSERT_EQ(restored.members.size(), 1u);
    EXPECT_EQ(restored.members.front().member, projectMember.toStdString());
    EXPECT_EQ(restored.members.front().document.project.pointMasses.front().mass, 7.25);
    EXPECT_EQ(restored.archive->sourceDigest(), snapshot.archive->sourceDigest());
}

/// Whether Tracker finds every media file of the experiment @p projectName (stored bytes) holding @p xml in an
/// archive with the members @p stored. OSP's ResourceLoader looks a member up with `Map.get` on the names
/// `ZipInputStream` returned: Java compares the UTF-16 text exactly and never normalizes it.
bool trackerFindsEveryImage(const std::vector<QByteArray>& stored, const QByteArray& projectName, const QByteArray& xml)
{
    const auto directory = projectName.left(projectName.lastIndexOf('/') + 1);
    const auto references = motrack::parseLegacyProject(xml.toStdString()).clip.mediaReferences;
    return !references.empty() &&
           std::ranges::all_of(references,
                               [&](const std::string& reference)
                               { return std::ranges::contains(stored, directory + QByteArray::fromStdString(reference)); });
}

TEST_F(NonAsciiRoundTrip, ArchiveSavesKeepStoredNamesAndReferencesInEitherNormalizationForm)
{
    // macOS file names are decomposed (NFD), so an archive made there can store "e" + U+0301 where one made on
    // Windows stores U+00E9, in its member names, in the experiment's media references, or in only one of them.
    // MoTrack identifies members by their NFC form on every platform (#308), but Tracker matches the stored bytes:
    // a save must leave both the stored names and the references exactly as they were. The archives are written
    // record by record, because libarchive normalizes the names it reads and writes differently on each platform.
    const auto folder = latin();
    const auto images = QString::fromUtf8("vidéos à ç");
    const auto stem = QString::fromUtf8("îmage");
    const auto projectName = folder + QString::fromUtf8("/expérience.trk");
    using Form = QString::NormalizationForm;
    struct Case
    {
        const char* name;
        Form members;
        Form references;
    };
    for (const auto& forms :
         {Case{.name = "NFC names, NFC references", .members = Form::NormalizationForm_C, .references = Form::NormalizationForm_C},
          Case{.name = "NFD names, NFD references", .members = Form::NormalizationForm_D, .references = Form::NormalizationForm_D},
          Case{.name = "NFD names, NFC references", .members = Form::NormalizationForm_D, .references = Form::NormalizationForm_C},
          Case{.name = "NFC names, NFD references", .members = Form::NormalizationForm_C, .references = Form::NormalizationForm_D}})
    {
        SCOPED_TRACE(forms.name);
        const auto xml = renamedExperiment(images.normalized(forms.references), stem.normalized(forms.references));
        std::vector<QByteArray> stored{projectName.normalized(forms.members).toUtf8()};
        std::vector<motrack::test::ArchiveEntry> members{{.name = nullptr, .bytes = xml}};
        QStringList identities;
        for (int frame = 0; frame < 4; ++frame)
        {
            const auto member = QStringLiteral("%1/%2/%3 %4.png").arg(folder, images, stem).arg(frame);
            identities.push_back(member.normalized(Form::NormalizationForm_C));
            stored.push_back(member.normalized(forms.members).toUtf8());
            members.push_back({.name = nullptr, .bytes = frameBytes(frame)});
        }
        for (std::size_t i = 0; i < members.size(); ++i)
        {
            members[i].name = stored[i].constData();
        }
        const auto original = motrack::test::storedArchiveBytes(members);
        ASSERT_EQ(motrack::test::storedMemberNames(original), stored);
        const bool trackerOpensMedia = trackerFindsEveryImage(stored, stored.front(), xml);
        ASSERT_EQ(trackerOpensMedia, forms.members == forms.references);
        const auto input =
            path(QStringLiteral("input %1%2.trz").arg(static_cast<int>(forms.members)).arg(static_cast<int>(forms.references)));
        ASSERT_TRUE(writeAll(input, original));

        // The same identities whatever was stored and whatever the platform's libarchive returned.
        auto prepared = motrack::prepareWorkspaceInput(input);
        ASSERT_TRUE(prepared.group);
        const auto ids = prepared.group->memberIds();
        ASSERT_EQ(ids.size(), 1u);
        const auto identity = projectName.normalized(Form::NormalizationForm_C);
        const auto& archived = prepared.group->document(ids.front());
        EXPECT_EQ(QString::fromStdString(archived.archiveMember.value_or("")), identity);
        const auto archive = archived.archivePayload;
        ASSERT_TRUE(archive);
        EXPECT_EQ(archive->projects(), QStringList{identity});
        ASSERT_EQ(archived.project.clip.mediaReferences.size(), 4u);
        for (int frame = 0; frame < 4; ++frame)
        {
            const auto reference = QString::fromStdString(archived.project.clip.mediaReferences[static_cast<std::size_t>(frame)]);
            EXPECT_EQ(reference, identities[frame]);
            EXPECT_EQ(readAll(QDir(archive->root()).filePath(reference)), frameBytes(frame));
            EXPECT_TRUE(archive->memberMatchesSource(reference.normalized(Form::NormalizationForm_D)));
        }

        // Saving without an edit returns the archive byte for byte.
        const auto unchanged =
            path(QStringLiteral("unchanged %1%2.trz").arg(static_cast<int>(forms.members)).arg(static_cast<int>(forms.references)));
        motrack::writeTrackerArchiveDocument(unchanged, prepared.group->prepareSave().snapshot.members.front().document);
        EXPECT_TRUE(readAll(unchanged) == original);

        // An edit changes only the experiment's bytes: no member is renamed and no reference is respelled, so
        // Tracker finds exactly the media it found before.
        ASSERT_TRUE(prepared.group->history(ids.front()).setPointMassValue(prepared.group->project(ids.front()), 0, 7.25));
        const auto snapshot = prepared.group->prepareSave().snapshot;
        const auto standard =
            path(QStringLiteral("saved %1%2.trz").arg(static_cast<int>(forms.members)).arg(static_cast<int>(forms.references)));
        motrack::writeTrackerArchiveDocument(standard, snapshot.members.front().document);
        const auto saved = readAll(standard);
        EXPECT_EQ(motrack::test::storedMemberNames(saved), stored);
        const auto reopened = motrack::TrackerArchive::open(standard);
        ASSERT_EQ(reopened.projects(), QStringList{identity});
        const auto savedXml = readAll(reopened.projectPath(identity));
        EXPECT_TRUE(savedXml != xml);
        EXPECT_EQ(motrack::parseLegacyProject(savedXml.toStdString()).clip.mediaReferences,
                  motrack::parseLegacyProject(xml.toStdString()).clip.mediaReferences);
        EXPECT_EQ(trackerFindsEveryImage(motrack::test::storedMemberNames(saved), stored.front(), savedXml), trackerOpensMedia);
        auto restored = motrack::importTrackerDocument(reopened.projectPath(identity));
        EXPECT_EQ(restored.project.pointMasses.front().mass, 7.25);
        reopened.resolveMedia(restored.project, identity);
        EXPECT_EQ(restored.project.clip.mediaReferences, archived.project.clip.mediaReferences);
        for (const auto& member : identities)
        {
            EXPECT_TRUE(reopened.memberMatchesSource(member));
        }

        // The native companion keeps the original archive and finds the member under either spelling's identity.
        const auto native =
            path(QStringLiteral("native %1%2.motrack").arg(static_cast<int>(forms.members)).arg(static_cast<int>(forms.references)));
        motrack::writeArchiveGroup(native, snapshot);
        auto nativeInput = motrack::prepareWorkspaceInput(native);
        ASSERT_TRUE(nativeInput.group);
        const auto nativeSnapshot = nativeInput.group->prepareSave().snapshot;
        ASSERT_EQ(nativeSnapshot.members.size(), 1u);
        EXPECT_EQ(QString::fromStdString(nativeSnapshot.members.front().member), identity);
        EXPECT_EQ(nativeSnapshot.members.front().document.project.pointMasses.front().mass, 7.25);
        EXPECT_TRUE(readAll(nativeSnapshot.archive->sourcePath()) == original);
        const auto resaved =
            path(QStringLiteral("resaved %1%2.trz").arg(static_cast<int>(forms.members)).arg(static_cast<int>(forms.references)));
        motrack::writeTrackerArchiveDocument(resaved, nativeSnapshot.members.front().document);
        EXPECT_TRUE(readAll(resaved) == saved);
    }
}

TEST_F(NonAsciiRoundTrip, ArchiveWithUnflaggedUtf8NamesOpensSavesAndReopens)
{
    // Info-ZIP and older archivers write UTF-8 member names without the ZIP UTF-8 flag (general-purpose bit 11).
    // java.util.zip, and so Tracker, reads every name as UTF-8 whatever the flag says (#275). MoTrack opens such an
    // archive under the same names on every platform and locale, and saves into it without touching a name or flag.
    const auto experiment = cjk() + ' ' + latin();
    const auto images = emoji() + ' ' + arabic();
    const auto projectName = experiment + '/' + arabic() + ".trk";
    std::vector<QByteArray> stored{projectName.toUtf8()};
    std::vector<motrack::test::ArchiveEntry> members{{.name = nullptr, .bytes = renamedExperiment(images, cjk())}};
    for (int frame = 0; frame < 4; ++frame)
    {
        stored.push_back(QStringLiteral("%1/%2/%3 %4.png").arg(experiment, images, cjk()).arg(frame).toUtf8());
        members.push_back({.name = nullptr, .bytes = frameBytes(frame)});
    }
    for (std::size_t i = 0; i < members.size(); ++i)
    {
        members[i].name = stored[i].constData();
    }
    const auto original = motrack::test::storedArchiveBytes(members, false);
    const auto input = path(latin() + " unflagged.trz");
    ASSERT_TRUE(writeAll(input, original));

    auto prepared = motrack::prepareWorkspaceInput(input);
    ASSERT_TRUE(prepared.group);
    const auto ids = prepared.group->memberIds();
    ASSERT_EQ(ids.size(), 1u);
    const auto& archived = prepared.group->document(ids.front());
    EXPECT_EQ(QString::fromStdString(archived.archiveMember.value_or("")), projectName);
    ASSERT_TRUE(archived.archivePayload);
    ASSERT_EQ(archived.project.clip.mediaReferences.size(), 4u);
    for (int frame = 0; frame < 4; ++frame)
    {
        const auto reference = QString::fromStdString(archived.project.clip.mediaReferences[static_cast<std::size_t>(frame)]);
        EXPECT_EQ(reference.toUtf8(), stored[static_cast<std::size_t>(frame) + 1]);
        EXPECT_EQ(readAll(QDir(archived.archivePayload->root()).filePath(reference)), frameBytes(frame));
    }

    const auto unchanged = path(latin() + " unflagged unchanged.trz");
    motrack::writeTrackerArchiveDocument(unchanged, prepared.group->prepareSave().snapshot.members.front().document);
    EXPECT_TRUE(readAll(unchanged) == original);

    ASSERT_TRUE(prepared.group->history(ids.front()).setPointMassValue(prepared.group->project(ids.front()), 0, 7.25));
    const auto standard = path(latin() + " unflagged saved.trz");
    motrack::writeTrackerArchiveDocument(standard, prepared.group->prepareSave().snapshot.members.front().document);
    const auto saved = readAll(standard);
    EXPECT_EQ(motrack::test::storedMemberNames(saved), stored);
    // Bit 11 stays clear in every local and central record: the flags follow each record's version field.
    EXPECT_EQ(saved.count(QByteArray("PK\x03\x04\x14\x00\x00\x00", 8)), 5);
    EXPECT_EQ(saved.count(QByteArray("PK\x01\x02\x14\x00\x14\x00\x00\x00", 10)), 5);
    const auto reopened = motrack::TrackerArchive::open(standard);
    ASSERT_EQ(reopened.projects(), QStringList{projectName});
    EXPECT_EQ(motrack::importTrackerDocument(reopened.projectPath(projectName)).project.pointMasses.front().mass, 7.25);
    EXPECT_TRUE(reopened.memberMatchesSource(QString::fromUtf8(stored.back())));
}

TEST_F(NonAsciiRoundTrip, Utf8PathsNameTheSameFilesAsQt)
{
    // Qt-free code (the OSP and library tests) names files with motrack::utf8Path.
    const auto name = path(allNames() + ".txt");
    QFile file(name);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly) && file.write("bytes") == 5);
    file.close();
    const auto text = name.toStdString();
    const auto native = motrack::utf8Path(text);
    EXPECT_EQ(native, motrack::toFilesystemPath(name));
    EXPECT_EQ(motrack::genericUtf8String(native), text);
    EXPECT_EQ(motrack::fromFilesystemPath(motrack::utf8Path(motrack::utf8String(native))), name);
    std::ifstream stream(native, std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()), "bytes");
}

TEST_F(NonAsciiRoundTrip, MovieOpensByANonAsciiPath)
{
    const auto movie = path(allNames() + ".mp4");
    ASSERT_TRUE(QFile::copy(QString::fromUtf8(MOTRACK_FIXTURES) + "/vfr.mp4", movie));
    motrack::VideoReader reader(movie);
    const auto frame = reader.readNext();
    EXPECT_TRUE(frame && !frame->image.isNull());
    auto prepared = motrack::prepareWorkspaceMovie(movie);
    ASSERT_TRUE(prepared.document);
    EXPECT_TRUE(prepared.document->document.movie);
}

} // namespace
