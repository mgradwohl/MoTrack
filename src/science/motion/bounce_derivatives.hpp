#pragma once

#include "science/motion/motion_derivatives.hpp"
#include "science/motion/motion_types.hpp"

#include <optional>
#include <span>

// Internal to motrack_motion: the Bounce Detection algorithm behind stepDerivatives().

namespace motrack::science::motion_detail
{

/// Tracker's BounceDerivatives.evaluate with bounceDerivsSpill = 3 on positions with finite coordinates.
/// The result is unmasked: it can hold estimates at steps without a position, which the caller drops.
[[nodiscard]] StepDerivatives bounceStepDerivatives(const ClipLattice& clip, std::span<const std::optional<PlanarValue>> positions);

} // namespace motrack::science::motion_detail
