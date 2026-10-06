#pragma once
#include "decoders/base/IDecoder.h"

namespace escope {

/// Classic CAN 2.0A / 2.0B decoder for one logic line (a transceiver's RXD or TXD:
/// recessive = high, dominant = low).
///
/// Per frame it emits events for SOF, the identifier (11 or 29 bit), RTR / DLC, each
/// data byte, the CRC (checked), the ACK slot and EOF. It synchronises on the SOF
/// edge, resynchronises on every edge (so a slightly wrong bit rate or clock drift is
/// tolerated), removes stuff bits and reports stuff errors, form errors, CRC errors, a
/// missing ACK, and frames cut off by the end of a capture burst. CAN FD frames are
/// recognised and flagged, not decoded. Extended IDs also get a DroneCAN breakdown
/// (priority, message type, source node, transfer tail) in the tooltip text.
///
/// Event labels (used by the waveform lane and the hex log):
///   Control:    "SOF", "EOF"
///   Address:    "ID 0x123" (standard) / "IDE 0x1801550A" (extended), value = the ID
///   Annotation: "DLC 8", "RTR DLC 2", "CRC 0x1A2B OK", "ACK", "NO ACK", "CAN FD ..."
///   Data:       "0x3E", value = the byte
///   Error:      "[STUFF ERR]", "[FORM ERR]", "[INCOMPLETE]"
class CANDecoder : public IDecoder {
public:
    std::string name()        const override { return "CAN"; }
    std::string description() const override {
        return "Classic CAN 2.0A/B decoder (single line: transceiver RXD or TXD).";
    }
    std::vector<std::string> required_channels() const override { return {"RX"}; }

    /// "bitrate" (bit/s; 0 or absent = detect it from the signal),
    /// "sample_point" (percent of the bit time, default 80).
    void configure(const std::vector<std::pair<std::string,std::string>>& params) override;

    /// The first channel in @p channels is the CAN line.
    std::vector<DecodedEvent> decode(const DigitalBuffer& buf,
                                     const std::vector<DecoderChannelMap>& channels) override;

    /// Bit rate of @p channel from its recent edges (the shortest pulse is one bit
    /// time), snapped to a standard rate within 4%; 0 if there is not enough signal.
    static uint32_t detect_bitrate(const DigitalBuffer& buf, uint8_t channel);

    /// CAN CRC-15 (polynomial 0x4599) over a bit sequence, SOF to the last data bit,
    /// destuffed. Exposed for tests.
    static uint16_t crc15(const std::vector<uint8_t>& bits);

    uint32_t bitrate_used() const { return bitrate_used_; }
    std::size_t frames()     const { return frames_; }
    std::size_t errors()     const { return errors_; }

private:
    std::vector<DecodedEvent> decode_at(const DigitalBuffer& buf, uint8_t channel, uint32_t rate);
    uint32_t    bitrate_cfg_  = 0;
    double      sample_point_ = 0.80;
    uint32_t    bitrate_used_ = 0;
    std::size_t frames_ = 0, errors_ = 0;
};

} // namespace escope
