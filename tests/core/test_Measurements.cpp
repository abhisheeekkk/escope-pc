#include <gtest/gtest.h>
#include "analysis/Measurements.h"
#include <cmath>
#include <numbers>

using namespace escope;

static std::vector<AnalogSample> make_sine(double freq_hz, double amp,
                                           double sample_rate, int n_cycles) {
    int n = static_cast<int>(sample_rate / freq_hz * n_cycles);
    std::vector<AnalogSample> s(n);
    double dt_ns = 1e9 / sample_rate;
    for (int i = 0; i < n; ++i) {
        s[i].timestamp_ns = i * dt_ns;
        s[i].voltage = static_cast<float>(
            amp * std::sin(2.0 * std::numbers::pi * freq_hz * i / sample_rate));
    }
    return s;
}

TEST(Measurements, SineVpp) {
    auto s = make_sine(1000.0, 1.0, 1e6, 10);
    auto m = compute_measurements(s);
    EXPECT_TRUE(m.valid);
    EXPECT_NEAR(m.v_pp, 2.0f, 0.05f); // 1.0 amp → 2V pp
}

TEST(Measurements, SineFrequency) {
    auto s = make_sine(1000.0, 1.0, 1e6, 10);
    auto m = compute_measurements(s);
    EXPECT_NEAR(m.freq_hz, 1000.0, 5.0); // within 5 Hz
}

TEST(Measurements, SineMean) {
    auto s = make_sine(1000.0, 1.0, 1e6, 10);
    auto m = compute_measurements(s);
    EXPECT_NEAR(m.v_mean, 0.0f, 0.01f); // sine mean = 0
}

TEST(Measurements, EmptyReturnsInvalid) {
    std::vector<AnalogSample> empty;
    auto m = compute_measurements(empty);
    EXPECT_FALSE(m.valid);
}
