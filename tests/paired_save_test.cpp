#include "paired_save.hpp"

#include "local_media_path.hpp"
#include "track_edits.hpp"
#include "tracker_archive.hpp"
#include "tracker_writer.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include <array>
#include <functional>
#include <stdexcept>

namespace
{
QString fixture(const QString& name = "linked-images.trk")
{
    return QString::fromUtf8(MOTRACK_FIXTURES) + "/../corpus/fixtures/linked-images/" + name;
}

QByteArray bytes(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw std::runtime_error("Could not read paired-save test file.");
    return file.readAll();
}

void put(const QString& path, const QByteArray& data)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size())
        throw std::runtime_error("Could not write paired-save test file.");
}

motrack::WorkspaceDocument editedDocument()
{
    auto document = motrack::importTrackerDocument(fixture());
    motrack::TrackEdits edits;
    if (!edits.setPointMassValue(document.project, 0, 5.0) || !edits.markPointMass(document.project, 0, {.frame = 1, .x = 8, .y = 12}))
        throw std::runtime_error("Could not edit paired-save fixture.");
    return document;
}

QStringList files(const QTemporaryDir& directory)
{
    return QDir(directory.path()).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDir::Name);
}

QJsonObject record(const QString& path)
{
    return QJsonDocument::fromJson(bytes(path)).object();
}

QString stage(const QString& recovery, int index)
{
    const auto name = record(recovery).value("outputs").toArray().at(index).toObject().value("stage").toString();
    return QFileInfo(recovery).absoluteDir().filePath(name);
}

TEST(PairedSave, SavesBothLinkedDocumentsAndKeepsTheInputSnapshot)
{
    QTemporaryDir temporary;
    const auto input = fixture();
    const auto originalBytes = bytes(input);
    const auto document = editedDocument();
    const auto original = document.project;
    const auto destination = temporary.filePath(QString::fromUtf8("élève & paired.trk"));
    std::vector<motrack::PairedSaveProgress> progress;
    const auto result =
        motrack::writePairedDocuments(destination, document, input, nullptr, [&progress](auto state) { progress.push_back(state); });
    ASSERT_EQ(result.status, motrack::PairedSaveStatus::Saved) << result.diagnostic.toStdString();
    EXPECT_TRUE(result.standardCurrent);
    EXPECT_TRUE(result.nativeCurrent);
    EXPECT_TRUE(result.recoveryPath.isEmpty());
    EXPECT_EQ(progress,
              (std::vector{motrack::PairedSaveProgress::StandardPrepared,
                           motrack::PairedSaveProgress::NativePrepared,
                           motrack::PairedSaveProgress::RecoveryRecorded,
                           motrack::PairedSaveProgress::StandardSaved,
                           motrack::PairedSaveProgress::NativeSaved}));
    const auto native = motrack::readWorkspaceDocument(result.nativePath);
    const auto standard = motrack::importTrackerDocument(result.standardPath);
    EXPECT_EQ(native.project, standard.project);
    EXPECT_DOUBLE_EQ(native.project.pointMasses.at(0).mass, 5.0);
    EXPECT_EQ(native.project.pointMasses.at(0).marks, original.pointMasses.at(0).marks);
    ASSERT_TRUE(native.trackerSource.has_value());
    EXPECT_EQ(native.trackerSource->xml, originalBytes.toStdString());
    ASSERT_EQ(native.project.clip.mediaReferences.size(), 4u);
    for (std::size_t i = 0; i < native.project.clip.mediaReferences.size(); ++i)
    {
        const auto resolved = motrack::resolveLocalMediaPath(result.nativePath, native.project.clip.mediaReferences[i]);
        EXPECT_EQ(bytes(resolved), bytes(fixture(QString("images/frame%1.png").arg(i))));
    }
    EXPECT_EQ(document.project, original);
    EXPECT_EQ(bytes(input), originalBytes);
    const auto standardBytes = bytes(result.standardPath);
    const auto nativeBytes = bytes(result.nativePath);
    const auto repeated = motrack::writePairedDocuments(destination, document, input);
    EXPECT_EQ(repeated.status, motrack::PairedSaveStatus::Saved) << repeated.diagnostic.toStdString();
    EXPECT_EQ(bytes(result.standardPath), standardBytes);
    EXPECT_EQ(bytes(result.nativePath), nativeBytes);
    EXPECT_EQ(files(temporary).size(), 2);
}

