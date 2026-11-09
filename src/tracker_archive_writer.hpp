#pragma once

#include <QString>

#include <atomic>
#include <span>
#include <string>
#include <string_view>

namespace motrack
{
class TrackerArchive;

/// Borrowed XML must remain valid and unchanged for the entire synchronous export call.
struct TrackerArchiveReplacement
{
    QString member;
    std::string_view xml;
};

/// Atomically replace a nonempty set of distinct existing TRK members in one archive.
/// Retains physical entry order, central-directory order and untouched compressed records/metadata.
/// All replacement XML and the complete staged archive are validated before destination replacement.
/// The source and extracted members remain unchanged; aggregate sizes retain the reader's limits.
void writeTrackerArchive(const QString& path,
                         const TrackerArchive& source,
                         std::span<const TrackerArchiveReplacement> replacements,
                         const std::atomic_bool* cancelled = nullptr);

/// Replace one existing TRK member, retaining other members and ZIP metadata.
/// Supports bounded, single-disk classic ZIPs with stored/deflated entries; unsupported
/// layouts are refused before replacement. Source ownership and extracted files stay unchanged.
void writeTrackerArchive(const QString& path,
                         const TrackerArchive& source,
                         const QString& member,
                         const std::string& xml,
                         const std::atomic_bool* cancelled = nullptr);
} // namespace motrack
