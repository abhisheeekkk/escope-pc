#pragma once

#include "acquisition/DigitalBuffer.h"
#include <string>
#include <vector>
#include <memory>
#include <cstdint>

namespace escope {

/// One decoded protocol event (a frame, packet, byte, etc.).
struct DecodedEvent {
    double      start_ns;     ///< Start time on the shared time axis
    double      end_ns;       ///< End time
    std::string label;        ///< Human-readable value ("0x68 WRITE", "ACK", etc.)
    std::string detail;       ///< Extended info for tooltip/detail panel
    uint8_t     channel;      ///< Source channel index

    enum class Type : uint8_t {
        Data,
        Address,
        Control,
        Error,
        Annotation,
    } type = Type::Data;

    bool is_error = false;
    int  value    = -1;       ///< Decoded byte value for data frames, -1 if n/a
};

/// Channel assignment for a decoder.
struct DecoderChannelMap {
    std::string role;    ///< "CLK", "MOSI", "MISO", "CS", "TX", "RX", "SCL", "SDA", etc.
    uint8_t     channel; ///< Digital channel index
};

/// Base interface for all protocol decoders.
///
/// Decoders consume DigitalEdge streams and produce DecodedEvent lists.
/// They are stateless between calls to decode() — all state resets each call.
class IDecoder {
public:
    virtual ~IDecoder() = default;

    virtual std::string name()        const = 0;
    virtual std::string description() const = 0;

    /// Channel roles this decoder requires (e.g. UART: "TX", "RX").
    virtual std::vector<std::string> required_channels() const = 0;

    /// Configure decoder parameters (baud rate, bit order, etc.).
    /// @param params  Key-value pairs, e.g. {{"baud", "115200"}, {"bits", "8"}}.
    virtual void configure(const std::vector<std::pair<std::string,std::string>>& params) {}

    /// Decode edges from @p buf using the assigned channel map.
    /// Returns decoded events sorted by start_ns.
    virtual std::vector<DecodedEvent> decode(
        const DigitalBuffer& buf,
        const std::vector<DecoderChannelMap>& channels) = 0;
};

} // namespace escope