TEST(PairedSave, SavesAnArchivePairAfterTheOriginalInputIsRemoved)
{
    QTemporaryDir temporary;
    const auto input = temporary.filePath("input.trz");
    ASSERT_TRUE(QFile::copy(fixture("multitab-images.trz"), input));
    auto payload = std::make_shared<motrack::TrackerArchive>(motrack::TrackerArchive::open(input));
    auto document = motrack::importTrackerDocument(payload->projectPath("experiment.trk"));
    payload->resolveMedia(document.project, "experiment.trk");
    document.archivePayload = payload;
    document.archiveMember = "experiment.trk";
    const auto originalArchive = bytes(payload->sourcePath());
    const auto otherExperiment = bytes(payload->projectPath("second.trk"));
    motrack::TrackEdits edits;
    ASSERT_TRUE(edits.setPointMassValue(document.project, 0, 5));
    ASSERT_TRUE(edits.markPointMass(document.project, 0, {.frame = 1, .x = 8, .y = 12}));
    const auto expectedProject = document.project;
    ASSERT_TRUE(QFile::remove(input));
    const auto result = motrack::writePairedDocuments(temporary.filePath("saved.trz"), document, input);
    ASSERT_EQ(result.status, motrack::PairedSaveStatus::Saved) << result.diagnostic.toStdString();
    document = {};
    payload.reset();
    const auto native = motrack::readWorkspaceDocument(result.nativePath);
    ASSERT_TRUE(native.archivePayload);
    EXPECT_EQ(native.project, expectedProject);
    EXPECT_EQ(bytes(native.archivePayload->sourcePath()), originalArchive);
    const auto standard = motrack::TrackerArchive::open(result.standardPath);
    EXPECT_EQ(standard.projects(), native.archivePayload->projects());
    EXPECT_EQ(bytes(standard.projectPath("second.trk")), otherExperiment);
    const auto selected = motrack::importTrackerDocument(standard.projectPath("experiment.trk"));
    EXPECT_EQ(selected.project.pointMasses, native.project.pointMasses);
    for (int i = 0; i < 4; ++i)
    {
        const auto member = QString("images/frame%1.png").arg(i);
        EXPECT_EQ(bytes(QDir(standard.root()).filePath(member)), bytes(fixture(member)));
    }
    EXPECT_EQ(files(temporary), (QStringList{"saved.motrack", "saved.trz"}));
}

TEST(PairedSave, CancellationAtEveryBoundaryRetainsARecoverablePair)
{
    using Progress = motrack::PairedSaveProgress;
    const auto document = editedDocument();
    for (const auto boundary :
         {Progress::StandardPrepared, Progress::NativePrepared, Progress::RecoveryRecorded, Progress::StandardSaved, Progress::NativeSaved})
    {
        SCOPED_TRACE(static_cast<int>(boundary));
        QTemporaryDir temporary;
        std::atomic_bool cancelled{false};
        const auto result = motrack::writePairedDocuments(temporary.filePath("saved.trk"),
                                                          document,
                                                          fixture(),
                                                          &cancelled,
                                                          [&cancelled, boundary](auto state) { cancelled = state == boundary; });
        const bool journaled = boundary >= Progress::RecoveryRecorded;
        EXPECT_EQ(result.status, journaled ? motrack::PairedSaveStatus::RecoveryRequired : motrack::PairedSaveStatus::Cancelled);
        EXPECT_EQ(result.standardCurrent, boundary >= Progress::StandardSaved);
        EXPECT_EQ(result.nativeCurrent, boundary >= Progress::NativeSaved);
        EXPECT_EQ(QFileInfo::exists(result.standardPath), boundary >= Progress::StandardSaved);
        EXPECT_EQ(QFileInfo::exists(result.nativePath), boundary >= Progress::NativeSaved);
        if (!journaled)
        {
            EXPECT_TRUE(files(temporary).empty());
            continue;
        }
        ASSERT_TRUE(QFileInfo::exists(result.recoveryPath));
        EXPECT_EQ(motrack::writePairedDocuments(result.standardPath, document, fixture()).status,
                  motrack::PairedSaveStatus::RecoveryRequired);
        const auto savedFiles = files(temporary);
        EXPECT_EQ(motrack::recoverPairedDocuments(result.recoveryPath, &cancelled).status, motrack::PairedSaveStatus::RecoveryRequired);
        EXPECT_EQ(files(temporary), savedFiles);
        cancelled = false;
        const auto recovered = motrack::recoverPairedDocuments(result.recoveryPath, &cancelled);
        ASSERT_EQ(recovered.status, motrack::PairedSaveStatus::Saved) << recovered.diagnostic.toStdString();
        EXPECT_TRUE(recovered.standardCurrent && recovered.nativeCurrent);
        EXPECT_EQ(motrack::readWorkspaceDocument(recovered.nativePath).project,
                  motrack::importTrackerDocument(recovered.standardPath).project);
        EXPECT_EQ(files(temporary), (QStringList{"saved.motrack", "saved.trk"}));
    }
}

