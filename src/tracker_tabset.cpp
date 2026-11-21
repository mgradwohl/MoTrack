#include "tracker_tabset.hpp"

#include "legacy_project.hpp"

#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QXmlStreamReader>

#include <algorithm>
#include <format>
#include <ranges>
#include <set>
#include <utility>

namespace motrack
{
namespace
{

constexpr std::string_view STRING_ARRAY_CLASS = "[Ljava.lang.String;";
constexpr std::string_view STRING_MATRIX_CLASS = "[[Ljava.lang.String;";

[[nodiscard]] std::optional<std::size_t> arrayIndex(const QString& name)
{
    // OSP names array elements "[n]"; reject signs, padding and other spellings it would not write.
    if (name.size() < 3 || !name.startsWith('[') || !name.endsWith(']'))
        return std::nullopt;
    const auto digits = QStringView(name).sliced(1, name.size() - 2);
    if (digits.size() > 1 && digits.front() == '0')
        return std::nullopt;
    bool ok = false;
    const auto value = digits.toULongLong(&ok);
    return ok && std::ranges::all_of(digits, [](QChar c) { return c.isDigit(); }) ? std::optional{static_cast<std::size_t>(value)}
                                                                                  : std::nullopt;
}

[[nodiscard]] std::vector<QDomElement> childElements(const QDomElement& parent)
{
    std::vector<QDomElement> result;
    for (auto child = parent.firstChildElement(); !child.isNull(); child = child.nextSiblingElement())
        result.push_back(child);
    return result;
}

[[nodiscard]] std::expected<std::string, std::string> stringValue(const QDomElement& property)
{
    if (property.attribute("type") != "string" || !property.firstChildElement().isNull())
        return std::unexpected("Tabset path entries must be strings.");
    return property.text().toStdString();
}

[[nodiscard]] std::expected<TabsetEntry, std::string> parseEntry(const QDomElement& tab)
{
    if (tab.attribute("type") != "array" || tab.attribute("class") != QLatin1StringView(STRING_ARRAY_CLASS))
        return std::unexpected("Each tabset entry must be a string array.");
    std::optional<std::string> absolute;
    std::optional<std::string> relative;
    for (const auto& element : childElements(tab))
    {
        const auto index = arrayIndex(element.attribute("name"));
        if (element.tagName() != "property" || !index || *index > 1)
            return std::unexpected("A tabset entry has an unexpected element.");
        auto& slot = *index == 0 ? absolute : relative;
        if (slot)
            return std::unexpected("A tabset entry repeats a path.");
        auto text = stringValue(element);
        if (!text)
            return std::unexpected(std::move(text).error());
        slot = std::move(*text);
    }
    // TFrame.Loader dereferences the relative path unconditionally; only the absolute path may be absent.
    if (!relative || relative->empty())
        return std::unexpected("A tabset entry has no relative path.");
    return TabsetEntry{.absolute = std::move(absolute), .relative = std::move(*relative)};
}

[[nodiscard]] QString forwardSlashes(const QString& path)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(path));
}

[[nodiscard]] QStringList components(const QString& path)
{
    auto parts = path.split('/', Qt::KeepEmptyParts);
    // "/a/b" -> {"", "a", "b"}; keep the empty root marker. "C:/a" -> {"C:", "a"}. "//server/share" -> {"", "", ...}.
    while (parts.size() > 1 && parts.back().isEmpty())
        parts.removeLast();
    return parts;
}

[[nodiscard]] bool sameComponent(const QString& first, const QString& second)
{
#ifdef Q_OS_WIN
    return first.compare(second, Qt::CaseInsensitive) == 0;
#else
    return first == second;
#endif
}

/// Number of leading components that only name a filesystem root and cannot by themselves be a common ancestor.
[[nodiscard]] qsizetype rootComponents(const QStringList& parts)
{
    // UNC "//server/share": like a drive, the share is the shallowest usable common ancestor.
    if (parts.size() >= 2 && parts[0].isEmpty() && parts[1].isEmpty())
        return std::min<qsizetype>(parts.size(), 3);
    // OSP's climb stops at an empty POSIX base, but a Windows drive ("C:") remains a usable ancestor.
    return !parts.isEmpty() && parts[0].isEmpty() ? 1 : 0;
}

