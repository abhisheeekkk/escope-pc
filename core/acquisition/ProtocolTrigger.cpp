#include "acquisition/ProtocolTrigger.h"
#include "decoders/can/CANDecoder.h"
#include "decoders/i2c/I2CDecoder.h"
#include "decoders/spi/SPIDecoder.h"
#include "decoders/uart/UartDecoder.h"

#include <memory>

#include <cstdio>

namespace escope {

namespace {
// Built directly rather than through the decoder registry: a registry entry lives in a static
// initialiser that the linker can drop from a static library, which would silently reject every window.
std::unique_ptr<IDecoder> make_decoder(const std::string& name) {
    if (name == "I2C")  return std::make_unique<I2CDecoder>();
    if (name == "SPI")  return std::make_unique<SPIDecoder>();
    if (name == "UART") return std::make_unique<UartDecoder>();
    if (name == "CAN")  return std::make_unique<CANDecoder>();
    return nullptr;
}
uint8_t channel_of(const ProtocolTriggerConfig& c, const char* role) {
    for (const auto& m : c.channels) if (m.role == role) return m.channel;
    return 0xFF;
}
bool ends_with(const std::string& s, const char* suffix) {
    const std::size_t n = std::char_traits<char>::length(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}
}

std::string ProtocolTriggerConfig::describe() const {
    char b[96];
    switch (kind) {
    case Kind::I2CAddress:
        std::snprintf(b, sizeof b, "I2C address 0x%02X%s%s", value & 0x7F,
                      direction == Direction::Write ? " write" : direction == Direction::Read ? " read" : "",
                      ack == Ack::Ack ? " ACK" : ack == Ack::Nack ? " NACK" : "");
        return b;
    case Kind::UartByte: std::snprintf(b, sizeof b, "UART byte 0x%02X", value & 0xFF); return b;
    case Kind::CanId:    std::snprintf(b, sizeof b, extended ? "CAN id 0x%08X" : "CAN id 0x%03X", value); return b;
    case Kind::SpiByte:  std::snprintf(b, sizeof b, "SPI %s 0x%02X", spi_line.c_str(), value & 0xFF); return b;
    case Kind::AnyError: return protocol + " error";
    case Kind::Off:      break;
    }
    return "off";
}

void ProtocolTrigger::set_config(ProtocolTriggerConfig cfg) {
    uint8_t mask = 0;
    if (cfg.enabled())
        for (const auto& m : cfg.channels) if (m.channel < 8) mask |= uint8_t(1u << m.channel);
    const bool on = cfg.enabled();
    {
        std::lock_guard<std::mutex> lk(mu_);
        cfg_ = std::move(cfg);
    }
    mask_.store(mask, std::memory_order_release);
    enabled_.store(on, std::memory_order_release);
    reset_counts();
}

ProtocolTriggerConfig ProtocolTrigger::config() const {
    std::lock_guard<std::mutex> lk(mu_);
    return cfg_;
}

std::optional<ProtocolTrigger::HardwareEdge> ProtocolTrigger::hardware_edge() const {
    const ProtocolTriggerConfig c = config();
    if (!c.enabled()) return std::nullopt;
    // Every frame of these buses begins with the line going low
    uint8_t ch = 0xFF;
    if (c.protocol == "I2C")       ch = channel_of(c, "SDA");           // START = SDA falls while SCL is high
    else if (c.protocol == "UART") ch = channel_of(c, "RX") != 0xFF ? channel_of(c, "RX") : channel_of(c, "TX");   // one line can start a window; prefer RX
    else if (c.protocol == "CAN")  ch = channel_of(c, "RX");            // SOF
    else if (c.protocol == "SPI")  ch = channel_of(c, "CS");
    if (c.protocol == "SPI" && ch == 0xFF) {                            // no chip select: first clock edge
        ch = channel_of(c, "CLK");
        if (ch != 0xFF) return HardwareEdge{ch, 2};
    }
    if (ch == 0xFF || ch > 7) return std::nullopt;
    return HardwareEdge{ch, 1};
}

std::optional<double> ProtocolTrigger::find_match(const ProtocolTriggerConfig& c,
                                                  const std::vector<DecodedEvent>& events) {
    using T = DecodedEvent::Type;
    const uint8_t spi_ch = channel_of(c, c.spi_line.c_str());
    for (const auto& e : events) {
        switch (c.kind) {
        case ProtocolTriggerConfig::Kind::AnyError:
            if (e.is_error) return e.start_ns;
            break;
        case ProtocolTriggerConfig::Kind::I2CAddress: {
            if (e.type != T::Address || e.value < 0) break;
            if (uint32_t(e.value >> 1) != (c.value & 0x7F)) break;
            const bool read = (e.value & 1) != 0;
            if (c.direction == ProtocolTriggerConfig::Direction::Write && read)  break;
            if (c.direction == ProtocolTriggerConfig::Direction::Read && !read)  break;
            const bool nack = ends_with(e.label, "NACK");
            if (c.ack == ProtocolTriggerConfig::Ack::Ack && nack)  break;
            if (c.ack == ProtocolTriggerConfig::Ack::Nack && !nack) break;
            return e.start_ns;
        }
        case ProtocolTriggerConfig::Kind::UartByte:
            if (e.type == T::Data && !e.is_error && uint32_t(e.value) == (c.value & 0xFF)) return e.start_ns;
            break;
        case ProtocolTriggerConfig::Kind::CanId: {
            if (e.type != T::Address || e.value < 0 || e.is_error) break;
            const bool ext = e.label.compare(0, 3, "IDE") == 0;
            if (ext == c.extended && uint32_t(e.value) == c.value) return e.start_ns;
            break;
        }
        case ProtocolTriggerConfig::Kind::SpiByte:
            if (e.type == T::Data && !e.is_error && e.channel == spi_ch && uint32_t(e.value) == (c.value & 0xFF))
                return e.start_ns;
            break;
        case ProtocolTriggerConfig::Kind::Off:
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<double> ProtocolTrigger::evaluate(const DigitalEdge* edges, std::size_t count) {
    if (!enabled()) return std::nullopt;
    const ProtocolTriggerConfig c = config();
    auto dec = make_decoder(c.protocol);
    if (!dec || count == 0) { rejected_.fetch_add(1); return std::nullopt; }
    dec->configure(c.params);

    DigitalBuffer buf(8);
    buf.push_batch(edges, count);
    const auto events = dec->decode(buf, c.channels);
    const auto hit = find_match(c, events);
    (hit ? matched_ : rejected_).fetch_add(1);
    return hit;
}

} // namespace escope