TEST(PairedSave, RefusesExternalChangesDuringRecoveryAndPreservesThePreviousCompanion)
{
    const auto document = editedDocument();
    for (const int changed : {0, 1})
    {
        SCOPED_TRACE(changed);
        QTemporaryDir temporary;
        const auto target = temporary.filePath("saved.trk");
        const auto initial = motrack::writePairedDocuments(target, motrack::importTrackerDocument(fixture()), fixture());
        ASSERT_EQ(initial.status, motrack::PairedSaveStatus::Saved);
        const auto previousNative = bytes(initial.nativePath);
        std::atomic_bool cancelled{false};
        const auto partial =
            motrack::writePairedDocuments(target,
                                          document,
                                          fixture(),
                                          &cancelled,
                                          [&cancelled](auto state) { cancelled = state == motrack::PairedSaveProgress::StandardSaved; });
        ASSERT_EQ(partial.status, motrack::PairedSaveStatus::RecoveryRequired);
        EXPECT_EQ(bytes(partial.nativePath), previousNative);
        EXPECT_DOUBLE_EQ(motrack::importTrackerDocument(target).project.pointMasses.at(0).mass, 5);
        const auto changedPath = changed == 0 ? target : partial.nativePath;
        const auto restore = bytes(changedPath);
        put(changedPath, "external edit");
        const auto beforeStandard = bytes(target);
        const auto beforeNative = bytes(partial.nativePath);
        const auto refused = motrack::recoverPairedDocuments(partial.recoveryPath);
        EXPECT_EQ(refused.status, motrack::PairedSaveStatus::RecoveryRequired);
        EXPECT_EQ(bytes(target), beforeStandard);
        EXPECT_EQ(bytes(partial.nativePath), beforeNative);
        EXPECT_EQ(refused.standardCurrent, changed == 1);
        put(changedPath, restore);
        const auto recovered = motrack::recoverPairedDocuments(partial.recoveryPath);
        ASSERT_EQ(recovered.status, motrack::PairedSaveStatus::Saved) << recovered.diagnostic.toStdString();
        EXPECT_DOUBLE_EQ(motrack::readWorkspaceDocument(partial.nativePath).project.pointMasses.at(0).mass, 5);
    }
}

TEST(PairedSave, RefusesChangedOrMissingStagesButCanFinishInterruptedCleanup)
{
    const auto document = editedDocument();
    for (const int kind : {0, 1, 2, 3})
    {
        SCOPED_TRACE(kind);
        QTemporaryDir temporary;
        std::atomic_bool cancelled{false};
        const auto boundary = kind < 2 ? motrack::PairedSaveProgress::StandardSaved : motrack::PairedSaveProgress::NativeSaved;
        const auto partial = motrack::writePairedDocuments(temporary.filePath("saved.trk"),
                                                           document,
                                                           fixture(),
                                                           &cancelled,
                                                           [&cancelled, boundary](auto state) { cancelled = state == boundary; });
        ASSERT_EQ(partial.status, motrack::PairedSaveStatus::RecoveryRequired);
        const auto prepared = stage(partial.recoveryPath, 1);
        const auto original = bytes(prepared);
        if (kind == 0 || kind == 3)
            put(prepared, "changed stage");
        else
            ASSERT_TRUE(QFile::remove(prepared));
        auto recovered = motrack::recoverPairedDocuments(partial.recoveryPath);
        if (kind != 2)
        {
            EXPECT_EQ(recovered.status, motrack::PairedSaveStatus::RecoveryRequired);
            EXPECT_TRUE(QFileInfo::exists(partial.recoveryPath));
            put(prepared, original);
            recovered = motrack::recoverPairedDocuments(partial.recoveryPath);
        }
        EXPECT_EQ(recovered.status, motrack::PairedSaveStatus::Saved) << recovered.diagnostic.toStdString();
        EXPECT_EQ(files(temporary), (QStringList{"saved.motrack", "saved.trk"}));
    }
}

