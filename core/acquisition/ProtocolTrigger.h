#pragma once

#include "acquisition/DigitalBuffer.h"
#include "decoders/base/IDecoder.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace escope {

/// What a protocol trigger waits for.
struct ProtocolTriggerConfig {
    enum class Kind : uint8_t {
        Off,
        I2CAddress,   ///< a transaction to / from a 7-bit address (optionally only writes, reads, ACK or NACK)
        UartByte,     ///< a received byte
        CanId,        ///< a CAN frame with this identifier (11 or 29 bit)
        SpiByte,      ///< a word on MOSI or MISO
        AnyError,     ///< any frame the decoder flags as damaged (framing, CRC, ...)
    };
    enum class Direction : uint8_t { Any, Write, Read };
    enum class Ack       : uint8_t { Any, Ack, Nack };

    Kind        kind = Kind::Off;
    std::string protocol;                                   ///< decoder name: I2C, UART, CAN or SPI
    std::vector<DecoderChannelMap>                    channels;   ///< pins, as in the protocol panel
    std::vector<std::pair<std::string, std::string>>  params;     ///< decoder settings (baud, bitrate, ...)

    uint32_t  value      = 0;        ///< address, byte or identifier
    bool      extended   = false;    ///< CAN: 29-bit identifier
    Direction direction  = Direction::Any;   ///< I2C
    Ack       ack        = Ack::Any;         ///< I2C
    std::string spi_line = "MOSI";   ///< SPI: which data line carries the word

    bool enabled() const { return kind != Kind::Off && !channels.empty(); }

    /// One line for the status bar, e.g.  I2C address 0x50 write
    std::string describe() const;
};

/// Matches decoded protocol frames inside one captured burst.
///
/// The device can only trigger on an edge, so it hands over a window around every edge of the bus's
/// start line (see hardware_edge()). Each window is decoded here and kept only when it holds the frame
/// being waited for; the others are dropped before they reach the display. The match time becomes the
/// trigger time, so the view lines the frame up like any other trigger.
///
/// The configuration is set from the GUI thread and read from the capture thread.
class ProtocolTrigger {
public:
    void set_config(ProtocolTriggerConfig cfg);
    ProtocolTriggerConfig config() const;
    bool enabled() const { return enabled_.load(std::memory_order_acquire); }

    /// Channels the trigger needs recorded (bit n = Dn), 0 when it is off.
    uint8_t required_mask() const { return mask_.load(std::memory_order_acquire); }

    /// Edge the device should trigger on so each frame of this bus starts a window:
    /// the line, and 0 = rising / 1 = falling / 2 = either. nullopt when the trigger is off.
    struct HardwareEdge { uint8_t channel; uint8_t mode; };
    std::optional<HardwareEdge> hardware_edge() const;

    /// Look for the frame in one burst (edges sorted per channel, as the capture produces them: a
    /// snapshot of every channel first, then the changes). Returns the start time of the frame, or
    /// nullopt when the burst holds none. Counts the burst as matched or rejected.
    std::optional<double> evaluate(const DigitalEdge* edges, std::size_t count);

    /// Same, for frames already decoded (used by the tests and by evaluate()).
    static std::optional<double> find_match(const ProtocolTriggerConfig& cfg,
                                            const std::vector<DecodedEvent>& events);

    uint64_t matched()  const { return matched_.load(); }
    uint64_t rejected() const { return rejected_.load(); }
    void     reset_counts()   { matched_ = 0; rejected_ = 0; }

private:
    mutable std::mutex     mu_;
    ProtocolTriggerConfig  cfg_;
    std::atomic<bool>      enabled_{false};
    std::atomic<uint8_t>   mask_{0};
    std::atomic<uint64_t>  matched_{0}, rejected_{0};
};

} // namespace escope
