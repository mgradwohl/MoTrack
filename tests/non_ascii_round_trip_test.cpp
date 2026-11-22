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
#include "video_reader.hpp"
#include "workspace_document.hpp"
#include "workspace_input.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include <filesystem>
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