TEST(PairedSave, InvalidRecoveryRecordsCannotReplaceEitherOutput)
{
    QTemporaryDir temporary;
    std::atomic_bool cancelled{false};
    const auto document = editedDocument();
    const auto partial =
        motrack::writePairedDocuments(temporary.filePath("saved.trk"),
                                      document,
                                      fixture(),
                                      &cancelled,
                                      [&cancelled](auto state) { cancelled = state == motrack::PairedSaveProgress::RecoveryRecorded; });
    ASSERT_EQ(partial.status, motrack::PairedSaveStatus::RecoveryRequired);
    const auto original = bytes(partial.recoveryPath);
    const auto initial = record(partial.recoveryPath);
    const std::vector<std::function<void(QJsonObject&)>> mutations{[](auto& json) { json["version"] = 2; },
                                                                   [](auto& json) { json["format"] = "Other"; },
                                                                   [](auto& json) { json["outputs"] = QJsonArray{}; },
                                                                   [](auto& json)
                                                                   {
                                                                       auto outputs = json["outputs"].toArray();
                                                                       outputs[0] = false;
                                                                       json["outputs"] = outputs;
                                                                   }};
    for (const auto& mutate : mutations)
    {
        auto json = initial;
        mutate(json);
        put(partial.recoveryPath, QJsonDocument(json).toJson());
        EXPECT_EQ(motrack::recoverPairedDocuments(partial.recoveryPath).status, motrack::PairedSaveStatus::RecoveryRequired);
        EXPECT_FALSE(QFileInfo::exists(partial.standardPath));
        EXPECT_FALSE(QFileInfo::exists(partial.nativePath));
    }
    for (int kind = 0; kind < 12; ++kind)
    {
        SCOPED_TRACE(kind);
        auto json = initial;
        auto outputs = json["outputs"].toArray();
        auto item = outputs[0].toObject();
        switch (kind)
        {
        case 0:
            item["name"] = "../outside.trk";
            break;
        case 1:
            item["stage"] = "../outside.trk";
            break;
        case 2:
            item["stage"] = item["name"];
            break;
        case 3:
            item["name"] = "other.trk";
            break;
        case 4:
            item["stage"] = ".motrack-stage-invalid.motrack";
            break;
        case 5:
            item.remove("before");
            break;
        case 6:
            item["before"] = false;
            break;
        case 7:
            item["after"] = QJsonObject{{"size", 1.5}, {"sha256", QString(64, 'a')}};
            break;
        case 8:
            item["after"] = QJsonObject{{"size", -1}, {"sha256", QString(64, 'a')}};
            break;
        case 9:
            item["after"] = QJsonObject{{"size", 0}, {"sha256", QString(64, 'a')}};
            break;
        case 10:
            item["after"] = QJsonObject{{"size", 1}, {"sha256", "wrong"}};
            break;
        case 11:
            item["after"] = QJsonObject{{"size", 1000000000}, {"sha256", QString(64, 'a')}};
            break;
        default:
            break;
        }
        outputs[0] = item;
        json["outputs"] = outputs;
        put(partial.recoveryPath, QJsonDocument(json).toJson());
        EXPECT_EQ(motrack::recoverPairedDocuments(partial.recoveryPath).status, motrack::PairedSaveStatus::RecoveryRequired);
        EXPECT_FALSE(QFileInfo::exists(partial.standardPath));
        EXPECT_FALSE(QFileInfo::exists(partial.nativePath));
    }
    for (const auto& invalid : {QByteArray{}, QByteArray("{"), QByteArray(65537, ' ')})
    {
        put(partial.recoveryPath, invalid);
        EXPECT_EQ(motrack::recoverPairedDocuments(partial.recoveryPath).status, motrack::PairedSaveStatus::RecoveryRequired);
    }
    put(partial.recoveryPath, original);
    EXPECT_EQ(motrack::recoverPairedDocuments(partial.recoveryPath).status, motrack::PairedSaveStatus::Saved);
    EXPECT_EQ(files(temporary), (QStringList{"saved.motrack", "saved.trk"}));
}

