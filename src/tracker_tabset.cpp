#include "tracker_tabset.hpp"

#include "legacy_project.hpp"
#include "local_media_path.hpp"
#include "model/source_edits.hpp"
#include "model/tracker_loaders.hpp"
#include "osp/render.hpp"
#include "osp/value.hpp"
#include "tracker_archive.hpp"
#include "tracker_import.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSet>

#include <algorithm>
#include <iterator>
#include <map>
#include <ranges>
#include <set>
#include <utility>

namespace motrack
{
static_assert(TRACKER_TABSET_CLASS == tracker::TFRAME_CLASS);

namespace
{

/// The bytes OSP's readInput examines for the root: the first ten lines, which in a Tracker file are short.
constexpr qint64 MAX_ROOT_PROBE_BYTES = qint64{64} * 1024;
constexpr int ROOT_SEARCH_LINES = 10;

/// XMLControlElement.readInput (OSP line 1581): the root is the first of the first ten lines containing
/// `<object class=`; getClassName takes the text after `class="` up to the next quote, or "" without one.
[[nodiscard]] std::optional<std::string> rootClass(std::string_view head)
{
    std::size_t position = 0;
    for (int line = 0; line < ROOT_SEARCH_LINES && position <= head.size(); ++line)
    {
        // BufferedReader.readLine ends a line at "\n", "\r\n" or a lone "\r".
        const auto end = head.find_first_of("\r\n", position);
        const auto text = head.substr(position, end == std::string_view::npos ? std::string_view::npos : end - position);
        if (text.contains("<object class="))
        {
            constexpr std::string_view ATTRIBUTE = "class=\"";
            const auto start = text.find(ATTRIBUTE);
            const auto close = start == std::string_view::npos ? start : text.find('"', start + ATTRIBUTE.size());
            if (close == std::string_view::npos)
                return std::string{};
            return osp::effectiveClassName(text.substr(start + ATTRIBUTE.size(), close - start - ATTRIBUTE.size()));
        }
        if (end == std::string_view::npos)
            break;
        position = end + (head.substr(end, 2) == "\r\n" ? 2 : 1);
    }
    return std::nullopt;
}

/// OSP names array elements "[n]" (read with Integer.parseInt); refuse signs, padding and other spellings it
/// would not write, as MoTrack's import policy.
[[nodiscard]] std::optional<std::size_t> strictIndex(const osp::Source& source, const tracker::ArrayElement& element)
{
    const auto& name = source.node(element.property).name;
    if (!name || name->size() < 3 || !name->starts_with('[') || !name->ends_with(']') || !element.index)
        return std::nullopt;
    const auto digits = std::string_view(*name).substr(1, name->size() - 2);
    if ((digits.size() > 1 && digits.front() == '0') || !std::ranges::all_of(digits, [](char c) { return c >= '0' && c <= '9'; }))
        return std::nullopt;
    return static_cast<std::size_t>(*element.index);
}

struct ParsedEntry
{
    TabsetEntry entry;
    TrackerTabset::EntryNodes nodes;
};

[[nodiscard]] std::expected<ParsedEntry, std::string> parseEntry(const osp::Source& source, const tracker::TabsetTabLoad& tab)
{
    if (tab.type != osp::PropertyType::Array || tab.className != tracker::STRING_ARRAY_CLASS)
        return std::unexpected("Each tabset entry must be a string array.");
    std::optional<std::string> absolute;
    std::optional<std::string> relative;
    std::optional<osp::NodeId> relativeNode;
    TrackerTabset::EntryNodes nodes{.element = tab.element.property};
    std::optional<std::size_t> previous;
    for (const auto& path : tab.paths)
    {
        const auto index = strictIndex(source, path.element);
        if (!index || *index > 1)
            return std::unexpected("A tabset entry has an unexpected element.");
        auto& slot = *index == 0 ? absolute : relative;
        if (slot)
            return std::unexpected("A tabset entry repeats a path.");
        if (source.node(path.element.property).type != osp::PropertyType::String || !path.value)
            return std::unexpected("Tabset path entries must be strings.");
        slot = *path.value;
        (*index == 0 ? nodes.absolute : relativeNode) = path.element.property;
        nodes.ordered = nodes.ordered && (!previous || *previous < *index);
        previous = index;
    }
    // TFrame.Loader dereferences the relative path unconditionally; only the absolute path may be absent.
    if (!relative || relative->empty() || !relativeNode)
        return std::unexpected("A tabset entry has no relative path.");
    nodes.relative = *relativeNode;
    return ParsedEntry{.entry = {.absolute = std::move(absolute), .relative = std::move(*relative)}, .nodes = nodes};
}

[[nodiscard]] bool representable(std::string_view path)
{
    // OSP would truncate a CDATA value at "]]>" when reading it back, and it joins multi-line values with the
    // platform's line separator, so neither survives a Tracker round trip.
    return !path.empty() && !path.contains("]]>") && !path.contains('\0') && !path.contains('\n') && !path.contains('\r');
}

/// Entries read from the file being edited (`known`) are kept as they were read, so only new ones are checked.
[[nodiscard]] std::expected<void, std::string> validEntries(std::span<const TabsetEntry> entries, std::span<const TabsetEntry> known = {})
{
    if (entries.size() > MAX_TABSET_ENTRIES)
        return std::unexpected("The tabset lists too many files.");
    if (!std::ranges::all_of(entries,
                             [&](const TabsetEntry& entry)
                             {
                                 return std::ranges::contains(known, entry) ||
                                        (representable(entry.relative) && (!entry.absolute || representable(*entry.absolute)));
                             }))
        return std::unexpected("A tabset path cannot be written in Tracker XML.");
    return {};
}

/// XMLPropertyElement's writer for one String[] entry; `[0]` is skipped when absent.
[[nodiscard]] osp::PropertyValue entryProperty(std::size_t index, const TabsetEntry& entry)
{
    std::vector<std::optional<osp::PropertyValue>> paths;
    paths.emplace_back(entry.absolute.transform([](const std::string& path) { return osp::stringProperty("[0]", path); }));
    paths.emplace_back(osp::stringProperty("[1]", entry.relative));
    return osp::arrayProperty("[" + std::to_string(index) + "]", std::string(tracker::STRING_ARRAY_CLASS), paths);
}

[[nodiscard]] osp::PropertyValue tabsProperty(std::span<const TabsetEntry> entries)
{
    std::vector<std::optional<osp::PropertyValue>> elements;
    elements.reserve(entries.size());
    // std::views::enumerate is unavailable in the libc++ release used by Apple Clang.
    for (std::size_t index = 0; index < entries.size(); ++index)
        elements.emplace_back(entryProperty(index, entries[index]));
    return osp::arrayProperty("tabs", std::string(tracker::STRING_MATRIX_CLASS), elements);
}

[[nodiscard]] std::size_t childPosition(const osp::Source& source, osp::NodeId parent, osp::NodeId child)
{
    const auto& children = source.node(parent).children;
    return static_cast<std::size_t>(std::ranges::find(children, child) - children.begin());
}

/// Edits one existing entry in place: only the paths that differ change.
void editEntry(SourceEdits& edits, const TrackerTabset::EntryNodes& nodes, const TabsetEntry& before, const TabsetEntry& after)
{
    if (after.relative != before.relative)
        edits.setString(nodes.relative, after.relative);
    if (after.absolute == before.absolute)
        return;
    if (!after.absolute)
    {
        if (nodes.absolute) // Present, since before.absolute differs.
            edits.remove(*nodes.absolute);
    }
    else if (nodes.absolute)
        edits.setString(*nodes.absolute, *after.absolute);
    else
        edits.insert(nodes.element, 0, osp::stringProperty("[0]", *after.absolute));
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

[[nodiscard]] std::expected<std::string, std::string> readBounded(const QString& path)
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
    return bytes.toStdString();
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

/// The `.trk` member names of each archive one operation looks at, read once per archive.
class ArchiveMembers
{
  public:
    [[nodiscard]] bool contains(const QString& archive, const QString& member)
    {
        auto found = m_Names.find(archive);
        if (found == m_Names.end())
            found = m_Names.emplace(archive, trackerArchiveProjectNames(archive)).first;
        return found->second && found->second->contains(archiveMemberKey(member));
    }

  private:
    std::map<QString, std::optional<QSet<QString>>> m_Names;
};

/// The existing target that `slot` names, joined to `base` unless that is empty. ResourceLoader never reads a path
/// with an archive marker as a plain file, so a slot naming an archive member names that member or nothing.
[[nodiscard]] std::optional<TabsetTarget> existingTarget(const QString& slot, const QString& base, ArchiveMembers& members)
{
    const auto joined = [&](const QString& path)
    {
        return base.isEmpty() ? path : QDir(base).filePath(path);
    };
    if (const auto named = tabsetArchiveMember(slot.toStdString()))
    {
        const auto archive = existingFile(joined(QString::fromStdString(named->archive)));
        auto member = QString::fromStdString(named->member);
        if (!archive || !members.contains(*archive, member))
            return std::nullopt;
        return TabsetTarget{.file = *archive, .member = std::move(member)};
    }
    return existingFile(joined(slot)).transform([](QString file) { return TabsetTarget{.file = std::move(file)}; });
}

[[nodiscard]] std::optional<TabsetTarget>
resolveTarget(const TabsetEntry& entry, const QString& tabsetDirectory, const QString& workingDirectory, ArchiveMembers& members)
{
    const auto relative = QString::fromStdString(entry.relative);
    if (!relative.isEmpty() && QDir::isRelativePath(QDir::fromNativeSeparators(relative)))
    {
        // java.io.File(parent, child) joins even an absolute child, so only relative slots use these bases.
        for (const auto& base : {tabsetDirectory, workingDirectory})
            if (!base.isEmpty())
                if (auto found = existingTarget(relative, base, members))
                    return found;
    }
    return entry.absolute.and_then([&](const std::string& absolute)
                                   { return existingTarget(QString::fromStdString(absolute), {}, members); });
}

class Expansion
{
  public:
    Expansion(QString workingDirectory, const std::atomic_bool* cancelled)
        : m_WorkingDirectory(std::move(workingDirectory)), m_Cancelled(cancelled)
    {}

    void expand(const QString& tabsetPath, const TrackerTabset& tabset, int depth)
    {
        m_Stack.push_back(tabsetIdentityKey(tabsetPath));
        const auto base = tabsetBase(tabset, tabsetPath);
        for (const auto& entry : tabset.entries)
        {
            checkCancellation();
            if (m_Full)
                break;
            if (m_Items.size() >= MAX_TABSET_ENTRIES)
            {
                // Report the bound once; enclosing tabsets stop at the same point.
                append(entry, tabsetPath, TabsetItemStatus::TooMany);
                m_Full = true;
                break;
            }
            const auto target = resolveTarget(entry, base, m_WorkingDirectory, m_Members);
            if (!target)
            {
                append(entry, tabsetPath, TabsetItemStatus::Missing);
                continue;
            }
            const auto* file = &target->file;
            // Canonical keys keep large sets linear; hard-linked aliases may still open separately.
            const auto key = tabsetIdentityKey(*file);
            if (!target->member.isEmpty())
            {
                // A member opens once, and not after its whole archive, which shows every member.
                if (m_Seen.contains(key) || !m_Seen.insert(tabsetTargetKey(*file, target->member)).second)
                    append(entry, tabsetPath, TabsetItemStatus::Duplicate);
                else
                    append(entry, tabsetPath, TabsetItemStatus::Resolved, *file, {}, target->member);
                continue;
            }
            if (std::ranges::find(m_Stack, key) != m_Stack.end())
            {
                append(entry, tabsetPath, TabsetItemStatus::Cycle);
                continue;
            }
            if (!m_Seen.insert(key).second)
            {
                append(entry, tabsetPath, TabsetItemStatus::Duplicate);
                continue;
            }
            // Only a TFrame root needs a full parse; experiments are parsed once, when they are opened.
            if (!isTrackerXmlFile(*file) || !isTrackerTabsetFile(*file))
            {
                append(entry, tabsetPath, TabsetItemStatus::Resolved, *file);
                continue;
            }
            if (depth + 1 >= MAX_TABSET_DEPTH)
            {
                append(entry, tabsetPath, TabsetItemStatus::TooDeep, *file);
                continue;
            }
            auto nested = readTrackerTabset(*file);
            if (!nested)
            {
                append(entry, tabsetPath, TabsetItemStatus::Unreadable, *file, std::move(nested).error());
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

    void append(const TabsetEntry& entry,
                const QString& tabset,
                TabsetItemStatus status,
                QString file = {},
                std::string diagnostic = {},
                QString member = {})
    {
        m_Items.push_back({.entry = entry,
                           .tabset = tabset,
                           .file = std::move(file),
                           .member = std::move(member),
                           .status = status,
                           .diagnostic = std::move(diagnostic)});
    }

    QString m_WorkingDirectory;
    const std::atomic_bool* m_Cancelled;
    ArchiveMembers m_Members;
    std::vector<QString> m_Stack;
    std::set<QString> m_Seen;
    std::vector<TabsetItem> m_Items;
    bool m_Full = false;
};

} // namespace

QString tabsetNameKey(const QString& canonical)
{
    // NTFS compares names case-insensitively by default; macOS volumes may be case-sensitive, where folding would
    // silently drop a distinct file, so only Windows folds.
#ifdef Q_OS_WIN
    return canonical.toCaseFolded();
#else
    return canonical;
#endif
}

TabsetFileKeys tabsetFileKeys(const QString& canonical)
{
    // The file system's own identity, as sameLocalFile compares files: case variants, hard links and short names
    // of one file agree on every volume. A missing file keys by its name.
    return {.name = tabsetNameKey(canonical), .identity = localFileIdentity(canonical)};
}

QString tabsetIdentityKey(const QString& canonical)
{
    return tabsetFileKeys(canonical).key();
}

bool isTrackerTabset(std::string_view xml)
{
    return rootClass(xml.substr(0, static_cast<std::size_t>(MAX_ROOT_PROBE_BYTES))) == TRACKER_TABSET_CLASS;
}

bool isTrackerTabsetFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.isSequential())
        return false;
    // The root line decides; stop there instead of reading an experiment of up to 32 MiB.
    const auto head = file.read(MAX_ROOT_PROBE_BYTES);
    return isTrackerTabset(std::string_view(head.constData(), static_cast<std::size_t>(head.size())));
}

std::expected<TrackerTabset, std::string> parseTrackerTabset(std::string_view xml)
{
    if (xml.size() > MAX_TRACKER_XML_BYTES)
        return std::unexpected("The tabset is too large.");
    std::shared_ptr<const osp::Source> source;
    try
    {
        // Tracker import's reader and diagnostics (line numbers, the Windows-1252 hint).
        source = detail::parseTrackerXml(xml);
    }
    catch (const ProjectError& error)
    {
        return std::unexpected(error.what());
    }
    const auto load = tracker::loadTabset(*source);
    if (load.className != tracker::TFRAME_CLASS)
        return std::unexpected("Expected a Tracker tabset.");
    // Tracker reads the last of repeated properties; which one a writer meant is unknown, so MoTrack refuses.
    if (load.tabsProperties > 1)
        return std::unexpected("The tabset repeats its tabs property.");
    TrackerTabset result{.source = source,
                         .entries = {},
                         .basepath = load.basepath ? std::optional{*load.basepath} : std::nullopt,
                         .tabs = load.tabs,
                         .nodes = {},
                         .ordered = true};
    if (!load.tabs)
        return result; // TFrame.Loader reads only "tabs"; unknown properties are ignored as in Tracker.
    if (const auto& tabs = source->node(*load.tabs);
        tabs.type != osp::PropertyType::Array || tabs.className != tracker::STRING_MATRIX_CLASS)
        return std::unexpected("The tabset's tabs property must be a string matrix.");
    if (load.tabElements.size() > MAX_TABSET_ENTRIES)
        return std::unexpected("The tabset lists too many files.");
    std::vector<std::pair<std::size_t, ParsedEntry>> indexed;
    for (const auto& tab : load.tabElements)
    {
        const auto index = strictIndex(*source, tab.element);
        if (!index)
            return std::unexpected("The tabset's tabs property has an unexpected element.");
        auto entry = parseEntry(*source, tab);
        if (!entry)
            return std::unexpected(std::move(entry).error());
        result.ordered = result.ordered && (indexed.empty() || indexed.back().first < *index);
        indexed.emplace_back(*index, std::move(*entry));
    }
    // Deliberately lenient: OSP sizes an array from its last element, so Tracker cannot load out-of-order indices,
    // but MoTrack accepts any unique dense order. Edited saves write changed entries in order (renderTrackerTabset).
    std::ranges::stable_sort(indexed, {}, &std::pair<std::size_t, ParsedEntry>::first);
    if (std::ranges::adjacent_find(indexed, {}, &std::pair<std::size_t, ParsedEntry>::first) != indexed.end())
        return std::unexpected("The tabset repeats an entry index.");
    // A null String[] element is simply absent from OSP output; Tracker would fail on it, so it is not skipped here.
    if (!indexed.empty() && indexed.back().first + 1 != indexed.size())
        return std::unexpected("The tabset has a missing entry.");
    for (auto& [index, parsed] : indexed)
    {
        result.entries.push_back(std::move(parsed.entry));
        result.nodes.push_back(parsed.nodes);
    }
    return result;
}

std::expected<TrackerTabset, std::string> readTrackerTabset(const QString& path)
{
    auto bytes = readBounded(path);
    if (!bytes)
        return std::unexpected(std::move(bytes).error());
    return parseTrackerTabset(*bytes);
}

std::expected<std::string, std::string> writeTrackerTabset(std::span<const TabsetEntry> entries)
{
    if (auto valid = validEntries(entries); !valid)
        return std::unexpected(std::move(valid).error());
    const osp::ObjectValue root{
        .className = std::string(TRACKER_TABSET_CLASS), .version = std::nullopt, .properties = {tabsProperty(entries)}};
    auto written = osp::write(root);
    if (!written)
        return std::unexpected("Could not write the tabset: " + written.error().message);
    return std::move(*written);
}

std::expected<std::string, std::string> renderTrackerTabset(const TrackerTabset& original, std::span<const TabsetEntry> entries)
{
    if (!original.source)
        return std::unexpected("The tabset has no source.");
    if (std::ranges::equal(entries, original.entries))
        return std::string(original.source->bytes()); // Nothing to edit: the file stays byte for byte.
    if (auto valid = validEntries(entries, original.entries); !valid)
        return std::unexpected(std::move(valid).error());
    SourceEdits edits(original.source);
    const auto& source = *original.source;
    const auto root = source.root();
    if (!original.tabs)
    {
        edits.append(root, tabsProperty(entries));
    }
    else if (!original.ordered)
    {
        // Tracker cannot read these indices; write the whole array in order where it was.
        edits.remove(*original.tabs);
        edits.insert(root, childPosition(source, root, *original.tabs), tabsProperty(entries));
    }
    else
    {
        const auto tabs = *original.tabs;
        const auto kept = std::min(entries.size(), original.entries.size());
        for (std::size_t index = 0; index < kept; ++index)
        {
            const auto& nodes = original.nodes[index];
            // An entry with [1] before [0] makes Tracker refuse the whole tabset, so an edited save rewrites it even
            // when its paths are unchanged.
            if (nodes.ordered && entries[index] == original.entries[index])
                continue;
            if (nodes.ordered)
            {
                editEntry(edits, nodes, original.entries[index], entries[index]);
                continue;
            }
            edits.remove(nodes.element);
            edits.insert(tabs, childPosition(source, tabs, nodes.element), entryProperty(index, entries[index]));
        }
        for (auto index = kept; index < original.entries.size(); ++index)
            edits.remove(original.nodes[index].element);
        for (auto index = kept; index < entries.size(); ++index)
            edits.append(tabs, entryProperty(index, entries[index]));
    }
    auto rendered = edits.render();
    if (!rendered)
        return std::unexpected("Could not write the tabset: " + rendered.error());
    return std::move(rendered->xml);
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

std::optional<TabsetArchiveMember> tabsetArchiveMember(std::string_view path)
{
    // ResourceLoader.isZipEntry: the first "!/" after an archive extension. Tracker writes forward slashes.
    constexpr std::string_view MARKER = "!/";
    constexpr std::string_view EXTENSION = ".trz";
    const auto marker = path.find(MARKER);
    if (marker == std::string_view::npos || marker < EXTENSION.size() || marker + MARKER.size() == path.size())
        return std::nullopt;
    const auto archive = path.substr(0, marker);
    const auto lower = [](char character)
    {
        return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
    };
    if (!std::ranges::equal(archive.substr(archive.size() - EXTENSION.size()), EXTENSION, {}, lower))
        return std::nullopt;
    return TabsetArchiveMember{.archive = std::string(archive), .member = std::string(path.substr(marker + MARKER.size()))};
}

TabsetEntry tabsetMemberEntry(const QString& archive, const QString& member, const QString& tabsetDirectory)
{
    auto entry = tabsetEntry(archive, tabsetDirectory);
    const auto suffix = "!/" + member.toStdString();
    entry.relative += suffix;
    entry.absolute = entry.absolute.value_or(std::string{}) + suffix;
    return entry;
}

std::optional<TabsetTarget> resolveTabsetTarget(const TabsetEntry& entry, const QString& tabsetDirectory, const QString& workingDirectory)
{
    ArchiveMembers members;
    return resolveTarget(entry, tabsetDirectory, workingDirectory, members);
}

std::optional<QString> resolveTabsetEntry(const TabsetEntry& entry, const QString& tabsetDirectory, const QString& workingDirectory)
{
    return resolveTabsetTarget(entry, tabsetDirectory, workingDirectory)
        .transform([](TabsetTarget&& target) { return std::move(target.file); });
}

QString tabsetTargetKey(const QString& canonical, const QString& member)
{
    const auto key = tabsetIdentityKey(canonical);
    return member.isEmpty() ? key : key + "!/" + archiveMemberKey(member);
}

QString tabsetBase(const TrackerTabset& tabset, const QString& tabsetPath)
{
    return tabset.basepath ? QString::fromStdString(*tabset.basepath) : QFileInfo(tabsetPath).absolutePath();
}

std::vector<TabsetItem> expandTrackerTabset(const QString& path, const QString& workingDirectory, const std::atomic_bool* cancelled)
{
    if (cancelled && cancelled->load())
        throw ProjectError("Workspace opening canceled.");
    const auto physical = existingFile(path);
    if (!physical)
        throw ProjectError("The tabset file is unavailable.");
    auto tabset = readTrackerTabset(*physical);
    if (!tabset)
        throw ProjectError(tabset.error());
    Expansion expansion(workingDirectory, cancelled);
    expansion.markSeen(*physical);
    expansion.expand(*physical, *tabset, 0);
    return std::move(expansion).take();
}

std::vector<TabsetEntry> tabsetEntries(std::span<const QString> files,
                                       const QString& tabsetPath,
                                       const QString& workingDirectory,
                                       const TrackerTabset* existing,
                                       std::span<const TabsetFileKeys> closed,
                                       TabsetChanges* changes)
{
    const auto base = existing ? tabsetBase(*existing, tabsetPath) : QFileInfo(tabsetPath).absolutePath();
    QSet<QString> listed;
    for (const auto& file : files)
        listed.insert(tabsetIdentityKey(file));
    ArchiveMembers members;
    // The file each existing entry names now, in Tracker's search order, and which listed files a nested tabset that
    // is kept opens: those stay represented by the nested tabset's entry instead of getting entries of their own.
    std::vector<QString> named;
    // The canonical file each entry names now (the archive for a member), and the member; empty when there is none.
    std::vector<QString> targets;
    std::vector<QString> memberNames;
    // Whether an entry names a file the user closed: one that has both its name and its identity.
    std::vector<bool> dropped;
    std::vector<bool> nested;
    QSet<QString> throughNested;
    // Listed archives that entries naming their members stand for.
    QSet<QString> throughMembers;
    // An entry naming the tabset itself is a cycle, not a nested tabset.
    const auto self = existingFile(tabsetPath).transform(tabsetIdentityKey).value_or(QString{});
    if (existing)
        for (const auto& entry : existing->entries)
        {
            const auto target = resolveTarget(entry, base, workingDirectory, members);
            const auto keys = target.transform([](const TabsetTarget& found) { return tabsetFileKeys(found.file); });
            named.push_back(keys.transform(&TabsetFileKeys::key).value_or(QString{}));
            const bool member = target && !target->member.isEmpty();
            targets.push_back(target ? target->file : QString{});
            memberNames.push_back(member ? target->member : QString{});
            dropped.push_back(keys && std::ranges::contains(closed, *keys));
            nested.push_back(false);
            if (member && !dropped.back() && listed.contains(named.back()))
                throughMembers.insert(named.back());
            if (!target || member || named.back() == self || !isTrackerXmlFile(target->file) || listed.contains(named.back()) ||
                dropped.back() || !isTrackerTabsetFile(target->file))
                continue;
            try
            {
                for (const auto& item : expandTrackerTabset(target->file, workingDirectory))
                    if (item.status == TabsetItemStatus::Resolved)
                    {
                        throughNested.insert(tabsetIdentityKey(item.file));
                        nested.back() = true;
                    }
            }
            catch (const ProjectError&)
            {
                // An unreadable nested tabset opened nothing; its entry is kept like any other unopened file.
                nested.back() = false;
            }
        }
    std::vector<bool> used(named.size(), false);
    const auto isMember = [&](std::size_t index)
    {
        return !memberNames[index].isEmpty();
    };
    // Entries whose file is not listed keep their place unless the user closed that file: a file that failed to open,
    // was not found, or was left unopened by a canceled open stays in the tabset, as an unchanged file would. Every
    // entry of a listed archive that member entries stand for keeps its place too: each member entry is a tab of its
    // own, not a repeat, and the archive's tabs need not be adjacent.
    const auto inPlace = [&](std::size_t index)
    {
        return throughMembers.contains(named[index]);
    };
    const auto retained = [&](std::size_t index)
    {
        return (isMember(index) || inPlace(index) || !listed.contains(named[index])) && !dropped[index];
    };
    // An entry stays as written while its relative path still finds what it names from the base, or when a new entry
    // would be the same; otherwise (say after the tabset moved alone) it is rewritten, so Tracker finds the file again.
    // A member entry is rewritten still naming its member.
    const auto current = [&](std::size_t index, const QString& file, const QString& key)
    {
        const auto& entry = existing->entries[index];
        const auto relative = QString::fromStdString(entry.relative);
        const auto& member = memberNames[index];
        if (!base.isEmpty() && QDir::isRelativePath(QDir::fromNativeSeparators(relative)))
        {
            if (member.isEmpty())
            {
                if (const auto found = existingFile(QDir(base).filePath(relative)); found && tabsetIdentityKey(*found) == key)
                    return entry;
            }
            else if (const auto found = existingTarget(relative, base, members);
                     found && tabsetIdentityKey(found->file) == key && found->member == member)
                return entry;
        }
        auto fresh = member.isEmpty() ? tabsetEntry(file, base) : tabsetMemberEntry(file, member, base);
        return fresh == entry ? entry : fresh;
    };
    std::vector<TabsetEntry> result;
    // New entries wait for the next kept entry, so that unopened entries before it keep their positions.
    std::vector<TabsetEntry> added;
    const auto flush = [&]
    {
        std::ranges::move(added, std::back_inserter(result));
        added.clear();
    };
    std::size_t next = 0; // Existing entries before this one have been kept or passed over.
    const auto passTo = [&](std::size_t end)
    {
        for (; next < end; ++next)
            if (!used[next] && retained(next))
            {
                used[next] = true;
                if (inPlace(next))
                {
                    result.push_back(current(next, targets[next], named[next])); // Open: its archive is listed.
                    continue;
                }
                result.push_back(existing->entries[next]);
                if (changes && !nested[next])
                    changes->unopened.push_back(existing->entries[next]);
            }
    };
    // The entry of a listed file itself; member entries are kept where they are instead.
    const auto find = [&](const QString& key, std::size_t begin, std::size_t end) -> std::optional<std::size_t>
    {
        for (auto index = begin; index < end; ++index)
            if (!used[index] && !isMember(index) && named[index] == key)
                return index;
        return std::nullopt;
    };
    for (const auto& file : files)
    {
        const auto key = tabsetIdentityKey(file);
        // An archive that member entries stand for keeps all its entries where they are (passTo).
        if (throughMembers.contains(key))
            continue;
        auto found = find(key, next, named.size());
        const bool ahead = found.has_value();
        if (!found)
            found = find(key, 0, next);
        if (!found)
        {
            // A file without an entry of its own that a kept nested tabset opens stays listed through that tabset.
            if (!throughNested.contains(key))
                added.push_back(tabsetEntry(file, base));
            continue;
        }
        if (ahead)
            passTo(*found);
        flush();
        // Keep the entry with the repeats that follow it: Tracker writes an archive once per experiment tab.
        auto index = *found;
        do
        {
            used[index] = true;
            result.push_back(current(index, file, key));
            ++index;
        } while (index < named.size() && !used[index] && !isMember(index) && named[index] == key);
        if (ahead)
            next = index;
    }
    passTo(named.size());
    flush();
    // Every other existing entry is dropped: one for a file the user closed, or a repeat of a listed file.
    if (changes)
        for (std::size_t index = 0; index < named.size(); ++index)
            if (!used[index])
                (dropped[index] ? changes->closed : changes->repeated).push_back(existing->entries[index]);
    return result;
}

QString trackerTabsetPath(const QString& path)
{
    // TrackerIO.fixXML: XML.stripExtension and the default ".trk" extension.
    const auto name = QFileInfo(path).fileName();
    const auto dot = name.lastIndexOf('.');
    if (dot > 0 && QStringView(name).sliced(dot + 1).compare(u"trk", Qt::CaseInsensitive) == 0)
        return path;
    return path.left(path.size() - name.size()) + (dot > 0 ? name.first(dot) : name) + ".trk";
}

TabsetSave saveTrackerTabsetFiles(const QString& path,
                                  std::span<const QString> files,
                                  const QString& workingDirectory,
                                  std::span<const TabsetFileKeys> closed)
{
    TabsetSave result;
    std::optional<TrackerTabset> existing;
    if (QFileInfo::exists(path))
    {
        if (!isTrackerTabsetFile(path))
        {
            result.status = TabsetSave::Status::NotTabset;
            return result;
        }
        auto read = readTrackerTabset(path);
        if (!read)
        {
            result.status = TabsetSave::Status::Unreadable;
            result.diagnostic = std::move(read).error();
            return result;
        }
        existing = std::move(*read);
    }
    const auto* baseline = existing ? &*existing : nullptr;
    const auto entries = tabsetEntries(files, path, workingDirectory, baseline, closed, &result.changes);
    if (auto written = saveTrackerTabset(path, entries, baseline); !written)
    {
        result.status = TabsetSave::Status::Failed;
        result.diagnostic = std::move(written).error();
        result.changes = {};
        return result;
    }
    result.status = TabsetSave::Status::Saved;
    return result;
}

std::expected<void, std::string> saveTrackerTabset(const QString& path, std::span<const TabsetEntry> entries, const TrackerTabset* existing)
{
    auto xml = existing ? renderTrackerTabset(*existing, entries) : writeTrackerTabset(entries);
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
