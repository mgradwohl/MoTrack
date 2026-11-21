#pragma once

#include <QString>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
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

/// True when the document element is a Tracker `TFrame` (tabset) object. Malformed XML is not a tabset.
[[nodiscard]] bool isTrackerTabset(std::string_view xml);
/// Bounded read of a local file followed by isTrackerTabset. Unreadable or oversized files are not tabsets.
[[nodiscard]] bool isTrackerTabsetFile(const QString& path);
/// Parse a bounded `TFrame` document. Errors describe malformed XML, another root or an invalid `tabs` array.
/// A tabset without a `tabs` property is valid and empty, as in Tracker.
[[nodiscard]] std::expected<std::vector<TabsetEntry>, std::string> parseTrackerTabset(std::string_view xml);
/// Serialize in OSP's XMLControl layout. Strings with XML-special characters use CDATA because OSP does not
/// decode entities in ordinary strings; values containing `]]>` cannot be represented and are refused.
[[nodiscard]] std::expected<std::string, std::string> writeTrackerTabset(std::span<const TabsetEntry> entries);

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
/// Write a tabset atomically. Existing destinations are only replaced after the complete file is staged.
[[nodiscard]] std::expected<void, std::string> saveTrackerTabset(const QString& path, std::span<const TabsetEntry> entries);

} // namespace motrack
