#include "paired_save.hpp"

#include "local_media_path.hpp"
#include "save_observation.hpp"
#include "tracker_archive.hpp"
#include "tracker_writer.hpp"
#include "workspace_storage.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSet>
#include <QTemporaryFile>
#include <QUuid>

#include <array>
#include <cmath>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace motrack
{
namespace
{
void stampObservation(PairedSaveResult& result)
{
    if (result.observation == 0)
        result.observation = nextSaveObservation();
}

constexpr qint64 BLOCK_BYTES = qint64{64} * 1024;
constexpr qint64 MAX_RECORD_BYTES = qint64{64} * 1024;
constexpr qint64 MAX_NATIVE_BYTES = MAX_WORKSPACE_JSON_BYTES + MAX_TRACKER_ARCHIVE_BYTES + qint64{8} * 1024 * 1024;
constexpr auto RECORD_SUFFIX = ".motrack-save.json";

QString lockPath(const QString& recordPath)
{
    return recordPath.chopped(5) + ".lock";
}

struct Cancelled : std::exception
{
    [[nodiscard]] const char* what() const noexcept override
    {
        return "Paired save canceled.";
    }
};

void require(bool condition, const char* message)
{
    if (!condition)
        throw ProjectError(message);
}

void checkCancelled(const std::atomic_bool* cancelled)
{
    if (cancelled && cancelled->load())
        throw Cancelled{};
}

void notify(const PairedSaveObserver& observer, PairedSaveProgress progress)
{
    if (observer)
        observer(progress);
}

struct Fingerprint
{
    qint64 size;
    QByteArray digest;
    bool operator==(const Fingerprint&) const = default;
};

struct Output
{
    QString name;
    QString stage;
    std::optional<Fingerprint> before;
    Fingerprint after;
};

struct Record
{
    QString directory;
    QString name;
    QString transactionId;
    std::array<Output, 2> outputs;
};

bool safeName(const QString& name)
{
    static const QRegularExpression forbidden(R"([\\/:*?"<>|])");
    return !name.isEmpty() && name.size() <= 512 && name != "." && name != ".." && !name.contains(forbidden) &&
           !name.contains(QChar::Null) && !name.endsWith('.') && !name.endsWith(' ');
}

qint64 fileLimit(const QString& name)
{
    if (name.endsWith(".trk", Qt::CaseInsensitive))
        return qint64{32} * 1024 * 1024;
    if (name.endsWith(".trz", Qt::CaseInsensitive))
        return MAX_TRACKER_ARCHIVE_BYTES;
    require(name.endsWith(".motrack", Qt::CaseInsensitive), "Unsupported paired-save filename.");
    return MAX_NATIVE_BYTES;
}

std::optional<Fingerprint> fingerprint(const QString& path, const QString& directory, const std::atomic_bool* cancelled)
{
    checkCancelled(cancelled);
    const QFileInfo info(path);
    require(!info.isSymLink(), "Paired saving cannot replace file aliases.");
    if (!info.exists())
        return std::nullopt;
    require(info.isFile() && QFileInfo(info.canonicalFilePath()).path() == directory,
            "A paired-save file is outside its physical destination directory.");
    QFile input(path);
    require(input.open(QIODevice::ReadOnly), "Could not read a paired-save file.");
    const auto expectedSize = input.size();
    require(expectedSize >= 0 && expectedSize <= fileLimit(info.fileName()), "Paired-save file size limit exceeded.");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    std::array<char, BLOCK_BYTES> buffer{};
    qint64 total = 0;
    for (;;)
    {
        checkCancelled(cancelled);
        const auto count = input.read(buffer.data(), BLOCK_BYTES);
        require(count >= 0 && count <= expectedSize - total, "A paired-save file changed while reading.");
        if (count == 0)
            break;
        hash.addData(QByteArrayView(buffer.data(), count));
        total += count;
    }
    require(total == expectedSize, "A paired-save file changed while reading.");
    return Fingerprint{.size = total, .digest = hash.result()};
}

QJsonObject jsonFingerprint(const Fingerprint& value)
{
    return {{"size", value.size}, {"sha256", QString::fromLatin1(value.digest.toHex())}};
}

Fingerprint parseFingerprint(const QJsonValue& value, qint64 limit)
{
    require(value.isObject(), "Invalid paired-save fingerprint.");
    const auto object = value.toObject();
    const auto size = object.value("size");
    const auto digest = object.value("sha256");
    static const QRegularExpression sha256("^[0-9a-f]{64}$");
    require(size.isDouble() && std::isfinite(size.toDouble()) && size.toDouble() >= 0 && size.toDouble() <= static_cast<double>(limit) &&
                std::trunc(size.toDouble()) == size.toDouble() && digest.isString() && digest.toString().contains(sha256),
            "Invalid paired-save fingerprint.");
    return {.size = static_cast<qint64>(size.toDouble()), .digest = QByteArray::fromHex(digest.toString().toLatin1())};
}

void validateRecord(const Record& record)
{
    const QUuid transaction(record.transactionId);
    require(!transaction.isNull() && transaction.toString(QUuid::WithoutBraces) == record.transactionId,
            "Invalid paired-save transaction identity.");
    const auto& standard = record.outputs[0];
    const auto& native = record.outputs[1];
    require(standard.name.endsWith(".trk", Qt::CaseInsensitive) || standard.name.endsWith(".trz", Qt::CaseInsensitive),
            "Paired saving requires a Tracker standard filename.");
    const auto stem = QFileInfo(standard.name).completeBaseName();
    require(!stem.isEmpty() && native.name == stem + ".motrack" && record.name == '.' + stem + RECORD_SUFFIX,
            "Paired-save filenames do not describe one sibling pair.");
    QSet<QString> names;
    for (const auto& name : {record.name, standard.name, native.name, standard.stage, native.stage})
    {
        const auto key = name.normalized(QString::NormalizationForm_C).toCaseFolded();
        require(safeName(name) && !names.contains(key), "Ambiguous or escaping paired-save filename.");
        names.insert(key);
    }
    for (const auto& output : record.outputs)
    {
        require(output.stage.startsWith(".motrack-stage-") &&
                    QFileInfo(output.stage).suffix().compare(QFileInfo(output.name).suffix(), Qt::CaseInsensitive) == 0 &&
                    output.after.size > 0 && output.after.size <= fileLimit(output.name),
                "Invalid paired-save staging file.");
    }
}

void writeRecord(const Record& record)
{
    validateRecord(record);
    QJsonArray outputs;
    for (const auto& output : record.outputs)
        outputs.append(QJsonObject{{"name", output.name},
                                   {"stage", output.stage},
                                   {"before", output.before ? QJsonValue(jsonFingerprint(*output.before)) : QJsonValue::Null},
                                   {"after", jsonFingerprint(output.after)}});
    const auto bytes =
        QJsonDocument(
            QJsonObject{{"format", "MoTrackPairedSave"}, {"version", 1}, {"transaction", record.transactionId}, {"outputs", outputs}})
            .toJson();
    require(bytes.size() <= MAX_RECORD_BYTES, "Paired-save recovery record exceeds its limit.");
    QSaveFile file(QDir(record.directory).filePath(record.name));
    require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit(),
            "Could not record paired-save recovery information.");
}