[[nodiscard]] std::expected<QByteArray, std::string> readBounded(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.isSequential())
        return std::unexpected("Could not open the tabset.");
    const auto length = file.size();
    if (length < 0 || std::cmp_greater(length, MAX_TRACKER_XML_BYTES))
        return std::unexpected("The tabset is too large.");
    auto bytes = file.read(length + 1);
    if (file.error() != QFileDevice::NoError || bytes.size() != length || !file.atEnd())
        return std::unexpected("Could not read the tabset.");
    return bytes;
}

[[nodiscard]] std::optional<QString> existingFile(const QString& candidate)
{
    const QFileInfo info(candidate);
    if (!info.isFile())
        return std::nullopt;
    auto canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? std::nullopt : std::optional{std::move(canonical)};
}

[[nodiscard]] bool isTrackerXmlFile(const QString& path)
{
    return path.endsWith(".trk", Qt::CaseInsensitive);
}

class Expansion
{
  public:
    Expansion(QString workingDirectory, const std::atomic_bool* cancelled)
        : m_WorkingDirectory(std::move(workingDirectory)), m_Cancelled(cancelled)
    {}

    void expand(const QString& tabset, const std::vector<TabsetEntry>& entries, int depth)
    {
        m_Stack.push_back(tabsetIdentityKey(tabset));
        const auto directory = QFileInfo(tabset).path();
        for (const auto& entry : entries)
        {
            checkCancellation();
            if (m_Full)
                break;
            if (m_Items.size() >= MAX_TABSET_ENTRIES)
            {
                // Report the bound once; enclosing tabsets stop at the same point.
                append(entry, tabset, TabsetItemStatus::TooMany);
                m_Full = true;
                break;
            }
            const auto file = resolveTabsetEntry(entry, directory, m_WorkingDirectory);
            if (!file)
            {
                append(entry, tabset, TabsetItemStatus::Missing);
                continue;
            }
            // Canonical keys keep large sets linear; hard-linked aliases may still open separately.
            const auto key = tabsetIdentityKey(*file);
            if (std::ranges::find(m_Stack, key) != m_Stack.end())
            {
                append(entry, tabset, TabsetItemStatus::Cycle);
                continue;
            }
            if (!m_Seen.insert(key).second)
            {
                append(entry, tabset, TabsetItemStatus::Duplicate);
                continue;
            }
            // Only a TFrame root needs a full parse; experiments are parsed once, when they are opened.
            if (!isTrackerXmlFile(*file) || !isTrackerTabsetFile(*file))
            {
                append(entry, tabset, TabsetItemStatus::Resolved, *file);
                continue;
            }
            auto bytes = readBounded(*file);
            if (!bytes)
            {
                append(entry, tabset, TabsetItemStatus::Unreadable, *file, std::move(bytes).error());
                continue;
            }
            const std::string_view xml(bytes->constData(), static_cast<std::size_t>(bytes->size()));
            if (depth + 1 >= MAX_TABSET_DEPTH)
            {
                append(entry, tabset, TabsetItemStatus::TooDeep, *file);
                continue;
            }
            auto nested = parseTrackerTabset(xml);
            if (!nested)
            {
                append(entry, tabset, TabsetItemStatus::Unreadable, *file, std::move(nested).error());
                continue;
            }
            expand(*file, *nested, depth + 1);
        }
        m_Stack.pop_back();
    }

    void markSeen(const QString& file)
    {
        m_Seen.insert(tabsetIdentityKey(file));
    }

    [[nodiscard]] std::vector<TabsetItem> take() &&
    {
        return std::move(m_Items);
    }

  private:
    void checkCancellation() const
    {
        if (m_Cancelled && m_Cancelled->load())
            throw ProjectError("Workspace opening canceled.");
    }

    void append(const TabsetEntry& entry, const QString& tabset, TabsetItemStatus status, QString file = {}, std::string diagnostic = {})
    {
        m_Items.push_back(
            {.entry = entry, .tabset = tabset, .file = std::move(file), .status = status, .diagnostic = std::move(diagnostic)});
    }

    QString m_WorkingDirectory;
    const std::atomic_bool* m_Cancelled;
    std::vector<QString> m_Stack;
    std::set<QString> m_Seen;
    std::vector<TabsetItem> m_Items;
    bool m_Full = false;
};

void appendString(std::string& xml, std::string_view indent, std::size_t index, std::string_view value)
{
    // Match XML.requiresCDATA: OSP keeps entity references literally in ordinary strings.
    const auto special = std::ranges::any_of(value, [](char c) { return c == '"' || c == '<' || c == '>' || c == '&' || c == '\''; });
    xml += std::format("{}<property name=\"[{}]\" type=\"string\">{}{}{}</property>\n",
                       indent,
                       index,
                       special ? "<![CDATA[" : "",
                       value,
                       special ? "]]>" : "");
}

} // namespace

