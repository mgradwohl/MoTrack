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
/// Tracker could not read because of their index order are rewritten in order whenever anything changes. Only new
/// entries must be representable; entries read from `original` are kept as read. The output is re-parsed and
/// checked like every standard export (oracle O8).
[[nodiscard]] std::expected<std::string, std::string> renderTrackerTabset(const TrackerTabset& original,
                                                                          std::span<const TabsetEntry> entries);

/// Tracker-compatible relative reference from `baseDirectory` to `file` (both absolute). Follows OSP's
/// five-level climb, including Windows drive roots but never the POSIX root, and returns the absolute path
/// otherwise. Unlike OSP, a directory only contains a file at a path-component boundary.
[[nodiscard]] QString tabsetRelativePath(const QString& file, const QString& baseDirectory);
/// Saved entry for a physical file referenced from a tabset written in `tabsetDirectory`.
[[nodiscard]] TabsetEntry tabsetEntry(const QString& file, const QString& tabsetDirectory);
/// Name key for a canonical path: case-folded on Windows only.
[[nodiscard]] QString tabsetNameKey(const QString& canonical);
/// The keys of a file at a canonical path: its name key, and its identity (localFileIdentity) while it exists.
struct TabsetFileKeys
{
    QString name{};
    std::optional<QString> identity{};
    /// The identity, or the name for a file without one: what duplicates, cycles and listed files compare.
    [[nodiscard]] QString key() const
    {
        return identity.value_or(name);
    }
    friend bool operator==(const TabsetFileKeys&, const TabsetFileKeys&) = default;
};
[[nodiscard]] TabsetFileKeys tabsetFileKeys(const QString& canonical);
/// tabsetFileKeys(canonical).key(), comparing files that exist now.
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
/// that path holds now:
/// - each of its entries that resolves to a listed file in Tracker's order is kept where that file is listed, with
///   the repeats that follow it (Tracker lists an archive once per experiment tab). It keeps its bytes while its
///   relative path still finds the file from tabsetBase, and is rewritten otherwise;
/// - entries for files that are not listed keep their place, except those naming a file in `closed` (the keys of
///   files the user closed, taken when each closed). A file that failed to open, was not found or was not opened is
///   therefore kept; a nested tabset is kept for the listed files it opens. A file id names a file only while it
///   exists, and replacing a file by renaming a new one into place changes it, so an entry is dropped only when the
///   file it names now has both the closed file's name and identity: a later file that got a deleted file's id, or a
///   file saved over since, keeps its entry;
/// so that saving an unchanged set leaves the file unchanged, apart from entries whose relative path no longer finds
/// their file. Other files get a new tabsetEntry against tabsetBase. `changes` reports every entry that is kept
/// without being open and every entry that is dropped, so that no change is silent.
struct TabsetChanges
{
    std::vector<TabsetEntry> unopened{}; ///< Kept although their file is not listed.
    std::vector<TabsetEntry> closed{};   ///< Dropped because the user closed their file.
    std::vector<TabsetEntry> repeated{}; ///< Dropped repeats of a listed file that do not follow its entry.
};
[[nodiscard]] std::vector<TabsetEntry> tabsetEntries(std::span<const QString> files,
                                                     const QString& tabsetPath,
                                                     const QString& workingDirectory,
                                                     const TrackerTabset* existing = nullptr,
                                                     std::span<const TabsetFileKeys> closed = {},
                                                     TabsetChanges* changes = nullptr);
/// TrackerIO.fixXML: a tabset path whose extension is not `.trk` gets `.trk` in place of it ("set.trz" and "Lab 3.5"
/// become "set.trk" and "Lab 3.trk"), so the saved file reopens as a tabset.
[[nodiscard]] QString trackerTabsetPath(const QString& path);

/// The outcome of saveTrackerTabsetFiles.
struct TabsetSave
{
    enum class Status : std::uint8_t
    {
        Saved,
        NotTabset,  ///< The destination exists and is not a tabset; it is not replaced.
        Unreadable, ///< The destination is a tabset MoTrack cannot read; it is not replaced.
        Failed,     ///< The tabset could not be written; any existing file is unchanged.
    };
    Status status = Status::Failed;
    std::string diagnostic{};
    TabsetChanges changes{}; ///< See tabsetEntries; empty unless saved.
};
/// The file work of Save tabset as…, safe to run off the GUI thread: read the tabset at `path` (bounded) when it
/// exists, compute tabsetEntries for `files` and save, editing the existing tabset in place.
[[nodiscard]] TabsetSave saveTrackerTabsetFiles(const QString& path,
                                                std::span<const QString> files,
                                                const QString& workingDirectory,
                                                std::span<const TabsetFileKeys> closed = {});
/// Write a tabset atomically: `existing` (read from `path`) edited by renderTrackerTabset, or a new document.
/// Existing destinations are only replaced after the complete file is staged.
[[nodiscard]] std::expected<void, std::string>
saveTrackerTabset(const QString& path, std::span<const TabsetEntry> entries, const TrackerTabset* existing = nullptr);

} // namespace motrack