Record readRecord(const QString& path, const QString& directory)
{
    const QFileInfo info(path);
    require(info.isFile() && !info.isSymLink() && QFileInfo(info.canonicalFilePath()).path() == directory,
            "Invalid paired-save recovery location.");
    QFile input(path);
    require(input.open(QIODevice::ReadOnly) && input.size() > 0 && input.size() <= MAX_RECORD_BYTES,
            "Could not read bounded paired-save recovery information.");
    const auto bytes = input.read(MAX_RECORD_BYTES + 1);
    require(bytes.size() <= MAX_RECORD_BYTES && input.atEnd(), "Paired-save recovery record changed while reading.");
    const auto json = QJsonDocument::fromJson(bytes);
    require(json.isObject(), "Invalid paired-save recovery JSON.");
    const auto object = json.object();
    require(object.value("format") == "MoTrackPairedSave" && object.value("version") == 1 && object.value("outputs").isArray(),
            "Unsupported paired-save recovery record.");
    const auto outputs = object.value("outputs").toArray();
    require(outputs.size() == 2, "Paired-save recovery requires exactly two outputs.");
    Record result{.directory = directory, .name = {}, .transactionId = object.value("transaction").toString(), .outputs = {}};
    for (int i = 0; i < 2; ++i)
    {
        require(outputs[i].isObject(), "Invalid paired-save output record.");
        const auto item = outputs[i].toObject();
        auto& output = result.outputs[static_cast<std::size_t>(i)];
        require(item.value("name").isString() && item.value("stage").isString() && item.contains("before"),
                "Invalid paired-save output fields.");
        output.name = item.value("name").toString();
        output.stage = item.value("stage").toString();
        const auto limit = fileLimit(output.name);
        output.after = parseFingerprint(item.value("after"), limit);
        if (!item.value("before").isNull())
            output.before = parseFingerprint(item.value("before"), limit);
    }
    result.name = '.' + QFileInfo(result.outputs[0].name).completeBaseName() + RECORD_SUFFIX;
    validateRecord(result);
    const auto declaredPath = QDir(directory).filePath(result.name);
    // Both spellings must use the lock already held by this caller. File identity alone would
    // also accept a hard-linked journal under a different stem, whose outputs are not locked.
    require(sameLocalFile(path, declaredPath) && QFileInfo::exists(lockPath(declaredPath)) &&
                sameLocalFile(lockPath(path), lockPath(declaredPath)),
            "Paired-save recovery filename does not match its locked sibling pair.");
    return result;
}

