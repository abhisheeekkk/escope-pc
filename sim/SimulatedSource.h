#pragma once

#include "hal/IDataSource.h"
#include <thread>
#include <atomic>

namespace escope {

/// Configuration for the simulated source.
/// Defined outside SimulatedSource so it can be used as a default argument.
struct SimulatedSourceConfig {
    double   ch1_freq_hz  = 1000.0;   ///< CH1 sine frequency
    double   ch1_amp_v    = 1.65;     ///< CH1 amplitude
    double   ch2_freq_hz  = 500.0;    ///< CH2 square wave
    double   ch2_amp_v    = 3.3;
    double   sample_rate  = 10e6;     ///< Analog sample rate (10 MS/s)
    double   digital_rate = 50e6;     ///< Digital edge resolution
    double   noise_v      = 0.02;     ///< Gaussian noise amplitude
    bool     gen_uart     = true;     ///< Generate synthetic UART on D0
    uint32_t uart_baud    = 115200;
};

/// Simulated data source for Phase 1 development.
///
/// Generates:
///   CH1 — sine wave at a configurable frequency
///   CH2 — square wave at a different frequency
///   D0–D2 — UART / square wave edges
///
/// Implements IDataSource so the GUI never needs to know it's simulated.
class SimulatedSource : public IDataSource {
public:
    using Config = SimulatedSourceConfig;

    explicit SimulatedSource(Config cfg = Config{});
    ~SimulatedSource() override;

    // ── IDataSource ───────────────────────────────────────────────────────────
    std::vector<DeviceInfo> enumerate() override;
    SourceStatus open(const std::string& device_id = "") override;
    void         close() override;
    bool         is_open()    const override { return open_; }

    SourceStatus configure(const CaptureSession& session) override;
    SourceStatus start(CaptureSession& session) override;
    void         stop() override;
    bool         is_running() const override { return running_; }

    void set_data_callback(DataCallback cb)       override { data_cb_    = std::move(cb); }
    void set_error_callback(ErrorCallback cb)     override { error_cb_   = std::move(cb); }
    void set_trigger_callback(TriggerCallback cb) override { trigger_cb_ = std::move(cb); }

    std::string name() const override { return "Simulated Device"; }

private:
    void  acquisition_loop(CaptureSession* session);
    float gaussian_noise();

    Config              cfg_;
    std::atomic<bool>   open_    {false};
    std::atomic<bool>   running_ {false};
    std::thread         worker_;

    DataCallback        data_cb_;
    ErrorCallback       error_cb_;
    TriggerCallback     trigger_cb_;

    unsigned int        rng_state_ = 42;
};

} // namespace escope