TEST(PairedSave, RecoversAfterTheWriterProcessExitsWithoutCleanup)
{
    QTemporaryDir temporary;
    QProcess child;
    const auto target = temporary.filePath("saved.trk");
    child.start(QString::fromUtf8(MOTRACK_CORPUS_PROBE), {"--interrupt-paired-save", fixture(), target});
    ASSERT_TRUE(child.waitForStarted(5000));
    ASSERT_TRUE(child.waitForFinished(15000));
    ASSERT_EQ(child.exitStatus(), QProcess::NormalExit) << child.readAllStandardError().toStdString();
    ASSERT_EQ(child.exitCode(), 0) << child.readAllStandardError().toStdString();
    EXPECT_TRUE(QFileInfo::exists(target));
    EXPECT_FALSE(QFileInfo::exists(temporary.filePath("saved.motrack")));
    const auto result = motrack::recoverPairedDocuments(temporary.filePath(".saved.motrack-save.json"));
    ASSERT_EQ(result.status, motrack::PairedSaveStatus::Saved) << result.diagnostic.toStdString();
    EXPECT_EQ(motrack::readWorkspaceDocument(result.nativePath).project, motrack::importTrackerDocument(target).project);
    EXPECT_EQ(files(temporary), (QStringList{"saved.motrack", "saved.trk"}));
}

TEST(PairedSave, DetectsDestinationChangesDuringPreparationAndCleansStages)
{
    QTemporaryDir temporary;
    const auto target = temporary.filePath("saved.trk");
    const auto native = temporary.filePath("saved.motrack");
    put(target, "previous standard");
    put(native, "previous native");
    const auto result = motrack::writePairedDocuments(target,
                                                      editedDocument(),
                                                      fixture(),
                                                      nullptr,
                                                      [&](auto state)
                                                      {
                                                          if (state == motrack::PairedSaveProgress::NativePrepared)
                                                              put(target, "external edit");
                                                      });
    EXPECT_EQ(result.status, motrack::PairedSaveStatus::Failed);
    EXPECT_FALSE(result.standardCurrent || result.nativeCurrent);
    EXPECT_EQ(bytes(target), "external edit");
    EXPECT_EQ(bytes(native), "previous native");
    EXPECT_EQ(files(temporary), (QStringList{"saved.motrack", "saved.trk"}));
}

TEST(PairedSave, RefusesInvalidInputsUnsupportedDocumentsAndConcurrentWritersBeforeCommit)
{
    QTemporaryDir temporary;
    const auto document = editedDocument();
    std::atomic_bool cancelled{true};
    EXPECT_EQ(motrack::writePairedDocuments(temporary.filePath("saved.trk"), document, fixture(), &cancelled).status,
              motrack::PairedSaveStatus::Cancelled);
    for (const auto* name : {"saved.json", "missing/saved.trk", ".trk", "bad:name.trk"})
        EXPECT_EQ(motrack::writePairedDocuments(temporary.filePath(name), document, fixture()).status, motrack::PairedSaveStatus::Failed);
    EXPECT_TRUE(files(temporary).empty());
    const auto destination = temporary.filePath("saved.trk");
    const auto native = temporary.filePath("saved.motrack");
    put(destination, "previous standard");
    put(native, "previous native");
    auto unsupported = document;
    unsupported.trackerSource.reset();
    EXPECT_EQ(motrack::writePairedDocuments(destination, unsupported, fixture()).status, motrack::PairedSaveStatus::Failed);
    EXPECT_EQ(bytes(destination), "previous standard");
    EXPECT_EQ(bytes(native), "previous native");
    bool nestedCalled = false;
    const auto result = motrack::writePairedDocuments(destination,
                                                      document,
                                                      fixture(),
                                                      nullptr,
                                                      [&](auto state)
                                                      {
                                                          if (state == motrack::PairedSaveProgress::StandardPrepared)
                                                          {
                                                              nestedCalled = true;
                                                              const auto nested =
                                                                  motrack::writePairedDocuments(destination, document, fixture());
                                                              EXPECT_EQ(nested.status, motrack::PairedSaveStatus::Failed);
                                                              EXPECT_TRUE(nested.diagnostic.contains("already in progress"));
                                                          }
                                                      });
    EXPECT_TRUE(nestedCalled);
    EXPECT_EQ(result.status, motrack::PairedSaveStatus::Saved) << result.diagnostic.toStdString();
    EXPECT_EQ(files(temporary), (QStringList{"saved.motrack", "saved.trk"}));
    EXPECT_EQ(motrack::recoverPairedDocuments(temporary.filePath("wrong.json")).status, motrack::PairedSaveStatus::RecoveryRequired);
}