std::optional<Fingerprint>
inspectDestination(const Record& record, std::size_t index, PairedSaveResult& result, const std::atomic_bool* cancelled)
{
    auto& current = index == 0 ? result.standardCurrent : result.nativeCurrent;
    try
    {
        const auto actual = fingerprint(QDir(record.directory).filePath(record.outputs[index].name), record.directory, cancelled);
        current = actual == std::optional(record.outputs[index].after);
        return actual;
    }
    catch (const Cancelled&)
    {
        throw; // Cancellation does not disprove a known successful commit.
    }
    catch (const std::exception&)
    {
        current = false;
        throw;
    }
}

std::array<std::optional<Fingerprint>, 2>
inspectDestinations(const Record& record, PairedSaveResult& result, const std::atomic_bool* cancelled)
{
    std::array<std::optional<Fingerprint>, 2> actual{};
    std::exception_ptr firstError;
    for (std::size_t i = 0; i < record.outputs.size(); ++i)
    {
        try
        {
            actual[i] = inspectDestination(record, i, result, cancelled);
        }
        catch (const Cancelled&)
        {
            throw;
        }
        catch (const std::exception&)
        {
            if (!firstError)
                firstError = std::current_exception();
        }
    }
    if (firstError)
        std::rethrow_exception(firstError);
    return actual;
}

void validateDestinations(const Record& record,
                          const std::array<bool, 2>& committed,
                          PairedSaveResult& result,
                          const std::atomic_bool* cancelled)
{
    const auto actual = inspectDestinations(record, result, cancelled);
    for (std::size_t i = 0; i < record.outputs.size(); ++i)
    {
        const auto expected = committed[i] ? std::optional(record.outputs[i].after) : record.outputs[i].before;
        require(actual[i] == expected, "A destination changed during paired saving; no further replacement is allowed.");
    }
}

void validateStages(const Record& record, const std::array<bool, 2>& committed, const std::atomic_bool* cancelled)
{
    for (std::size_t i = 0; i < record.outputs.size(); ++i)
    {
        const auto& output = record.outputs[i];
        const auto staged = fingerprint(QDir(record.directory).filePath(output.stage), record.directory, cancelled);
        require(staged ? staged == std::optional(output.after) : committed[i], "Prepared paired-save content is missing or changed.");
    }
}

void copyAtomic(const Record& record,
                const Output& entry,
                const std::array<bool, 2>& committed,
                PairedSaveResult& result,
                const std::atomic_bool* cancelled)
{
    const QDir directory(record.directory);
    QFile input(directory.filePath(entry.stage));
    require(input.open(QIODevice::ReadOnly), "Could not reopen prepared paired-save content.");
    QSaveFile output(directory.filePath(entry.name));
    require(output.open(QIODevice::WriteOnly), "Could not open a paired-save destination.");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    std::array<char, BLOCK_BYTES> buffer{};
    qint64 copied = 0;
    for (;;)
    {
        checkCancelled(cancelled);
        const auto count = input.read(buffer.data(), BLOCK_BYTES);
        require(count >= 0 && count <= entry.after.size - copied, "Prepared paired-save content changed while copying.");
        if (count == 0)
            break;
        require(output.write(buffer.data(), count) == count, "Could not write paired-save content.");
        hash.addData(QByteArrayView(buffer.data(), count));
        copied += count;
    }
    require(copied == entry.after.size && hash.result() == entry.after.digest, "Prepared paired-save content changed while copying.");
    validateStages(record, committed, cancelled);
    validateDestinations(record, committed, result, cancelled);
    checkCancelled(cancelled);
    require(output.commit(), "Could not replace a paired-save destination.");
}

