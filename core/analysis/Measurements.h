#pragma once

#include "acquisition/SampleBuffer.h"
#include <optional>
#include <vector>

namespace escope {

/// Results of a standard set of waveform measurements.
struct WaveformMeasurements {
    float  v_min     = 0.f;   ///< Minimum voltage
    float  v_max     = 0.f;   ///< Maximum voltage
    float  v_pp      = 0.f;   ///< Peak-to-peak
    float  v_mean    = 0.f;   ///< Mean (DC component)
    float  v_rms     = 0.f;   ///< RMS voltage
    double period_ns = 0.0;   ///< Period (0 if no edges detected)
    double freq_hz   = 0.0;   ///< Frequency
    double duty_pct  = 0.0;   ///< Duty cycle (0–100)
    double rise_ns   = 0.0;   ///< Rise time (10–90%)
    double fall_ns   = 0.0;   ///< Fall time (90–10%)

    bool valid = false;        ///< True if measurements were computed
};

/// Compute standard measurements on a snapshot of analog samples.
/// @param samples  Ordered sample vector (from SampleBuffer::snapshot).
/// @param threshold_pct  Percentage of Vpp used as the crossing threshold (default 50%).
WaveformMeasurements compute_measurements(const std::vector<AnalogSample>& samples,
                                          float threshold_pct = 0.5f);

/// Compute measurements directly from a buffer.
WaveformMeasurements compute_measurements(const SampleBuffer& buf,
                                          float threshold_pct = 0.5f);

/// Cursor measurement between two timestamps.
struct CursorMeasurement {
    double delta_t_ns   = 0.0;
    double freq_hz      = 0.0;  ///< 1 / delta_t
    float  delta_v      = 0.f;
    float  v_at_cursor1 = 0.f;
    float  v_at_cursor2 = 0.f;
};

CursorMeasurement compute_cursor(const std::vector<AnalogSample>& samples,
                                 double t1_ns, double t2_ns);

} // namespace escope
