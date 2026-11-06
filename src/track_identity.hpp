#pragma once

#include "legacy_project.hpp"

namespace motrack
{

[[nodiscard]] bool hasTrackIdCapacity(const LegacyProject& project);
/// Allocate a document-scoped identity. Zero is reserved; exhaustion throws ProjectError.
[[nodiscard]] TrackId allocateTrackId(LegacyProject& project);
/// Reject zero/duplicate identities and a counter that could reuse an existing identity.
void validateTrackIdentities(const LegacyProject& project);

} // namespace motrack
