#pragma once

#include <QString>

#include <atomic>
#include <string>

namespace motrack
{
class TrackerArchive;

/// Replace one existing TRK member, retaining other members and ZIP metadata.
/// Supports bounded, single-disk classic ZIPs with stored/deflated entries; unsupported
/// layouts are refused before replacement. Source ownership and extracted files stay unchanged.
void writeTrackerArchive(const QString& path,
                         const TrackerArchive& source,
                         const QString& member,
                         const std::string& xml,
                         const std::atomic_bool* cancelled = nullptr);
} // namespace motrack
