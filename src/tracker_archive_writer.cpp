#include "tracker_archive_writer.hpp"

#include "tracker_archive.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace motrack
{
namespace
{
constexpr qint64 BLOCK_BYTES = qint64{64} * 1024;
constexpr qint64 MAX_DIRECTORY_BYTES = qint64{8} * 1024 * 1024;

void require(bool valid, const char* message = "Invalid or unsupported Tracker archive export structure.")
{
    if (!valid)
        throw ProjectError(message);
}

void checkCancelled(const std::atomic_bool* cancelled)
{
    require(!cancelled || !cancelled->load(), "Tracker archive export cancelled.");
}

template <typename T> T number(const QByteArray& bytes, qsizetype offset)
{
    require(offset >= 0 && offset <= bytes.size() && static_cast<qsizetype>(sizeof(T)) <= bytes.size() - offset);
    return qFromLittleEndian<T>(bytes.constData() + offset);
}

template <typename T> void setNumber(QByteArray& bytes, qsizetype offset, T value)
{
    require(offset >= 0 && offset <= bytes.size() && static_cast<qsizetype>(sizeof(T)) <= bytes.size() - offset);
    qToLittleEndian<T>(value, bytes.data() + offset);
}

QByteArray readAt(QFile& file, qint64 offset, qint64 size)
{
    require(offset >= 0 && size >= 0 && size <= MAX_DIRECTORY_BYTES && offset <= file.size() && size <= file.size() - offset &&
            file.seek(offset));
    auto bytes = file.read(size);
    require(bytes.size() == size);
    return bytes;
}

void checkExtra(const QByteArray& extra, bool selected)
{
    for (qsizetype offset = 0; offset < extra.size();)
    {
        const auto tag = number<quint16>(extra, offset);
        const auto size = number<quint16>(extra, offset + 2);
        // ZIP64 offsets/sizes require a separate writer. For the edited member, accept only
        // content-independent timestamp, Unix identity and Unicode name/comment metadata.
        require(tag != 1 && (!selected || tag == 0x5455 || tag == 0x7875 || tag == 0x000a || tag == 0x7075 || tag == 0x6375));
        offset += 4;
        require(size <= extra.size() - offset);
        offset += size;
    }
}

struct Entry
{
    QByteArray central;
    QByteArray selectedLocal;
    qint64 offset = 0;
    qint64 end = 0;
    bool selected = false;
};

struct Directory
{
    std::vector<Entry> entries;
    QByteArray endRecord;
    qint64 offset = 0;
};

Directory readDirectory(QFile& input, const QString& member, const std::atomic_bool* cancelled)
{
    Directory result;
    const auto tailOffset = std::max(qint64{0}, input.size() - 65557);
    const auto tail = readAt(input, tailOffset, input.size() - tailOffset);
    qsizetype endOffset = -1;
    for (qsizetype i = tail.size() - 22; i >= 0; --i)
    {
        if (number<quint32>(tail, i) == 0x06054b50 && i + 22 + number<quint16>(tail, i + 20) == tail.size())
        {
            require(endOffset < 0);
            endOffset = i;
        }
    }
    require(endOffset >= 0);
    result.endRecord = tail.mid(endOffset);
    const auto& end = result.endRecord;
    const auto count = number<quint16>(end, 10);
    const auto size = number<quint32>(end, 12);
    result.offset = number<quint32>(end, 16);
    require(number<quint16>(end, 4) == 0 && number<quint16>(end, 6) == 0 && number<quint16>(end, 8) == count && count > 0 &&
            count <= 4096 && size <= MAX_DIRECTORY_BYTES && result.offset + size == tailOffset + endOffset);
    const auto directory = readAt(input, result.offset, size);
    result.entries.reserve(count);
    qsizetype position = 0;
    unsigned selectedCount = 0;
    for (unsigned i = 0; i < count; ++i)
    {
        checkCancelled(cancelled);
        auto central = directory.mid(position, 46);
        require(central.size() == 46 && number<quint32>(central, 0) == 0x02014b50 && number<quint16>(central, 34) == 0);
        const auto nameSize = number<quint16>(central, 28);
        const auto extraSize = number<quint16>(central, 30);
        const auto recordSize = qsizetype{46} + nameSize + extraSize + number<quint16>(central, 32);
        require(recordSize <= directory.size() - position);
        central = directory.mid(position, recordSize);
        position += recordSize;
        const auto flags = number<quint16>(central, 8);
        const auto method = number<quint16>(central, 10);
        const auto nameBytes = central.mid(46, nameSize);
        const auto name = QString::fromUtf8(nameBytes);
        require(number<quint16>(central, 6) <= 20 && (flags & ~0x080e) == 0 && (method == 0 || method == 8) &&
                (method != 0 || (flags & 6) == 0) && nameSize > 0 && name.toUtf8() == nameBytes &&
                ((flags & 0x0800) != 0 || std::ranges::all_of(nameBytes, [](char c) { return static_cast<unsigned char>(c) < 128; })));
        const bool selected = name == member;
        selectedCount += selected ? 1u : 0u;
        checkExtra(central.mid(46 + nameSize, extraSize), selected);
        const auto offset = number<quint32>(central, 42);
        auto local = readAt(input, offset, 30);
        require(number<quint32>(local, 0) == 0x04034b50 && number<quint16>(local, 4) <= 20 && number<quint16>(local, 6) == flags &&
                number<quint16>(local, 8) == method && number<quint16>(local, 26) == nameSize);
        const auto localSize = qint64{30} + nameSize + number<quint16>(local, 28);
        local = readAt(input, offset, localSize);
        require(local.mid(30, nameSize) == nameBytes);
        checkExtra(local.mid(30 + nameSize), selected);
        for (const auto [localField, centralField] : {std::pair{14, 16}, std::pair{18, 20}, std::pair{22, 24}})
        {
            const auto value = number<quint32>(local, localField);
            require(value == number<quint32>(central, centralField) || ((flags & 8) != 0 && value == 0));
        }
        const auto compressed = number<quint32>(central, 20);
        const auto uncompressed = number<quint32>(central, 24);
        require(compressed <= MAX_TRACKER_ARCHIVE_BYTES && uncompressed <= qint64{512} * 1024 * 1024 &&
                (method != 0 || compressed == uncompressed));
        auto entryEnd = offset + localSize + compressed;
        if ((flags & 8) != 0)
        {
            const auto descriptor = readAt(input, entryEnd, 16);
            const qsizetype start = number<quint32>(descriptor, 0) == 0x08074b50 ? 4 : 0;
            require(number<quint32>(descriptor, start) == number<quint32>(central, 16) &&
                    number<quint32>(descriptor, start + 4) == compressed && number<quint32>(descriptor, start + 8) == uncompressed);
            entryEnd += start + 12;
        }
        require(entryEnd <= result.offset);
        result.entries.push_back({.central = std::move(central),
                                  .selectedLocal = selected ? std::move(local) : QByteArray{},
                                  .offset = offset,
                                  .end = entryEnd,
                                  .selected = selected});
    }
    require(position == directory.size() && selectedCount == 1);
    std::vector<std::pair<qint64, qint64>> ranges;
    ranges.reserve(result.entries.size());
    for (const auto& entry : result.entries)
        ranges.emplace_back(entry.offset, entry.end);
    std::ranges::sort(ranges);
    qint64 next = 0;
    for (const auto [start, end] : ranges)
    {
        require(start == next && end > start);
        next = end;
    }
    require(next == result.offset);
    return result;
}

quint32 checksum(const std::string& bytes)
{
    static constexpr auto TABLE = []
    {
        std::array<quint32, 256> values{};
        for (quint32 i = 0; i < values.size(); ++i)
        {
            auto value = i;
            for (unsigned bit = 0; bit < 8; ++bit)
                value = (value >> 1) ^ ((value & 1) != 0 ? 0xedb88320u : 0u);
            values[i] = value;
        }
        return values;
    }();
    quint32 crc = 0xffffffff;
    for (const auto byte : bytes)
        crc = (crc >> 8) ^ TABLE[(crc ^ static_cast<unsigned char>(byte)) & 255];
    return crc ^ 0xffffffff;
}

void copyRange(QFile& input, QIODevice& output, qint64 first, qint64 last, const std::atomic_bool* cancelled)
{
    require(first >= 0 && last >= first && last <= input.size() && input.seek(first), "Could not seek Tracker archive input.");
    std::array<char, BLOCK_BYTES> buffer{};
    while (first < last)
    {
        checkCancelled(cancelled);
        const auto size = std::min(BLOCK_BYTES, last - first);
        require(input.read(buffer.data(), size) == size, "Could not read Tracker archive input.");
        require(output.write(buffer.data(), size) == size, "Could not write Tracker archive output.");
        first += size;
    }
}

void writeBytes(QIODevice& output, const QByteArray& bytes)
{
    require(output.write(bytes) == bytes.size(), "Could not write Tracker archive metadata.");
}
} // namespace

void writeTrackerArchive(
    const QString& path, const TrackerArchive& source, const QString& member, const std::string& xml, const std::atomic_bool* cancelled)
{
    checkCancelled(cancelled);
    require(path.endsWith(".trz", Qt::CaseInsensitive) && source.projects().contains(member) && !xml.empty() &&
            xml.size() <= MAX_TRACKER_XML_BYTES);
    static_cast<void>(parseLegacyProject(xml));
    const QFileInfo target(path);
    require(!target.isSymLink(), "Tracker archive output cannot replace a file alias.");
    const auto parent = QFileInfo(target.absolutePath()).canonicalFilePath();
    require(!parent.isEmpty(), "The Tracker archive destination directory must exist.");
    const auto destination = parent + '/' + target.fileName();
    for (const auto& owned : {source.root(), QFileInfo(source.sourcePath()).absolutePath()})
    {
        const auto root = QFileInfo(owned).canonicalFilePath();
        require(!root.isEmpty() && destination != root && !destination.startsWith(root + '/'),
                "Tracker archive output cannot replace owned source or extracted files.");
    }
    QFile input(source.sourcePath());
    require(input.open(QIODevice::ReadOnly), "Could not open the owned Tracker archive source.");
    require(input.size() > 0 && input.size() <= MAX_TRACKER_ARCHIVE_BYTES);
    auto directory = readDirectory(input, member, cancelled);
    const auto selected = std::ranges::find_if(directory.entries, [](const Entry& entry) { return entry.selected; });
    require(selected != directory.entries.end());
    auto local = selected->selectedLocal;
    const auto crc = checksum(xml);
    const auto size = static_cast<quint32>(xml.size());
    setNumber<quint16>(local, 6, number<quint16>(local, 6) & 0x0800);
    setNumber<quint16>(local, 8, 0);
    setNumber<quint32>(local, 14, crc);
    setNumber<quint32>(local, 18, size);
    setNumber<quint32>(local, 22, size);
    const qint64 delta = local.size() + static_cast<qint64>(xml.size()) - (selected->end - selected->offset);
    require(input.size() + delta > 0 && input.size() + delta <= MAX_TRACKER_ARCHIVE_BYTES);
    QTemporaryDir temporary;
    require(temporary.isValid(), "Could not create a temporary Tracker archive directory.");
    const auto stagedPath = temporary.filePath("validated.trz");
    QFile staged(stagedPath);
    require(staged.open(QIODevice::WriteOnly | QIODevice::NewOnly), "Could not create a staged Tracker archive.");
    copyRange(input, staged, 0, selected->offset, cancelled);
    writeBytes(staged, local);
    require(staged.write(xml.data(), static_cast<qint64>(xml.size())) == static_cast<qint64>(xml.size()),
            "Could not write the edited Tracker archive member.");
    copyRange(input, staged, selected->end, directory.offset, cancelled);
    for (auto& entry : directory.entries)
    {
        checkCancelled(cancelled);
        if (entry.offset > selected->offset)
            setNumber<quint32>(entry.central, 42, static_cast<quint32>(entry.offset + delta));
        if (entry.selected)
        {
            setNumber<quint16>(entry.central, 8, number<quint16>(entry.central, 8) & 0x0800);
            setNumber<quint16>(entry.central, 10, 0);
            setNumber<quint32>(entry.central, 16, crc);
            setNumber<quint32>(entry.central, 20, size);
            setNumber<quint32>(entry.central, 24, size);
        }
        writeBytes(staged, entry.central);
    }
    setNumber<quint32>(directory.endRecord, 16, static_cast<quint32>(directory.offset + delta));
    writeBytes(staged, directory.endRecord);
    require(staged.flush() && staged.size() == input.size() + delta, "Could not finish writing the staged Tracker archive.");
    staged.close();
    {
        const auto verified = TrackerArchive::open(stagedPath, cancelled);
        require(verified.projects() == source.projects());
        QFile edited(verified.projectPath(member));
        require(edited.open(QIODevice::ReadOnly) && edited.size() == static_cast<qint64>(xml.size()) &&
                edited.readAll().toStdString() == xml);
    }
    require(staged.open(QIODevice::ReadOnly), "Could not reopen the staged Tracker archive.");
    QSaveFile output(path);
    require(output.open(QIODevice::WriteOnly), "Could not open the Tracker archive destination.");
    copyRange(staged, output, 0, staged.size(), cancelled);
    checkCancelled(cancelled);
    require(output.commit(), "Could not replace the Tracker archive destination.");
}
} // namespace motrack
