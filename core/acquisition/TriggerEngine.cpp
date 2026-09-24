#include "acquisition/TriggerEngine.h"

namespace escope {

TriggerEngine::TriggerEngine(TriggerConfig config)
    : config_(std::move(config))
{}

void TriggerEngine::set_config(const TriggerConfig& config) {
    config_ = config;
    reset();
}

void TriggerEngine::set_callback(TriggerCallback cb) {
    callback_ = std::move(cb);
}

std::optional<TriggerEvent> TriggerEngine::evaluate_analog(double timestamp_ns, float voltage) {
    if (!armed_) return std::nullopt;

    // Only handles analog sources
    if (config_.source != TriggerSource::Analog0 &&
        config_.source != TriggerSource::Analog1) {
        prev_voltage_ = voltage;
        return std::nullopt;
    }

    bool fired = false;
    const float hi = config_.level_v + config_.hysteresis_v / 2.f;
    const float lo = config_.level_v - config_.hysteresis_v / 2.f;

    switch (config_.condition) {
    case TriggerCondition::RisingEdge:
        fired = (prev_voltage_ < lo && voltage >= hi);
        break;
    case TriggerCondition::FallingEdge:
        fired = (prev_voltage_ > hi && voltage <= lo);
        break;
    case TriggerCondition::EitherEdge:
        fired = (prev_voltage_ < lo && voltage >= hi) ||
                (prev_voltage_ > hi && voltage <= lo);
        break;
    case TriggerCondition::LevelHigh:
        fired = (voltage >= hi);
        break;
    case TriggerCondition::LevelLow:
        fired = (voltage <= lo);
        break;
    default:
        break;
    }

    prev_voltage_ = voltage;
    if (fired) return fire(timestamp_ns);
    return std::nullopt;
}

std::optional<TriggerEvent> TriggerEngine::evaluate_digital(double timestamp_ns,
                                                              uint8_t channel, bool rising) {
    if (!armed_) return std::nullopt;

    // Map channel index to TriggerSource enum
    auto expected_src = static_cast<TriggerSource>(
        static_cast<uint8_t>(TriggerSource::Digital0) + channel);
    if (config_.source != expected_src) return std::nullopt;

    bool fired = false;
    switch (config_.condition) {
    case TriggerCondition::RisingEdge:  fired =  rising; break;
    case TriggerCondition::FallingEdge: fired = !rising; break;
    case TriggerCondition::EitherEdge:  fired = true;    break;
    default: break;
    }

    if (fired) return fire(timestamp_ns);
    return std::nullopt;
}

std::optional<TriggerEvent> TriggerEngine::force_trigger(double timestamp_ns) {
    return fire(timestamp_ns);
}

std::optional<TriggerEvent> TriggerEngine::fire(double timestamp_ns) {
    if (config_.mode != TriggerMode::Auto) {
        disarm(); // Single / Normal: disarm after one event
    }

    TriggerEvent evt{timestamp_ns, config_.source, config_.condition};
    if (callback_) callback_(evt);
    return evt;
}

void TriggerEngine::reset() {
    armed_        = true;
    prev_voltage_ = 0.f;
    prev_level_   = false;
}

} // namespace escope
