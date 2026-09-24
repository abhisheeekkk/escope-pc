#include "analysis/Measurements.h"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace escope {

WaveformMeasurements compute_measurements(const std::vector<AnalogSample>& samples,
                                          float threshold_pct) {
    WaveformMeasurements m;
    if (samples.size() < 2) return m;

    // ── Min / max / mean / RMS — single pass ─────────────────────────────────
    double sum    = 0.0;
    double sum_sq = 0.0;
    m.v_min = samples[0].voltage;
    m.v_max = samples[0].voltage;

    for (const auto& s : samples) {
        if (s.voltage < m.v_min) m.v_min = s.voltage;
        if (s.voltage > m.v_max) m.v_max = s.voltage;
        sum    += s.voltage;
        sum_sq += static_cast<double>(s.voltage) * s.voltage;
    }

    m.v_pp   = m.v_max - m.v_min;
    m.v_mean = static_cast<float>(sum / samples.size());
    m.v_rms  = static_cast<float>(std::sqrt(sum_sq / samples.size()));

    if (m.v_pp < 1e-6f) { m.valid = true; return m; } // DC signal

    // ── Frequency / period via hysteretic threshold crossing ──────────────────
    // Using two thresholds (Schmitt trigger style) eliminates noise-induced
    // false crossings on a sine wave with Gaussian noise.
    //
    // hi_thresh: voltage must rise above this to register a rising crossing
    // lo_thresh: voltage must fall below this before the next rising crossing
    //
    // The gap between them (hysteresis band) must be larger than peak noise.
    // We use 20% of Vpp on each side of the midpoint.
    const float mid       = m.v_min + m.v_pp * threshold_pct;
    const float hyst      = m.v_pp * 0.10f;   // ±10% of Vpp hysteresis
    const float hi_thresh = mid + hyst;
    const float lo_thresh = mid - hyst;

    // Estimate a minimum valid period: require at least 4 samples per cycle.
    // This rejects crossings that are closer together than physically possible.
    double dt_ns = 0.0;
    if (samples.size() >= 2)
        dt_ns = (samples.back().timestamp_ns - samples.front().timestamp_ns)
                / (samples.size() - 1);
    const double min_period_ns = dt_ns * 4.0;

    std::vector<double> rising_times;
    rising_times.reserve(512);

    // State machine: track whether signal is in HIGH or LOW zone
    // Only register a rising crossing when coming from LOW zone
    enum class Zone { Unknown, Low, High };
    Zone zone = (samples[0].voltage > hi_thresh) ? Zone::High
              : (samples[0].voltage < lo_thresh) ? Zone::Low
              : Zone::Unknown;

    double last_rising_ns = -1e18;

    for (std::size_t i = 1; i < samples.size(); ++i) {
        float v = samples[i].voltage;

        if (zone != Zone::High && v >= hi_thresh) {
            // Rising crossing — only count if enough time has passed
            float  v0 = samples[i-1].voltage;
            double t0 = samples[i-1].timestamp_ns;
            double t1 = samples[i].timestamp_ns;
            // Linear interpolation for sub-sample accuracy
            double t_cross = t0 + (hi_thresh - v0) / (v - v0) * (t1 - t0);

            if (t_cross - last_rising_ns > min_period_ns) {
                rising_times.push_back(t_cross);
                last_rising_ns = t_cross;
            }
            zone = Zone::High;
        } else if (zone != Zone::Low && v <= lo_thresh) {
            zone = Zone::Low;
        }
    }

    // ── Period: median of inter-crossing intervals (robust to outliers) ───────
    if (rising_times.size() >= 2) {
        std::vector<double> periods;
        periods.reserve(rising_times.size() - 1);
        for (std::size_t i = 1; i < rising_times.size(); ++i)
            periods.push_back(rising_times[i] - rising_times[i-1]);

        // Median is more robust than mean for noisy signals
        std::sort(periods.begin(), periods.end());
        double median_period = periods[periods.size() / 2];

        // Sanity check: reject if period implies freq > Nyquist (sample_rate/2)
        // We don't know the sample rate here, but dt_ns gives us the Nyquist limit
        if (dt_ns > 0.0 && median_period >= dt_ns * 2.0) {
            m.period_ns = median_period;
            m.freq_hz   = 1e9 / median_period;
        }
    }

    // ── Duty cycle — using the same hysteretic threshold ─────────────────────
    double time_above = 0.0;
    double total_time = samples.back().timestamp_ns - samples.front().timestamp_ns;
    for (std::size_t i = 1; i < samples.size(); ++i) {
        double dt = samples[i].timestamp_ns - samples[i-1].timestamp_ns;
        if (samples[i-1].voltage >= mid) time_above += dt;
    }
    m.duty_pct = (total_time > 0.0) ? 100.0 * time_above / total_time : 0.0;

    // ── Rise/fall time (10%–90%) ──────────────────────────────────────────────
    // Find first rising edge that goes from 10% to 90% of Vpp
    const float v10 = m.v_min + m.v_pp * 0.10f;
    const float v90 = m.v_min + m.v_pp * 0.90f;

    bool looking_for_10 = true;
    double t_10 = 0.0;
    for (std::size_t i = 1; i < samples.size(); ++i) {
        if (looking_for_10 && samples[i-1].voltage <= v10 && samples[i].voltage > v10) {
            t_10 = samples[i-1].timestamp_ns
                 + (v10 - samples[i-1].voltage) / (samples[i].voltage - samples[i-1].voltage)
                 * (samples[i].timestamp_ns - samples[i-1].timestamp_ns);
            looking_for_10 = false;
        } else if (!looking_for_10 && samples[i-1].voltage <= v90 && samples[i].voltage > v90) {
            double t_90 = samples[i-1].timestamp_ns
                        + (v90 - samples[i-1].voltage) / (samples[i].voltage - samples[i-1].voltage)
                        * (samples[i].timestamp_ns - samples[i-1].timestamp_ns);
            m.rise_ns = t_90 - t_10;
            break;
        }
    }

    m.valid = true;
    return m;
}

WaveformMeasurements compute_measurements(const SampleBuffer& buf, float threshold_pct) {
    std::vector<AnalogSample> samples;
    buf.snapshot(samples);
    return compute_measurements(samples, threshold_pct);
}

CursorMeasurement compute_cursor(const std::vector<AnalogSample>& samples,
                                 double t1_ns, double t2_ns) {
    CursorMeasurement c;
    if (samples.empty()) return c;

    // Binary search — O(log n) instead of the old O(n) linear scan
    auto nearest_v = [&](double t) -> float {
        auto it = std::lower_bound(samples.begin(), samples.end(), t,
            [](const AnalogSample& s, double ts){ return s.timestamp_ns < ts; });
        if (it == samples.end()) return samples.back().voltage;
        if (it == samples.begin()) return it->voltage;
        auto prev = std::prev(it);
        return (std::abs(it->timestamp_ns - t) < std::abs(prev->timestamp_ns - t))
             ? it->voltage : prev->voltage;
    };

    c.v_at_cursor1 = nearest_v(t1_ns);
    c.v_at_cursor2 = nearest_v(t2_ns);
    c.delta_v      = c.v_at_cursor2 - c.v_at_cursor1;
    c.delta_t_ns   = t2_ns - t1_ns;
    c.freq_hz      = (c.delta_t_ns != 0.0) ? 1e9 / std::abs(c.delta_t_ns) : 0.0;
    return c;
}

} // namespace escope