#ifndef Q_OS_WIN
TEST(PairedSave, RejectsFileAliasesAndRecoversThroughADirectoryAlias)
{
    QTemporaryDir temporary;
    const auto physical = temporary.filePath("physical");
    const auto alias = temporary.filePath("alias");
    ASSERT_TRUE(QDir().mkdir(physical));
    ASSERT_TRUE(QFile::link(physical, alias));
    const auto document = editedDocument();
    const auto standard = physical + "/saved.trk";
    const auto native = physical + "/saved.motrack";
    const auto outside = temporary.filePath("outside.trk");
    for (const bool existing : {false, true})
    {
        SCOPED_TRACE(existing);
        if (existing)
            put(outside, "unrelated original");
        for (const auto& linked : {standard, native})
        {
            ASSERT_TRUE(QFile::link(outside, linked));
            EXPECT_EQ(motrack::writePairedDocuments(alias + "/saved.trk", document, fixture()).status, motrack::PairedSaveStatus::Failed);
            EXPECT_TRUE(QFileInfo(linked).isSymLink());
            if (existing)
                EXPECT_EQ(bytes(outside), "unrelated original");
            else
                EXPECT_FALSE(QFileInfo::exists(outside));
            ASSERT_TRUE(QFile::remove(linked));
        }
    }
    std::atomic_bool cancelled{false};
    const auto partial =
        motrack::writePairedDocuments(alias + "/saved.trk",
                                      document,
                                      fixture(),
                                      &cancelled,
                                      [&cancelled](auto state) { cancelled = state == motrack::PairedSaveProgress::StandardSaved; });
    ASSERT_EQ(partial.status, motrack::PairedSaveStatus::RecoveryRequired);
    const auto prepared = stage(partial.recoveryPath, 1);
    const auto preparedBytes = bytes(prepared);
    ASSERT_TRUE(QFile::remove(prepared));
    put(outside, preparedBytes);
    ASSERT_TRUE(QFile::link(outside, prepared));
    EXPECT_EQ(motrack::recoverPairedDocuments(partial.recoveryPath).status, motrack::PairedSaveStatus::RecoveryRequired);
    EXPECT_EQ(bytes(outside), preparedBytes);
    ASSERT_TRUE(QFile::remove(prepared));
    put(prepared, preparedBytes);
    const auto recoveryAlias = temporary.filePath(".saved.motrack-save.json");
    ASSERT_TRUE(QFile::link(partial.recoveryPath, recoveryAlias));
    EXPECT_EQ(motrack::recoverPairedDocuments(recoveryAlias).status, motrack::PairedSaveStatus::RecoveryRequired);
    const auto result = motrack::recoverPairedDocuments(alias + "/.saved.motrack-save.json");
    ASSERT_EQ(result.status, motrack::PairedSaveStatus::Saved) << result.diagnostic.toStdString();
    const auto reopened = motrack::readWorkspaceDocument(result.nativePath);
    for (std::size_t i = 0; i < reopened.project.clip.mediaReferences.size(); ++i)
        EXPECT_EQ(bytes(motrack::resolveLocalMediaPath(result.nativePath, reopened.project.clip.mediaReferences[i])),
                  bytes(fixture(QString("images/frame%1.png").arg(i))));
}
#endif
} // namespace
