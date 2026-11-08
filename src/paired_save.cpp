#include "paired_save.hpp"

#include "local_media_path.hpp"
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
#include <QSet>
#include <QTemporaryFile>

#include <array>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <utility>

namespace motrack
{
namespace
{
constexpr qint64 BLOCK_BYTES = 64 * 1024;
constexpr qint64 MAX_RECORD_BYTES = 64 * 1024;
constexpr qint64 MAX_NATIVE_BYTES = MAX_WORKSPACE_JSON_BYTES + MAX_TRACKER_ARCHIVE_BYTES + qint64{8} * 1024 * 1024;
constexpr auto RECORD_SUFFIX = ".motrack-save.json";

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
    require(size.isDouble() && std::isfinite(size.toDouble()) && size.toDouble() >= 0 && size.toDouble() <= limit &&
                std::trunc(size.toDouble()) == size.toDouble() && digest.isString() && digest.toString().contains(sha256),
            "Invalid paired-save fingerprint.");
    return {.size = static_cast<qint64>(size.toDouble()), .digest = QByteArray::fromHex(digest.toString().toLatin1())};
}

void validateRecord(const Record& record)
{
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
    const auto bytes = QJsonDocument(QJsonObject{{"format", "MoTrackPairedSave"}, {"version", 1}, {"outputs", outputs}}).toJson();
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
    const auto json = QJsonDocument::fromJson(input.read(MAX_RECORD_BYTES + 1));
    require(json.isObject(), "Invalid paired-save recovery JSON.");
    const auto object = json.object();
    require(object.value("format") == "MoTrackPairedSave" && object.value("version") == 1 && object.value("outputs").isArray(),
            "Unsupported paired-save recovery record.");
    const auto outputs = object.value("outputs").toArray();
    require(outputs.size() == 2, "Paired-save recovery requires exactly two outputs.");
    Record result{.directory = directory, .name = info.fileName(), .outputs = {}};
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
    validateRecord(result);
    return result;
}

void copyAtomic(const Record& record, const Output& entry, const std::atomic_bool* cancelled)
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
    require(fingerprint(directory.filePath(entry.name), record.directory, cancelled) == entry.before,
            "A destination changed during paired saving; recovery data was retained.");
    checkCancelled(cancelled);
    require(output.commit(), "Could not replace a paired-save destination.");
}

void commitPair(const Record& record, PairedSaveResult& result, const std::atomic_bool* cancelled, const PairedSaveObserver& observer)
{
    const QDir directory(record.directory);
    std::array<bool, 2> current{};
    for (std::size_t i = 0; i < record.outputs.size(); ++i)
    {
        const auto& output = record.outputs[i];
        const auto actual = fingerprint(directory.filePath(output.name), record.directory, cancelled);
        current[i] = actual == std::optional(output.after);
        require(current[i] || actual == output.before, "A destination changed since paired saving; no further replacement is allowed.");
        if (!current[i])
            require(fingerprint(directory.filePath(output.stage), record.directory, cancelled) == std::optional(output.after),
                    "Prepared paired-save content is missing or changed.");
    }
    result.standardCurrent = current[0];
    result.nativeCurrent = current[1];
    for (std::size_t i = 0; i < record.outputs.size(); ++i)
    {
        checkCancelled(cancelled);
        if (!current[i])
            copyAtomic(record, record.outputs[i], cancelled);
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
    // Verify both final contents before deleting any recovery material. No rollback can overwrite
    // an external edit. Missing stages are harmless once their corresponding final file is complete.
    for (const auto& output : record.outputs)
    {
        require(fingerprint(directory.filePath(output.name), record.directory, cancelled) == std::optional(output.after),
                "A completed paired-save output changed before cleanup.");
        const auto staged = fingerprint(directory.filePath(output.stage), record.directory, cancelled);
        require(!staged || staged == std::optional(output.after), "A recovery staging file changed; it was preserved.");
    }
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
            const auto media = resolveLocalMediaPath(mediaProjectPath, reference);
            for (const auto& output : record.outputs)
                require(media.isEmpty() || media != directory.filePath(output.name), "Paired saving cannot overwrite linked media.");
        }
    }
}

