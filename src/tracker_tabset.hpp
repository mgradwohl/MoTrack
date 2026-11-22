#pragma once

#include "osp/source.hpp"

#include <QString>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace motrack
{

inline constexpr std::string_view TRACKER_TABSET_CLASS = "org.opensourcephysics.cabrillo.tracker.TFrame";
/// Bounds one tabset file and the complete expansion of nested tabsets.
inline constexpr std::size_t MAX_TABSET_ENTRIES = 256;
inline constexpr int MAX_TABSET_DEPTH = 8;
/// Tracker's relative-path search climbs at most this many parent directories before storing an absolute path.
inline constexpr int MAX_TABSET_PARENT_LEVELS = 5;

/// One saved Tracker tab reference: `TFrame.Loader` writes a `String[2]` of {absolute, relative} paths with
/// forward slashes. `relative` is relative to the tabset's directory, or a copy of the absolute path when no
/// common ancestor lies within MAX_TABSET_PARENT_LEVELS. Tracker tolerates a missing absolute path.
struct TabsetEntry
{
    std::optional<std::string> absolute;
    std::string relative;
    bool operator==(const TabsetEntry&) const = default;
};

/// A tabset as OSP's line-oriented reader reads it (`motrack_osp`, as Tracker does): the immutable source, its
/// entries in index order and the nodes they were read from, so that saving can edit the file in place
/// (renderTrackerTabset) instead of rewriting it.
struct TrackerTabset
{
    /// The `String[]` element of one entry and its `[0]` (absolute) and `[1]` (relative) strings.
    struct EntryNodes
    {
        osp::NodeId element{};
        std::optional<osp::NodeId> absolute{};
        osp::NodeId relative{};
        /// `[0]` precedes `[1]`. OSP sizes an array from its last element, so Tracker cannot read `[1]` before `[0]`.
        bool ordered = true;
    };
    std::shared_ptr<const osp::Source> source;
    std::vector<TabsetEntry> entries;
    /// The TFrame's `getString("basepath")`: a saved string property of that name replaces the tabset's folder as
    /// the first base for relative paths, in Tracker and here. Tracker itself never writes one.
    std::optional<std::string> basepath{};
    std::optional<osp::NodeId> tabs{};
    std::vector<EntryNodes> nodes{}; ///< By entry index.
    bool ordered = true;             ///< Entries appear in index order in the file, which Tracker needs to read it.
};

/// True when OSP would read a Tracker `TFrame` (tabset) root: the first of the first ten lines that contains
/// `<object class=` names that class (XMLControlElement.readInput). Only that line is examined, so experiments are
/// not parsed twice; parseTrackerTabset then reads the whole document.
[[nodiscard]] bool isTrackerTabset(std::string_view xml);
/// isTrackerTabset for the start of a local file. Unreadable files are not tabsets.
[[nodiscard]] bool isTrackerTabsetFile(const QString& path);
/// Read a bounded `TFrame` document with `motrack_osp`. Errors describe unreadable XML (as Tracker import reports
/// it), another root or an invalid `tabs` array. A tabset without a `tabs` property is valid and empty, as in
/// Tracker. MoTrack also reads entries whose `[n]` indices are out of order, which Tracker cannot.
[[nodiscard]] std::expected<TrackerTabset, std::string> parseTrackerTabset(std::string_view xml);
/// parseTrackerTabset of a local file, bounded like Tracker import.
[[nodiscard]] std::expected<TrackerTabset, std::string> readTrackerTabset(const QString& path);
/// A new tabset in OSP's layout (XMLControlElement.toString, with LF line breaks). Strings with XML-special characters
/// use CDATA because OSP does not decode entities; paths containing `]]>`, NUL or line breaks cannot be written.
[[nodiscard]] std::expected<std::string, std::string> writeTrackerTabset(std::span<const TabsetEntry> entries);
/// `original` edited so that it lists `entries`. Unchanged entries and every other property keep their bytes; a
/// changed entry has only its changed paths replaced, so an unedited tabset renders byte-identical. Entries that
/// Tracker could not read because of their index order are rewritten in order when they change. The output is
/// re-parsed and checked like every standard export (oracle O8).
[[nodiscard]] std::expected<std::string, std::string> renderTrackerTabset(const TrackerTabset& original,
                                                                          std::span<const TabsetEntry> entries);

/// Tracker-compatible relative reference from `baseDirectory` to `file` (both absolute). Follows OSP's
/// five-level climb, including Windows drive roots but never the POSIX root, and returns the absolute path
/// otherwise. Unlike OSP, a directory only contains a file at a path-component boundary.
[[nodiscard]] QString tabsetRelativePath(const QString& file, const QString& baseDirectory);
/// Saved entry for a physical file referenced from a tabset written in `tabsetDirectory`.
[[nodiscard]] TabsetEntry tabsetEntry(const QString& file, const QString& tabsetDirectory);
/// Identity key for a canonical path, as used for duplicates and cycles: case-folded on Windows only.
[[nodiscard]] QString tabsetIdentityKey(const QString& canonical);
/// Resolve an existing file in Tracker's order: relative to the tabset directory, relative to the working
/// directory, then the absolute path. A relative slot that is itself absolute only uses the absolute fallback.
[[nodiscard]] std::optional<QString>
resolveTabsetEntry(const TabsetEntry& entry, const QString& tabsetDirectory, const QString& workingDirectory);
/// The folder a tabset's relative paths are resolved against: its saved `basepath`, or the tabset file's folder.
[[nodiscard]] QString tabsetBase(const TrackerTabset& tabset, const QString& tabsetPath);

enum class TabsetItemStatus : std::uint8_t
{
    Resolved,
    Missing,
    Duplicate,
    Cycle,
    Unreadable,
    TooDeep,
    TooMany,
};

/// One tab reference in open order. Nested tabsets are expanded in place and do not appear themselves.
struct TabsetItem
{
    TabsetEntry entry;
    /// Physical tabset file that listed this entry.
    QString tabset;
    /// Canonical physical file when Resolved; the unreadable nested tabset when Unreadable; otherwise empty.
    QString file;
    TabsetItemStatus status = TabsetItemStatus::Missing;
    /// Parser or read diagnostic for Unreadable nested tabsets.
    std::string diagnostic{};
};

/// Read `path` and resolve every entry, expanding nested tabsets depth-first. Each physical file appears once:
/// later references are Duplicate, a tabset already being expanded is Cycle. Cancellation and an unreadable
/// top-level file throw ProjectError; problems inside nested tabsets are reported as items instead.
[[nodiscard]] std::vector<TabsetItem>
expandTrackerTabset(const QString& path, const QString& workingDirectory, const std::atomic_bool* cancelled = nullptr);

/// Entries for a tabset at `tabsetPath` that lists `files` (canonical, in tab order). When `existing` is the tabset
/// that path holds now, each entry of it that still resolves to the listed file in Tracker's order is kept as it
/// is, together with the repeats that follow it (Tracker lists an archive once per experiment tab), so that saving
/// an unchanged set leaves the file unchanged. Other files get a new tabsetEntry against tabsetBase.
[[nodiscard]] std::vector<TabsetEntry> tabsetEntries(std::span<const QString> files,
                                                     const QString& tabsetPath,
                                                     const QString& workingDirectory,
                                                     const TrackerTabset* existing = nullptr);
/// Write a tabset atomically: `existing` (read from `path`) edited by renderTrackerTabset, or a new document.
/// Existing destinations are only replaced after the complete file is staged.
[[nodiscard]] std::expected<void, std::string>
saveTrackerTabset(const QString& path, std::span<const TabsetEntry> entries, const TrackerTabset* existing = nullptr);

} // namespace motrack
