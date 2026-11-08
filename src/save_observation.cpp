#include "save_observation.hpp"

#include <atomic>

namespace motrack
{
std::uint64_t nextSaveObservation()
{
    static std::atomic<std::uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}
} // namespace motrack