QString lockPath(const QString& recordPath)
{
    return recordPath.chopped(5) + ".lock";
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
        Record record{.directory = parent, .name = '.' + stem + RECORD_SUFFIX, .outputs = {}};
        record.outputs[0].name = target.fileName();
        record.outputs[1].name = stem + ".motrack";
        result.standardPath = directory.filePath(record.outputs[0].name);
        result.nativePath = directory.filePath(record.outputs[1].name);
        require((target.suffix().compare("trk", Qt::CaseInsensitive) == 0 || target.suffix().compare("trz", Qt::CaseInsensitive) == 0) &&
                    !stem.isEmpty() && safeName(record.name) && safeName(record.outputs[0].name) && safeName(record.outputs[1].name),
                "Paired saving requires a standard Tracker filename and a same-stem native companion.");
        const auto recoveryPath = directory.filePath(record.name);
        QLockFile lock(lockPath(recoveryPath));
        require(lock.tryLock(0), "Another save for this project is already in progress.");
        if (QFileInfo::exists(recoveryPath) || QFileInfo(recoveryPath).isSymLink())
        {
            result.recoveryPath = recoveryPath;
            throw ProjectError("Resolve the previous paired save before replacing either output.");
        }
        protectSourceFiles(record, document, mediaProjectPath);
        for (auto& output : record.outputs)
            output.before = fingerprint(directory.filePath(output.name), parent, cancelled);
        QTemporaryFile standard(directory.filePath(".motrack-stage-XXXXXX." + target.suffix()));
        QTemporaryFile native(directory.filePath(".motrack-stage-XXXXXX.motrack"));
        require(standard.open() && native.open(), "Could not create paired-save staging files.");
        standard.close();
        native.close();
        record.outputs[0].stage = QFileInfo(standard.fileName()).fileName();
        record.outputs[1].stage = QFileInfo(native.fileName()).fileName();
        if (target.suffix().compare("trz", Qt::CaseInsensitive) == 0)
            writeTrackerArchiveDocument(standard.fileName(), document, cancelled);
        else
            writeTrackerDocument(standard.fileName(), document, mediaProjectPath);
        notify(observer, PairedSaveProgress::StandardPrepared);
        checkCancelled(cancelled);
        auto snapshot = document;
        rebaseWorkspaceMedia(snapshot, mediaProjectPath, result.nativePath);
        writeWorkspaceDocument(native.fileName(), snapshot);
        validateNativeSnapshot(snapshot, readWorkspaceDocument(native.fileName(), cancelled));
        notify(observer, PairedSaveProgress::NativePrepared);
        for (auto& output : record.outputs)
        {
            const auto prepared = fingerprint(directory.filePath(output.stage), parent, cancelled);
            require(prepared.has_value(), "Prepared paired-save file disappeared.");
            output.after = prepared.value();
            require(fingerprint(directory.filePath(output.name), parent, cancelled) == output.before,
                    "A destination changed during paired-save preparation.");
        }
        checkCancelled(cancelled);
        writeRecord(record);
        standard.setAutoRemove(false);
        native.setAutoRemove(false);
        result.recoveryPath = recoveryPath;
        notify(observer, PairedSaveProgress::RecoveryRecorded);
        commitPair(record, result, cancelled, observer);
    }
    catch (const Cancelled& error)
    {
        failed(result, error, true);
    }
    catch (const std::exception& error)
    {
        failed(result, error, false);
    }
    return result;
}

PairedSaveResult recoverPairedDocuments(const QString& recoveryPath, const std::atomic_bool* cancelled, const PairedSaveObserver& observer)
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
        require(lock.tryLock(0), "Another save for this project is already in progress.");
        const auto record = readRecord(result.recoveryPath, parent);
        result.standardPath = QDir(parent).filePath(record.outputs[0].name);
        result.nativePath = QDir(parent).filePath(record.outputs[1].name);
        commitPair(record, result, cancelled, observer);
    }
    catch (const Cancelled& error)
    {
        failed(result, error, true);
    }
    catch (const std::exception& error)
    {
        failed(result, error, false);
    }
    return result;
}
} // namespace motrack
