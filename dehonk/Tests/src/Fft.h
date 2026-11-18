// Iterative radix-2 complex FFT in double precision. Used only by the harness for analysis
// and test-signal synthesis, never by the DSP core.
#pragma once

#include <complex>
#include <cstddef>
#include <vector>

namespace dh
{

std::size_t nextPow2 (std::size_t n);
std::size_t prevPow2 (std::size_t n);

/** In place. Size must be a power of two. Forward uses e^{-j...}; inverse is unscaled. */
void fft (std::vector<std::complex<double>>& a, bool inverse);

} // namespace dh
