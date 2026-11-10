#pragma once

#include "archive_group.hpp"

#include <QJsonObject>

namespace motrack::detail
{

/// Decode version-6 metadata against one already-extracted immutable source owner.
/// Shared by the dedicated group reader and complete workspace input dispatch.
[[nodiscard]] ArchiveGroup
decodeArchiveGroup(const QJsonObject& input, std::shared_ptr<const TrackerArchive> archive, const std::atomic_bool* cancelled);

} // namespace motrack::detail