void commitPair(const Record& record, PairedSaveResult& result, const std::atomic_bool* cancelled, const PairedSaveObserver& observer)
{
    const QDir directory(record.directory);
    std::array<bool, 2> current{};
    const auto actual = inspectDestinations(record, result, cancelled);
    for (std::size_t i = 0; i < record.outputs.size(); ++i)
    {
        const auto& output = record.outputs[i];
        current[i] = actual[i] == std::optional(output.after);
        require(current[i] || actual[i] == output.before, "A destination changed since paired saving; no further replacement is allowed.");
    }
    validateStages(record, current, cancelled);
    for (std::size_t i = 0; i < record.outputs.size(); ++i)
    {
        checkCancelled(cancelled);
        if (!current[i])
            copyAtomic(record, record.outputs[i], current, result, cancelled);
        current[i] = true;
        if (i == 0)
        {
            result.standardCurrent = true;
            notify(observer, PairedSaveProgress::StandardSaved);
        }
        else
        {
            result.nativeCurrent = true;
            notify(observer, PairedSaveProgress::NativeSaved);
        }
    }
    // A conflict or unreadable output clears its current flag. Cancellation and cleanup errors
    // retain successful output status so callers can distinguish saved data from leftover journals.
    validateDestinations(record, current, result, cancelled);
    validateStages(record, current, cancelled);
    // Keep the record until cleanup is complete; recovery accepts already committed outputs even
    // if an interruption occurred after deleting one of these stages.
    for (const auto& output : record.outputs)
    {
        const auto path = directory.filePath(output.stage);
        require(!QFileInfo::exists(path) || QFile::remove(path), "Saved content is complete, but recovery staging cleanup failed.");
    }
    require(QFile::remove(result.recoveryPath), "Saved content is complete, but recovery record cleanup failed.");
    result.recoveryPath.clear();
    result.status = PairedSaveStatus::Saved;
}

void validateNativeSnapshot(const WorkspaceDocument& before, const WorkspaceDocument& after)
{
    require(before.project == after.project && before.movieFrameTimes == after.movieFrameTimes && before.movie == after.movie &&
                before.useDisplayOrientation == after.useDisplayOrientation && before.alignMovieToClip == after.alignMovieToClip &&
                before.archiveReference == after.archiveReference && before.archiveMember == after.archiveMember &&
                static_cast<bool>(before.archivePayload) == static_cast<bool>(after.archivePayload) &&
                before.trackerSource.has_value() == after.trackerSource.has_value(),
            "Prepared native companion did not preserve its snapshot.");
    if (before.trackerSource && after.trackerSource)
        require(before.trackerSource->xml == after.trackerSource->xml && before.trackerSource->bindings == after.trackerSource->bindings,
                "Prepared native companion did not preserve its source envelope.");
}

void protectSourceFiles(const Record& record, const WorkspaceDocument& document, const QString& mediaProjectPath)
{
    const QDir directory(record.directory);
    if (document.archivePayload)
    {
        for (const auto& root : {document.archivePayload->root(), QFileInfo(document.archivePayload->sourcePath()).path()})
        {
            const auto physical = QFileInfo(root).canonicalFilePath();
            require(!physical.isEmpty() && record.directory != physical && !record.directory.startsWith(physical + '/'),
                    "Paired saving cannot replace its owned archive files.");
        }
    }
    else
    {
        for (const auto& reference : document.project.clip.mediaReferences)
        {
            const auto media = workspaceMediaPath(document, mediaProjectPath, reference);
            for (const auto& output : record.outputs)
                require(media.isEmpty() || !sameLocalFile(media, directory.filePath(output.name)),
                        "Paired saving cannot overwrite linked media.");
        }
    }
}

QString createStage(const QString& nameTemplate)
{
    QTemporaryFile file(nameTemplate);
    require(file.open(), "Could not create a paired-save staging file.");
    const auto path = file.fileName();
    file.setAutoRemove(false);
    // Destruction releases the native handle. QTemporaryFile::close alone may retain it,
    // preventing an atomic writer from replacing this reserved file on Windows.
    return path;
}

