#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace escope {

/// Trigger source selection
enum class TriggerSource : uint8_t {
    Analog0,    ///< Analog channel 0
    Analog1,    ///< Analog channel 1
    Digital0,   ///< Digital channel 0–7
    Digital1,
    Digital2,
    Digital3,
    Digital4,
    Digital5,
    Digital6,
    Digital7,
    External,   ///< External trigger input
    Software,   ///< Manual / software trigger
};

/// Trigger condition
enum class TriggerCondition : uint8_t {
    RisingEdge,
    FallingEdge,
    EitherEdge,
    LevelHigh,
    LevelLow,
    PulseWidthMin,  ///< Pulse wider than threshold
    PulseWidthMax,  ///< Pulse narrower than threshold
};

/// Trigger mode
enum class TriggerMode : uint8_t {
    Auto,       ///< Always display, trigger when possible
    Normal,     ///< Wait for trigger condition
    Single,     ///< Capture once then stop
};

/// Full trigger configuration
struct TriggerConfig {
    TriggerSource    source    = TriggerSource::Analog0;
    TriggerCondition condition = TriggerCondition::RisingEdge;
    TriggerMode      mode      = TriggerMode::Auto;
    float            level_v   = 1.65f;  ///< Voltage threshold (analog triggers)
    float            hysteresis_v = 0.1f;
    double           pre_trigger_ns  = 0.0;  ///< How much data to keep before trigger
    double           post_trigger_ns = 1e6;  ///< Capture window after trigger (1 ms default)
};

/// Result returned when a trigger fires.
struct TriggerEvent {
    double timestamp_ns;
    TriggerSource source;
    TriggerCondition condition;
};

/// Callback type: called on the acquisition thread when a trigger fires.
using TriggerCallback = std::function<void(const TriggerEvent&)>;

/// Software-side trigger evaluator.
/// The real trigger fires in hardware (FPGA/MCU); this mirrors the logic for
/// simulation, playback, and software-trigger mode.
class TriggerEngine {
public:
    explicit TriggerEngine(TriggerConfig config = {});

    void set_config(const TriggerConfig& config);
    const TriggerConfig& config() const noexcept { return config_; }

    void set_callback(TriggerCallback cb);

    /// Feed a new analog sample; returns trigger timestamp if fired.
    std::optional<TriggerEvent> evaluate_analog(double timestamp_ns, float voltage);

    /// Feed a new digital edge; returns trigger event if fired.
    std::optional<TriggerEvent> evaluate_digital(double timestamp_ns, uint8_t channel, bool rising);

    /// Force a software trigger immediately.
    std::optional<TriggerEvent> force_trigger(double timestamp_ns);

    /// Reset internal state (after a capture completes).
    void reset();

    bool is_armed() const noexcept { return armed_; }
    void arm()     noexcept { armed_ = true; }
    void disarm()  noexcept { armed_ = false; }

private:
    TriggerConfig    config_;
    TriggerCallback  callback_;
    bool             armed_          = true;
    float            prev_voltage_   = 0.f;
    bool             prev_level_     = false;

    std::optional<TriggerEvent> fire(double timestamp_ns);
};

} // namespace escope