QString tabsetIdentityKey(const QString& canonical)
{
    // NTFS compares names case-insensitively by default; macOS volumes may be case-sensitive, where folding would
    // silently drop a distinct file, so only Windows folds.
#ifdef Q_OS_WIN
    return canonical.toCaseFolded();
#else
    return canonical;
#endif
}

bool isTrackerTabset(std::string_view xml)
{
    if (xml.size() > MAX_TRACKER_XML_BYTES)
        return false;
    QDomDocument document;
    if (!document.setContent(QByteArray::fromRawData(xml.data(), static_cast<qsizetype>(xml.size()))))
        return false;
    const auto root = document.documentElement();
    return root.tagName() == "object" && root.attribute("class") == QLatin1StringView(TRACKER_TABSET_CLASS);
}

bool isTrackerTabsetFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.isSequential())
        return false;
    // The root element decides; stop there instead of parsing an experiment of up to 32 MiB.
    constexpr qint64 MAX_PROLOG_BYTES = qint64{64} * 1024;
    QXmlStreamReader reader(&file);
    while (!reader.atEnd() && !reader.hasError() && reader.characterOffset() <= MAX_PROLOG_BYTES)
        if (reader.readNext() == QXmlStreamReader::StartElement)
            return reader.name() == u"object" && reader.attributes().value("class") == QLatin1StringView(TRACKER_TABSET_CLASS);
    return false;
}

std::expected<std::vector<TabsetEntry>, std::string> parseTrackerTabset(std::string_view xml)
{
    if (xml.size() > MAX_TRACKER_XML_BYTES)
        return std::unexpected("The tabset is too large.");
    QDomDocument document;
    if (!document.setContent(QByteArray::fromRawData(xml.data(), static_cast<qsizetype>(xml.size()))))
        return std::unexpected("Invalid Tracker XML.");
    if (!document.doctype().name().isEmpty())
        return std::unexpected("Tracker XML must not contain a document type.");
    const auto root = document.documentElement();
    if (root.tagName() != "object" || root.attribute("class") != QLatin1StringView(TRACKER_TABSET_CLASS))
        return std::unexpected("Expected a Tracker tabset.");
    std::optional<QDomElement> tabs;
    for (const auto& property : childElements(root))
    {
        if (property.tagName() != "property" || property.attribute("name") != "tabs")
            continue; // TFrame.Loader reads only "tabs"; unknown properties are ignored as in Tracker.
        if (tabs)
            return std::unexpected("The tabset repeats its tabs property.");
        tabs = property;
    }
    if (!tabs)
        return std::vector<TabsetEntry>{};
    if (tabs->attribute("type") != "array" || tabs->attribute("class") != QLatin1StringView(STRING_MATRIX_CLASS))
        return std::unexpected("The tabset's tabs property must be a string matrix.");
    std::vector<std::pair<std::size_t, TabsetEntry>> indexed;
    for (const auto& element : childElements(*tabs))
    {
        const auto index = arrayIndex(element.attribute("name"));
        if (element.tagName() != "property" || !index)
            return std::unexpected("The tabset's tabs property has an unexpected element.");
        if (indexed.size() >= MAX_TABSET_ENTRIES)
            return std::unexpected("The tabset lists too many files.");
        auto entry = parseEntry(element);
        if (!entry)
            return std::unexpected(std::move(entry).error());
        indexed.emplace_back(*index, std::move(*entry));
    }
    std::ranges::sort(indexed, {}, &std::pair<std::size_t, TabsetEntry>::first);
    if (std::ranges::adjacent_find(indexed, {}, &std::pair<std::size_t, TabsetEntry>::first) != indexed.end())
        return std::unexpected("The tabset repeats an entry index.");
    // A null String[] element is simply absent from OSP output; Tracker would fail on it, so it is not skipped here.
    if (!indexed.empty() && indexed.back().first + 1 != indexed.size())
        return std::unexpected("The tabset has a missing entry.");
    auto entries = indexed | std::views::values;
    return std::vector<TabsetEntry>(entries.begin(), entries.end());
}

