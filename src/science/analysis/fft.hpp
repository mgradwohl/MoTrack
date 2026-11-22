#pragma once

#include <span>

namespace motrack::science
{

/// Forward complex discrete Fourier transform in place, X_k = sum_j z_j exp(-2 pi i j k / n).
///
/// Ports `org.opensourcephysics.numerics.FFT.transform` (OSP d56207b0, GSL's mixed-radix
/// algorithm): `interleaved` holds n complex values as {re0, im0, re1, im1, ...}. Factors 7, 6, 5,
/// 4, 3 and 2 use dedicated passes; other primes use the general pass. The result is unnormalized
/// and in wrap-around order (k = 0 first). Returns false, leaving the data unchanged, when the span
/// is empty or has an odd number of doubles (OSP throws `IllegalArgumentException`).
[[nodiscard]] bool complexFft(std::span<double> interleaved);

/// Forward real discrete Fourier transform in place, packed like `FFTReal.transform`.
///
/// For n real values (n even): data[0] = X_0, data[1] = X_{n/2} (both real), and data[2k],
/// data[2k + 1] = Re X_k, Im X_k for 0 < k < n/2, with X_k = sum_j y_j exp(-2 pi i j k / n).
/// Deliberate difference: OSP's unpacking loop stops one pair early when n/2 is odd and n > 6
/// (n = 10, 14, 18, ...), leaving X_k and X_{n/2-k} for k = (n/2 - 1)/2 as raw intermediate values.
/// MoTrack completes that pair; all other outputs follow OSP's operation order. Returns false,
/// leaving the data unchanged, when n is zero or odd.
[[nodiscard]] bool realFft(std::span<double> data);

} // namespace motrack::science
