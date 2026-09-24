#pragma once

// KissFFT is MIT-licensed — no GPL contamination.
// Include the single-header version bundled in third_party/.
// For now we provide a simple interface using std::vector and the DFT.

#include "acquisition/SampleBuffer.h"
#include <vector>
#include <complex>

namespace escope {

struct FFTBin {
    double frequency_hz;
    float  magnitude_db;
    float  phase_rad;
};

struct FFTConfig {
    std::size_t size        = 4096;    ///< FFT size (must be power of 2)
    double      sample_rate = 10e6;    ///< Hz, used to compute frequency axis
    bool        apply_window = true;   ///< Apply Hann window
};

/// Compute FFT of a sample snapshot.
/// Returns empty on error (too few samples, bad config).
std::vector<FFTBin> compute_fft(const std::vector<AnalogSample>& samples,
                                const FFTConfig& config = {});

/// Helper: compute power spectral density in dBFS.
std::vector<FFTBin> compute_fft(const SampleBuffer& buf,
                                 const FFTConfig& config = {});

} // namespace escope
