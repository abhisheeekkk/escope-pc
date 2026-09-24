#include "analysis/FFT.h"
#include <cmath>
#include <algorithm>
#include <numbers>

namespace escope {

// Phase 1: simple DFT — correct but O(n²). Replace with KissFFT in Phase 2.
// KissFFT (MIT): https://github.com/mborgerding/kissfft

static std::vector<float> hann_window(std::size_t n) {
    std::vector<float> w(n);
    for (std::size_t i = 0; i < n; ++i) {
        w[i] = 0.5f * (1.f - std::cos(2.f * std::numbers::pi_v<float> * i / (n - 1)));
    }
    return w;
}

std::vector<FFTBin> compute_fft(const std::vector<AnalogSample>& samples,
                                const FFTConfig& config) {
    if (samples.size() < config.size) return {};

    // Extract voltage values (use the latest config.size samples)
    std::size_t offset = samples.size() - config.size;
    std::vector<float> x(config.size);
    for (std::size_t i = 0; i < config.size; ++i) {
        x[i] = samples[offset + i].voltage;
    }

    // Apply window
    if (config.apply_window) {
        auto w = hann_window(config.size);
        for (std::size_t i = 0; i < config.size; ++i) x[i] *= w[i];
    }

    // Compute DFT (only positive frequencies: 0..N/2)
    const std::size_t half = config.size / 2;
    std::vector<FFTBin> bins(half);
    const double inv_n = 1.0 / config.size;

    for (std::size_t k = 0; k < half; ++k) {
        double re = 0.0, im = 0.0;
        for (std::size_t n = 0; n < config.size; ++n) {
            double angle = -2.0 * std::numbers::pi * k * n * inv_n;
            re += x[n] * std::cos(angle);
            im += x[n] * std::sin(angle);
        }
        double mag = std::sqrt(re * re + im * im) * inv_n;
        if (k > 0) mag *= 2.0;  // single-sided spectrum

        bins[k].frequency_hz = k * config.sample_rate / config.size;
        bins[k].magnitude_db = static_cast<float>(
            20.0 * std::log10(std::max(mag, 1e-10)));
        bins[k].phase_rad    = static_cast<float>(std::atan2(im, re));
    }

    return bins;
}

std::vector<FFTBin> compute_fft(const SampleBuffer& buf, const FFTConfig& config) {
    std::vector<AnalogSample> samples;
    buf.snapshot(samples);
    return compute_fft(samples, config);
}

} // namespace escope
