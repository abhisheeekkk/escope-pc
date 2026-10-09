#include "session/CaptureSession.h"
#include <stdexcept>
#include <string>

namespace escope {

CaptureSession::CaptureSession(std::size_t analog_buf_size) {
    // Analog channels (kept for future use, not displayed in digital-only mode)
    analog_info_.resize(MAX_ANALOG_CH);
    for (std::size_t i = 0; i < MAX_ANALOG_CH; ++i) {
        analog_info_[i].label = "CH" + std::to_string(i + 1);
        analog_info_[i].color = (i == 0) ? 0xFFFF00FF : 0x00FFFFFF;
        analog_info_[i].enabled = false; // disabled in digital-only mode
        analog_bufs_.push_back(std::make_unique<SampleBuffer>(analog_buf_size));
    }

    // Digital channels
    digital_info_.resize(MAX_DIGITAL_CH);
    for (std::size_t i = 0; i < MAX_DIGITAL_CH; ++i) {
        digital_info_[i].label   = "D" + std::to_string(i);   // protocol roles are shown only once a decoder is chosen
        digital_info_[i].enabled = true;
    }

    digital_ = std::make_unique<DigitalBuffer>(static_cast<uint8_t>(MAX_DIGITAL_CH));
}

SampleBuffer& CaptureSession::analog_buffer(std::size_t ch) {
    if (ch >= analog_bufs_.size()) throw std::out_of_range("analog channel index");
    return *analog_bufs_[ch];
}
const SampleBuffer& CaptureSession::analog_buffer(std::size_t ch) const {
    if (ch >= analog_bufs_.size()) throw std::out_of_range("analog channel index");
    return *analog_bufs_[ch];
}
AnalogChannelInfo& CaptureSession::analog_info(std::size_t ch) {
    if (ch >= analog_info_.size()) throw std::out_of_range("analog channel index");
    return analog_info_[ch];
}
const AnalogChannelInfo& CaptureSession::analog_info(std::size_t ch) const {
    if (ch >= analog_info_.size()) throw std::out_of_range("analog channel index");
    return analog_info_[ch];
}
DigitalChannelInfo& CaptureSession::digital_info(std::size_t ch) {
    if (ch >= digital_info_.size()) throw std::out_of_range("digital channel index");
    return digital_info_[ch];
}
const DigitalChannelInfo& CaptureSession::digital_info(std::size_t ch) const {
    if (ch >= digital_info_.size()) throw std::out_of_range("digital channel index");
    return digital_info_[ch];
}

double CaptureSession::duration_ns() const {
    auto [t0, t1] = digital_->time_range_ns();
    return t1 - t0;
}

void CaptureSession::reset() {
    for (auto& buf : analog_bufs_) buf->clear();
    digital_->clear();
    trigger_.reset();
    trigger_event_.reset();
    state_ = State::Idle;
}

} // namespace escope
