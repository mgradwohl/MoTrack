#pragma once

#include <cstdint>

namespace motrack
{
/// Process-local completion order shared by native writes and paired journal observations.
/// Stamp the result in the worker before releasing any destination lock; never serialize it.
[[nodiscard]] std::uint64_t nextSaveObservation();
} // namespace motrack