std::shared_ptr<const QString> observeRecoveryIdentity(const QString& path)
{
    // Called with this journal's lock held. Its real lock file lets the filesystem decide
    // case/Unicode equivalence, including per-directory policies, without inventing a fold.
    // Weak entries live only while results or sessions retain the identity.
    static std::mutex mutex;
    static std::vector<std::weak_ptr<const QString>> observed;
    const std::scoped_lock guard(mutex);
    std::erase_if(observed, [](const auto& identity) { return identity.expired(); });
    for (const auto& candidate : observed)
        if (auto identity = candidate.lock(); identity && sameLocalFile(lockPath(*identity), lockPath(path)))
            return identity;
    auto identity = std::make_shared<const QString>(path);
    observed.emplace_back(identity);
    return identity;
}

void failed(PairedSaveResult& result, const std::exception& error, bool cancelled)
{
    result.diagnostic = QString::fromUtf8(error.what());
    result.status = !result.recoveryPath.isEmpty() ? PairedSaveStatus::RecoveryRequired
                    : cancelled                    ? PairedSaveStatus::Cancelled
                                                   : PairedSaveStatus::Failed;
}
} // namespace

PairedSaveResult writePairedDocuments(const QString& standardPath,
                                      const WorkspaceDocument& document,
                                      const QString& mediaProjectPath,
                                      const std::atomic_bool* cancelled,
                                      const PairedSaveObserver& observer)
{
    PairedSaveResult result;
    try
    {
        checkCancelled(cancelled);
        const QFileInfo target(standardPath);
        require(!target.isSymLink(), "Paired saving cannot replace file aliases.");
        const auto parent = QFileInfo(target.absolutePath()).canonicalFilePath();
        require(!parent.isEmpty(), "The paired-save destination directory must exist.");
        const QDir directory(parent);
        const auto stem = target.completeBaseName();
        Record record{.directory = parent,
                      .name = '.' + stem + RECORD_SUFFIX,
                      .transactionId = QUuid::createUuid().toString(QUuid::WithoutBraces),
                      .outputs = {}};
        record.outputs[0].name = target.fileName();
        record.outputs[1].name = stem + ".motrack";
        result.standardPath = directory.filePath(record.outputs[0].name);
        result.nativePath = directory.filePath(record.outputs[1].name);
        require((target.suffix().compare("trk", Qt::CaseInsensitive) == 0 || target.suffix().compare("trz", Qt::CaseInsensitive) == 0) &&
                    !stem.isEmpty() && safeName(record.name) && safeName(record.outputs[0].name) && safeName(record.outputs[1].name),
                "Paired saving requires a standard Tracker filename and a same-stem native companion.");
        const auto recoveryPath = directory.filePath(record.name);
        QLockFile lock(lockPath(recoveryPath));
        lock.setStaleLockTime(0);
        require(lock.tryLock(0), "Another save for this project is already in progress.");
        const auto observation = qScopeGuard([&result] { stampObservation(result); });
        result.observedRecoveryPath = recoveryPath;
        result.recoveryIdentity = observeRecoveryIdentity(recoveryPath);
        if (QFileInfo::exists(recoveryPath) || QFileInfo(recoveryPath).isSymLink())
        {
            result.recoveryPath = recoveryPath;
            result.transactionId = readRecord(recoveryPath, parent).transactionId;
            throw ProjectError("Resolve the previous paired save before replacing either output.");
        }
        auto snapshot = document;
        refreshWorkspaceMediaLocations(snapshot, mediaProjectPath);
        result.mediaLocations = snapshot.mediaLocations;
        protectSourceFiles(record, snapshot, mediaProjectPath);
        for (auto& output : record.outputs)
            output.before = fingerprint(directory.filePath(output.name), parent, cancelled);
        QString standard;
        QString native;
        bool retainStages = false;
        const auto cleanup = qScopeGuard(
            [&]
            {
                if (!retainStages)
                {
                    if (!standard.isEmpty())
                        QFile::remove(standard);
                    if (!native.isEmpty())
                        QFile::remove(native);
                }
            });
        standard = createStage(directory.filePath(".motrack-stage-XXXXXX." + target.suffix()));
        native = createStage(directory.filePath(".motrack-stage-XXXXXX.motrack"));
        record.outputs[0].stage = QFileInfo(standard).fileName();
        record.outputs[1].stage = QFileInfo(native).fileName();
        if (target.suffix().compare("trz", Qt::CaseInsensitive) == 0)
            writeTrackerArchiveDocument(standard, snapshot, cancelled);
        else
            writeTrackerDocument(standard, snapshot, mediaProjectPath);
        const auto standardFingerprint = fingerprint(standard, parent, cancelled);
        if (!standardFingerprint)
            throw ProjectError("Prepared standard file disappeared.");
        record.outputs[0].after = *standardFingerprint;
        notify(observer, PairedSaveProgress::StandardPrepared);
        checkCancelled(cancelled);
        rebaseWorkspaceMedia(snapshot, mediaProjectPath, result.nativePath);
        writeWorkspaceDocument(native, snapshot);
        validateNativeSnapshot(snapshot, readWorkspaceDocument(native, cancelled));
        const auto nativeFingerprint = fingerprint(native, parent, cancelled);
        if (!nativeFingerprint)
            throw ProjectError("Prepared native file disappeared.");
        record.outputs[1].after = *nativeFingerprint;
        notify(observer, PairedSaveProgress::NativePrepared);
        for (auto& output : record.outputs)
        {
            const auto prepared = fingerprint(directory.filePath(output.stage), parent, cancelled);
            if (!prepared)
                throw ProjectError("Prepared paired-save file disappeared.");
            require(*prepared == output.after, "A prepared paired-save file changed.");
            require(fingerprint(directory.filePath(output.name), parent, cancelled) == output.before,
                    "A destination changed during paired-save preparation.");
        }
        checkCancelled(cancelled);
        writeRecord(record);
        retainStages = true;
        result.recoveryPath = recoveryPath;
        result.transactionId = record.transactionId;
        notify(observer, PairedSaveProgress::RecoveryRecorded);
        commitPair(record, result, cancelled, observer);
    }
    catch (const Cancelled& error)
    {
        failed(result, error, true);
    }
    catch (const std::exception& error)
    {
        failed(result, error, cancelled && cancelled->load());
    }
    return result;
}

