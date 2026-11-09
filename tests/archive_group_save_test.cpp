#include "archive_group.hpp"
#include "paired_save.hpp"
#include "track_edits.hpp"
#include "tracker_archive.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include <array>
#include <functional>
#include <stdexcept>

namespace
{

QByteArray bytes(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Could not read archive-group test file.");
    return file.readAll();
}

void put(const QString& path, const QByteArray& data)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size())
        throw std::runtime_error("Could not write archive-group test file.");
}

motrack::ArchiveGroup editedGroup(const QString& source = {})
{
    const auto path =
        source.isEmpty() ? QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/linked-images/multitab-images.trz" : source;
    const auto archive = std::make_shared<motrack::TrackerArchive>(motrack::TrackerArchive::open(path));
    const auto prepare = [&](const char* name, double mass)
    {
        auto document = motrack::importTrackerDocument(archive->projectPath(name));
        archive->resolveMedia(document.project, name);
        document.archivePayload = archive;
        document.archiveMember = name;
        motrack::TrackEdits edits;
        if (!edits.setPointMassValue(document.project, 0, mass))
            throw std::runtime_error("Could not edit archive-group fixture.");
        return document;
    };
    auto result = motrack::makeArchiveGroup(prepare("experiment.trk", 5));
    (void)motrack::appendArchiveGroupDocument(result, prepare("second.trk", 7));
    return result;
}

QStringList files(const QTemporaryDir& directory)
{
    return QDir(directory.path()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDir::Name);
}

void expectPair(const QString& standardPath, const QString& nativePath, const motrack::ArchiveGroup& expected)
{
    const auto standard = motrack::TrackerArchive::open(standardPath);
    const auto native = motrack::readArchiveGroup(nativePath);
    EXPECT_EQ(native.id, expected.id);
    EXPECT_EQ(native.archive->sourceDigest(), expected.archive->sourceDigest());
    ASSERT_EQ(native.members.size(), expected.members.size());
    for (std::size_t i = 0; i < expected.members.size(); ++i)
    {
        const auto& before = expected.members[i];
        const auto& after = native.members[i];
        EXPECT_EQ(after.id, before.id);
        EXPECT_EQ(after.member, before.member);
        EXPECT_EQ(after.document.project, before.document.project);
        EXPECT_EQ(after.document.archivePayload, native.archive);
        const auto imported = motrack::importTrackerDocument(standard.projectPath(QString::fromStdString(before.member)));
        ASSERT_EQ(imported.project.pointMasses.size(), 1u);
        EXPECT_DOUBLE_EQ(imported.project.pointMasses.front().mass, before.document.project.pointMasses.front().mass);
    }
}

