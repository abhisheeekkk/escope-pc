#pragma once

#include "acquisition/SampleBuffer.h"
#include "acquisition/DigitalBuffer.h"
#include "acquisition/TriggerEngine.h"

#include <string>
#include <vector>
#include <memory>
#include <chrono>
#include <optional>

namespace escope {

/// Metadata for one analog channel.
struct AnalogChannelInfo {
    std::string label        = "CH";
    float       v_div        = 1.0f;    ///< Volts per division
    float       v_offset     = 0.0f;    ///< Voltage offset
    float       probe_factor = 1.0f;    ///< 1x / 10x probe
    bool        enabled      = true;
    // Display colour stored as RGBA packed uint32 (0xRRGGBBAA)
    uint32_t    color        = 0xFFFF00FF; // yellow
};

/// Metadata for one digital channel.
struct DigitalChannelInfo {
    std::string label   = "D";
    bool        enabled = true;
    uint32_t    color   = 0x00FF00FF; // green
};

/// Session-level metadata.
struct SessionMetadata {
    std::string  session_id;                        ///< UUID
    std::string  device_name  = "Simulated";
    std::string  firmware_ver = "N/A";
    double       sample_rate_hz = 10e6;             ///< Analog sample rate
    double       digital_rate_hz = 50e6;            ///< Digital sample rate
    std::string  timestamp;                         ///< ISO8601 capture time
    std::string  notes;
};

/// CaptureSession is the root data object for one capture.
///
/// It owns:
///  - Up to 2 analog SampleBuffers
///  - 1 DigitalBuffer (8 channels)
///  - Trigger configuration
///  - Metadata
///
/// Everything shares one time base (nanoseconds from capture start = 0).
class CaptureSession {
public:
    static constexpr std::size_t MAX_ANALOG_CH  = 2;
    static constexpr std::size_t MAX_DIGITAL_CH = 16;
    static constexpr std::size_t DEFAULT_ANALOG_BUF = 4 * 1024 * 1024; // 4M samples/ch

    explicit CaptureSession(std::size_t analog_buf_size = DEFAULT_ANALOG_BUF);

    // ── Metadata ──────────────────────────────────────────────────────────────
    SessionMetadata&       metadata()       { return metadata_; }
    const SessionMetadata& metadata() const { return metadata_; }

    // ── Analog channels ───────────────────────────────────────────────────────
    std::size_t analog_channel_count() const { return analog_info_.size(); }

    SampleBuffer&       analog_buffer(std::size_t ch);
    const SampleBuffer& analog_buffer(std::size_t ch) const;

    AnalogChannelInfo&       analog_info(std::size_t ch);
    const AnalogChannelInfo& analog_info(std::size_t ch) const;

    // ── Digital channels ──────────────────────────────────────────────────────
    DigitalBuffer&       digital_buffer()       { return *digital_; }
    const DigitalBuffer& digital_buffer() const { return *digital_; }

    DigitalChannelInfo&       digital_info(std::size_t ch);
    const DigitalChannelInfo& digital_info(std::size_t ch) const;

    // ── Trigger ───────────────────────────────────────────────────────────────
    TriggerEngine&       trigger()       { return trigger_; }
    const TriggerEngine& trigger() const { return trigger_; }

    std::optional<TriggerEvent>& trigger_event() { return trigger_event_; }

    // ── State ─────────────────────────────────────────────────────────────────
    enum class State { Idle, Armed, Capturing, Complete, Error };
    State state() const noexcept { return state_; }
    void  set_state(State s) noexcept { state_ = s; }

    /// Duration of captured data in nanoseconds.
    double duration_ns() const;

    /// Clear all buffers and reset to Idle.
    void reset();

private:
    SessionMetadata                          metadata_;
    std::vector<AnalogChannelInfo>           analog_info_;
    std::vector<std::unique_ptr<SampleBuffer>> analog_bufs_;
    std::vector<DigitalChannelInfo>          digital_info_;
    std::unique_ptr<DigitalBuffer>           digital_;
    TriggerEngine                            trigger_;
    std::optional<TriggerEvent>              trigger_event_;
    State                                    state_ = State::Idle;
};

} // namespace escope