PairedSaveResult recoverPairedDocuments(const QString& recoveryPath,
                                        const std::atomic_bool* cancelled,
                                        const PairedSaveObserver& observer,
                                        const std::optional<QString>& expectedTransaction)
{
    PairedSaveResult result;
    result.recoveryPath = QFileInfo(recoveryPath).absoluteFilePath();
    try
    {
        checkCancelled(cancelled);
        const QFileInfo info(recoveryPath);
        const auto parent = QFileInfo(info.absolutePath()).canonicalFilePath();
        require(!parent.isEmpty() && info.fileName().startsWith('.') && info.fileName().endsWith(RECORD_SUFFIX),
                "Invalid paired-save recovery filename.");
        result.recoveryPath = QDir(parent).filePath(info.fileName());
        QLockFile lock(lockPath(result.recoveryPath));
        lock.setStaleLockTime(0);
        require(lock.tryLock(0), "Another save for this project is already in progress.");
        const auto observation = qScopeGuard([&result] { stampObservation(result); });
        result.observedRecoveryPath = result.recoveryPath;
        result.recoveryIdentity = observeRecoveryIdentity(result.recoveryPath);
        require(!expectedTransaction || expectedTransaction->isEmpty() ||
                    (!QUuid(*expectedTransaction).isNull() &&
                     QUuid(*expectedTransaction).toString(QUuid::WithoutBraces) == *expectedTransaction),
                "Invalid expected recovery transaction.");
        if (!QFileInfo::exists(result.recoveryPath) && !QFileInfo(result.recoveryPath).isSymLink())
        {
            result.transactionId = expectedTransaction.value_or(QString{});
            result.status = PairedSaveStatus::RecoveryNotNeeded;
            stampObservation(result); // Set before return-value construction, independently of NRVO.
            return result;
        }
        const auto record = readRecord(result.recoveryPath, parent);
        result.transactionId = record.transactionId;
        result.standardPath = QDir(parent).filePath(record.outputs[0].name);
        result.nativePath = QDir(parent).filePath(record.outputs[1].name);
        require(!expectedTransaction || *expectedTransaction == record.transactionId,
                "The pending save changed in another instance. Confirm recovery of the new transaction before continuing.");
        commitPair(record, result, cancelled, observer);
    }
    catch (const Cancelled& error)
    {
        failed(result, error, true);
    }
    catch (const std::exception& error)
    {
        failed(result, error, cancelled && cancelled->load());
    }
    return result;
}
} // namespace motrack