TEST(ArchiveGroupSave, SavesAllMembersFromReopenedGroupAfterOriginalRemoval)
{
    QTemporaryDir temporary;
    const auto original = temporary.filePath("original.trz");
    ASSERT_TRUE(QFile::copy(QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/linked-images/multitab-images.trz", original));
    const auto oldNative = temporary.filePath("before.motrack");
    motrack::writeArchiveGroup(oldNative, editedGroup(original));
    ASSERT_TRUE(QFile::remove(original));
    const auto expected = motrack::readArchiveGroup(oldNative);
    ASSERT_TRUE(QFile::remove(oldNative));
    const auto target = temporary.filePath(QString::fromUtf8("expériences.TRZ"));
    const auto saved = motrack::writePairedArchiveGroup(target, expected);
    ASSERT_EQ(saved.status, motrack::PairedSaveStatus::Saved) << saved.diagnostic.toStdString();
    EXPECT_TRUE(saved.standardCurrent && saved.nativeCurrent);
    EXPECT_TRUE(saved.recoveryPath.isEmpty());
    EXPECT_TRUE(saved.mediaLocations.isEmpty());
    EXPECT_GT(saved.observation, 0u);
    expectPair(saved.standardPath, saved.nativePath, expected);
    EXPECT_EQ(files(temporary).size(), 2);
}

TEST(ArchiveGroupSave, EveryCancellationBoundaryPreservesOrRecoversTheCompletePair)
{
    const auto group = editedGroup();
    constexpr std::array BOUNDARIES{motrack::PairedSaveProgress::StandardPrepared,
                                    motrack::PairedSaveProgress::NativePrepared,
                                    motrack::PairedSaveProgress::RecoveryRecorded,
                                    motrack::PairedSaveProgress::StandardSaved,
                                    motrack::PairedSaveProgress::NativeSaved};
    for (const auto boundary : BOUNDARIES)
    {
        SCOPED_TRACE(static_cast<int>(boundary));
        QTemporaryDir temporary;
        const auto standard = temporary.filePath("saved.trz");
        const auto native = temporary.filePath("saved.motrack");
        put(standard, "old standard");
        put(native, "old native");
        std::atomic_bool cancelled{false};
        const auto partial =
            motrack::writePairedArchiveGroup(standard, group, &cancelled, [&](auto state) { cancelled = state == boundary; });
        if (boundary == motrack::PairedSaveProgress::StandardPrepared || boundary == motrack::PairedSaveProgress::NativePrepared)
        {
            EXPECT_EQ(partial.status, motrack::PairedSaveStatus::Cancelled);
            EXPECT_FALSE(partial.standardCurrent || partial.nativeCurrent);
            EXPECT_EQ(bytes(standard), "old standard");
            EXPECT_EQ(bytes(native), "old native");
            EXPECT_EQ(files(temporary), (QStringList{"saved.motrack", "saved.trz"}));
            continue;
        }
        ASSERT_EQ(partial.status, motrack::PairedSaveStatus::RecoveryRequired) << partial.diagnostic.toStdString();
        EXPECT_EQ(partial.standardCurrent, boundary != motrack::PairedSaveProgress::RecoveryRecorded);
        EXPECT_EQ(partial.nativeCurrent, boundary == motrack::PairedSaveProgress::NativeSaved);
        if (!partial.standardCurrent)
            EXPECT_EQ(bytes(standard), "old standard");
        if (!partial.nativeCurrent)
            EXPECT_EQ(bytes(native), "old native");
        const auto before = files(temporary);
        EXPECT_THROW(motrack::writeNativeArchiveGroup(native, group), motrack::SaveDestinationError);
        EXPECT_THROW(motrack::writeNativeDocument(native, group.members.front().document), motrack::SaveDestinationError);
        const auto blocked = motrack::writePairedArchiveGroup(standard, group);
        EXPECT_EQ(blocked.status, motrack::PairedSaveStatus::RecoveryRequired);
        EXPECT_EQ(blocked.transactionId, partial.transactionId);
        EXPECT_EQ(files(temporary), before);
        const auto recovered = motrack::recoverPairedDocuments(partial.recoveryPath, nullptr, {}, partial.transactionId);
        ASSERT_EQ(recovered.status, motrack::PairedSaveStatus::Saved) << recovered.diagnostic.toStdString();
        EXPECT_TRUE(recovered.standardCurrent && recovered.nativeCurrent);
        EXPECT_GT(recovered.observation, partial.observation);
        expectPair(standard, native, group);
        EXPECT_EQ(files(temporary), (QStringList{"saved.motrack", "saved.trz"}));
    }
}

TEST(ArchiveGroupSave, InvalidGroupOrUnsupportedMemberCannotReplaceEitherOutput)
{
    QTemporaryDir temporary;
    const auto standard = temporary.filePath("saved.trz");
    const auto native = temporary.filePath("saved.motrack");
    put(standard, "old standard");
    put(native, "old native");
    const auto valid = editedGroup();
    const std::vector<std::function<void(motrack::ArchiveGroup&)>> mutations{
        [](auto& group) { group.id = QUuid{}; },
        [](auto& group) { group.archive.reset(); },
        [](auto& group) { group.members.clear(); },
        [](auto& group) { group.members[1].id = group.members[0].id; },
        [](auto& group) { group.members[1].document.archivePayload = editedGroup().archive; },
        [](auto& group)
        {
            group.members[1].document.project.description = "unsupported edit";
        }};
    for (std::size_t i = 0; i < mutations.size(); ++i)
    {
        SCOPED_TRACE(i);
        auto invalid = valid;
        mutations[i](invalid);
        const auto result = motrack::writePairedArchiveGroup(standard, invalid);
        EXPECT_EQ(result.status, motrack::PairedSaveStatus::Failed);
        EXPECT_FALSE(result.standardCurrent || result.nativeCurrent);
        EXPECT_EQ(bytes(standard), "old standard");
        EXPECT_EQ(bytes(native), "old native");
        EXPECT_EQ(files(temporary), (QStringList{"saved.motrack", "saved.trz"}));
    }
    const auto wrongSuffix = motrack::writePairedArchiveGroup(temporary.filePath("wrong.trk"), valid);
    EXPECT_EQ(wrongSuffix.status, motrack::PairedSaveStatus::Failed);
    const auto insideOwner = motrack::writePairedArchiveGroup(QDir(valid.archive->root()).filePath("unsafe.trz"), valid);
    EXPECT_EQ(insideOwner.status, motrack::PairedSaveStatus::Failed);
    EXPECT_FALSE(QFileInfo::exists(QDir(valid.archive->root()).filePath("unsafe.trz")));
}

TEST(ArchiveGroupSave, NativeAndPairedWritesShareLocksAndOrderedObservations)
{
    QTemporaryDir temporary;
    const auto group = editedGroup();
    const auto standard = temporary.filePath("saved.trz");
    const auto native = temporary.filePath("saved.motrack");
    const auto paired = motrack::writePairedArchiveGroup(
        standard,
        group,
        nullptr,
        [&](auto state)
        {
            if (state == motrack::PairedSaveProgress::StandardPrepared)
            {
                EXPECT_THROW(motrack::writeNativeArchiveGroup(native, group), motrack::SaveDestinationError);
                EXPECT_THROW(motrack::writeNativeDocument(native, group.members.front().document), motrack::SaveDestinationError);
                EXPECT_EQ(motrack::writePairedArchiveGroup(standard, group).status, motrack::PairedSaveStatus::Failed);
            }
        });
    ASSERT_EQ(paired.status, motrack::PairedSaveStatus::Saved);
    const auto nativeOnly = motrack::writeNativeArchiveGroup(native,
                                                             group,
                                                             nullptr,
                                                             [&](auto state)
                                                             {
                                                                 EXPECT_EQ(state, motrack::PairedSaveProgress::NativeSaved);
                                                                 EXPECT_EQ(motrack::writePairedArchiveGroup(standard, group).status,
                                                                           motrack::PairedSaveStatus::Failed);
                                                                 throw std::runtime_error("Observer failed after replacement");
                                                             });
    EXPECT_TRUE(nativeOnly.saved);
    EXPECT_GT(nativeOnly.observation, paired.observation);
    EXPECT_TRUE(nativeOnly.mediaLocations.isEmpty());
    const auto before = bytes(native);
    const std::atomic_bool cancelled{true};
    EXPECT_THROW(motrack::writeNativeArchiveGroup(native, group, &cancelled), std::exception);
    EXPECT_THROW(motrack::writeNativeArchiveGroup(temporary.filePath("wrong.txt"), group), motrack::ProjectError);
    EXPECT_EQ(bytes(native), before);
    expectPair(standard, native, group);
}

TEST(ArchiveGroupSave, RecoveryRequiresTheObservedTransactionAndUnchangedOutputs)
{
    QTemporaryDir temporary;
    const auto group = editedGroup();
    const auto standard = temporary.filePath("saved.trz");
    const auto native = temporary.filePath("saved.motrack");
    put(native, "old native");
    std::atomic_bool cancelled{false};
    const auto partial = motrack::writePairedArchiveGroup(
        standard, group, &cancelled, [&](auto state) { cancelled = state == motrack::PairedSaveProgress::StandardSaved; });
    ASSERT_EQ(partial.status, motrack::PairedSaveStatus::RecoveryRequired);
    const auto savedStandard = bytes(standard);
    const auto journal = bytes(partial.recoveryPath);
    auto replaced = QJsonDocument::fromJson(journal).object();
    replaced.insert("transaction", QUuid::createUuid().toString(QUuid::WithoutBraces));
    put(partial.recoveryPath, QJsonDocument(replaced).toJson());
    const auto stale = motrack::recoverPairedDocuments(partial.recoveryPath, nullptr, {}, partial.transactionId);
    EXPECT_EQ(stale.status, motrack::PairedSaveStatus::RecoveryRequired);
    EXPECT_NE(stale.transactionId, partial.transactionId);
    EXPECT_EQ(bytes(native), "old native");
    EXPECT_EQ(bytes(standard), savedStandard);
    put(partial.recoveryPath, journal);
    put(standard, "external change");
    const auto conflict = motrack::recoverPairedDocuments(partial.recoveryPath, nullptr, {}, partial.transactionId);
    EXPECT_EQ(conflict.status, motrack::PairedSaveStatus::RecoveryRequired);
    EXPECT_FALSE(conflict.standardCurrent);
    EXPECT_EQ(bytes(standard), "external change");
    EXPECT_EQ(bytes(native), "old native");
    put(standard, savedStandard);
    const auto recovered = motrack::recoverPairedDocuments(partial.recoveryPath, nullptr, {}, partial.transactionId);
    ASSERT_EQ(recovered.status, motrack::PairedSaveStatus::Saved);
    expectPair(standard, native, group);
    EXPECT_EQ(files(temporary), (QStringList{"saved.motrack", "saved.trz"}));
}

} // namespace