std::expected<std::string, std::string> writeTrackerTabset(std::span<const TabsetEntry> entries)
{
    if (entries.size() > MAX_TABSET_ENTRIES)
        return std::unexpected("The tabset lists too many files.");
    std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    xml += std::format("<object class=\"{}\">\n", TRACKER_TABSET_CLASS);
    xml += std::format("    <property name=\"tabs\" type=\"array\" class=\"{}\">\n", STRING_MATRIX_CLASS);
    const auto unrepresentable = [](std::string_view value)
    {
        return value.contains("]]>") || value.contains('\0');
    };
    // std::views::enumerate is unavailable in the libc++ releases used by Apple Clang and Clang 18.
    for (std::size_t index = 0; index < entries.size(); ++index)
    {
        const auto& entry = entries[index];
        if (entry.relative.empty() || unrepresentable(entry.relative) || (entry.absolute && unrepresentable(*entry.absolute)))
            return std::unexpected("A tabset path cannot be written in Tracker XML.");
        xml += std::format("        <property name=\"[{}]\" type=\"array\" class=\"{}\">\n", index, STRING_ARRAY_CLASS);
        if (entry.absolute)
            appendString(xml, "            ", 0, *entry.absolute);
        appendString(xml, "            ", 1, entry.relative);
        xml += "        </property>\n";
    }
    xml += "    </property>\n</object>\n";
    return xml;
}

QString tabsetRelativePath(const QString& file, const QString& baseDirectory)
{
    auto absolute = forwardSlashes(file);
    const auto base = forwardSlashes(baseDirectory);
    if (QDir::isRelativePath(absolute) || QDir::isRelativePath(base))
        return absolute;
    const auto target = components(absolute);
    const auto from = components(base);
    qsizetype common = 0;
    while (common < from.size() && common + 1 < target.size() && sameComponent(from[common], target[common]))
        ++common;
    const auto climbs = from.size() - common;
    if (common <= rootComponents(from) || climbs > MAX_TABSET_PARENT_LEVELS)
        return absolute;
    QStringList relative;
    relative.reserve(climbs + target.size() - common);
    for (qsizetype level = 0; level < climbs; ++level)
        relative.append("..");
    relative.append(target.sliced(common));
    return relative.join('/');
}

TabsetEntry tabsetEntry(const QString& file, const QString& tabsetDirectory)
{
    const auto absolute = forwardSlashes(file);
    return {.absolute = absolute.toStdString(), .relative = tabsetRelativePath(absolute, tabsetDirectory).toStdString()};
}

std::optional<QString> resolveTabsetEntry(const TabsetEntry& entry, const QString& tabsetDirectory, const QString& workingDirectory)
{
    const auto relative = QString::fromStdString(entry.relative);
    if (!relative.isEmpty() && QDir::isRelativePath(QDir::fromNativeSeparators(relative)))
    {
        // java.io.File(parent, child) joins even an absolute child, so only relative slots use these bases.
        for (const auto& base : {tabsetDirectory, workingDirectory})
            if (!base.isEmpty())
                if (auto found = existingFile(QDir(base).filePath(relative)))
                    return found;
    }
    return entry.absolute.and_then([](const std::string& absolute) { return existingFile(QString::fromStdString(absolute)); });
}

std::vector<TabsetItem> expandTrackerTabset(const QString& path, const QString& workingDirectory, const std::atomic_bool* cancelled)
{
    if (cancelled && cancelled->load())
        throw ProjectError("Workspace opening canceled.");
    const auto physical = existingFile(path);
    if (!physical)
        throw ProjectError("The tabset file is unavailable.");
    auto bytes = readBounded(*physical);
    if (!bytes)
        throw ProjectError(bytes.error());
    auto entries = parseTrackerTabset(std::string_view(bytes->constData(), static_cast<std::size_t>(bytes->size())));
    if (!entries)
        throw ProjectError(entries.error());
    Expansion expansion(workingDirectory, cancelled);
    expansion.markSeen(*physical);
    expansion.expand(*physical, *entries, 0);
    return std::move(expansion).take();
}

std::expected<void, std::string> saveTrackerTabset(const QString& path, std::span<const TabsetEntry> entries)
{
    auto xml = writeTrackerTabset(entries);
    if (!xml)
        return std::unexpected(std::move(xml).error());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return std::unexpected("Could not create the tabset.");
    if (file.write(xml->data(), static_cast<qint64>(xml->size())) != static_cast<qint64>(xml->size()) || !file.commit())
        return std::unexpected("Could not write the tabset.");
    return {};
}

} // namespace motrack
