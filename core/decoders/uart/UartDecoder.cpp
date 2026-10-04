#include "decoders/uart/UartDecoder.h"
#include "decoders/base/DecoderRegistry.h"
#include <algorithm>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <cctype>

namespace escope {

REGISTER_DECODER(UartDecoder);

void UartDecoder::configure(const std::vector<std::pair<std::string,std::string>>& params) {
    for (const auto& [k, v] : params) {
        if (k == "baud")      baud_      = std::stoul(v);
        else if (k == "bits") data_bits_ = static_cast<uint8_t>(std::stoul(v));
        else if (k == "parity") parity_  = (v != "none" && v != "0");
        else if (k == "stop2")  two_stop_ = (v == "1" || v == "true");
        else if (k == "invert") invert_   = (v == "1" || v == "true");
    }
}

uint32_t UartDecoder::detect_baud(const DigitalBuffer& buf, uint8_t channel) {
    const auto edges = buf.last_edges(channel, 2000);
    if (edges.size() < 8) return 0;

    std::vector<double> gaps;
    gaps.reserve(edges.size());
    for (std::size_t i = 1; i < edges.size(); ++i) {
        const double g = edges[i].timestamp_ns - edges[i - 1].timestamp_ns;
        if (g > 0) gaps.push_back(g);
    }
    std::sort(gaps.begin(), gaps.end());

    // Smallest gap value that at least 3 gaps agree on (within 10%), so a
    // single glitch pulse can't be mistaken for the bit time.
    double bit_ns = 0;
    for (std::size_t i = 0; i + 2 < gaps.size(); ++i) {
        if (gaps[i + 2] <= gaps[i] * 1.10) {
            std::size_t j = i;
            double sum = 0;
            while (j < gaps.size() && gaps[j] <= gaps[i] * 1.10) sum += gaps[j++];
            bit_ns = sum / static_cast<double>(j - i);
            break;
        }
    }
    if (bit_ns <= 0) return 0;

    const double raw = 1e9 / bit_ns;
    static const uint32_t standard[] = {1200, 2400, 4800, 9600, 14400, 19200, 28800,
        38400, 57600, 76800, 115200, 230400, 460800, 500000, 921600, 1000000, 1500000,
        2000000, 3000000};
    for (uint32_t b : standard)
        if (std::abs(raw - b) <= b * 0.04) return b;
    return static_cast<uint32_t>(raw + 0.5);
}

std::vector<DecodedEvent> UartDecoder::decode(const DigitalBuffer& buf,
                                               const std::vector<DecoderChannelMap>& channels) {
    std::vector<DecodedEvent> events;
    for (const auto& ch_map : channels) {
        auto ch_events = decode_channel(buf, ch_map.channel, ch_map.role);
        events.insert(events.end(), ch_events.begin(), ch_events.end());
    }
    std::sort(events.begin(), events.end(),
        [](const DecodedEvent& a, const DecodedEvent& b){
            return a.start_ns < b.start_ns;
        });
    return events;
}

std::vector<DecodedEvent> UartDecoder::decode_channel(const DigitalBuffer& buf,
                                                       uint8_t ch_idx,
                                                       const std::string& role) const {
    auto edges = buf.edges_for_channel(ch_idx);
    if (edges.empty()) return {};

    std::vector<DecodedEvent> events;
    const double bit_ns = 1e9 / baud_;  // nanoseconds per bit

    // UART idle = high (or low if inverted)
    // Start bit = falling edge (or rising if inverted)
    // We scan for start-bit edges and decode full frames from there.

    const bool idle_level = !invert_;  // idle is high normally

    std::size_t ei = 0;
    while (ei < edges.size()) {
        const auto& edge = edges[ei];

        // Look for a start bit (edge from idle to active)
        bool is_start = edge.rising ? !idle_level : idle_level;
        if (!is_start) { ++ei; continue; }

        double frame_start = edge.timestamp_ns;
        // Sample each bit at bit_ns/2 into the bit window
        double t = frame_start + bit_ns * 1.5;  // middle of bit 0

        uint8_t byte_val = 0;


        for (int bit = 0; bit < data_bits_; ++bit) {
            bool level = buf.level_at(ch_idx, t);
            if (invert_) level = !level;
            if (level) byte_val |= (1 << bit);
            t += bit_ns;
        }

        // Check stop bit
        bool stop_level = buf.level_at(ch_idx, t);
        if (invert_) stop_level = !stop_level;
        if (!stop_level) {
            // Framing error
            DecodedEvent err;
            err.start_ns = frame_start;
            err.end_ns   = t + bit_ns;
            err.label    = "[FRAME ERR]";
            err.type     = DecodedEvent::Type::Error;
            err.is_error = true;
            err.channel  = ch_idx;
            events.push_back(err);
        } else {
            DecodedEvent evt;
            evt.start_ns = frame_start;
            evt.end_ns   = t + bit_ns * (two_stop_ ? 2.0 : 1.0);
            evt.channel  = ch_idx;
            evt.type     = DecodedEvent::Type::Data;
            evt.value    = byte_val;

            // Label: hex + ASCII if printable
            std::ostringstream oss;
            oss << role << " 0x" << std::uppercase << std::hex
                << std::setw(2) << std::setfill('0') << (int)byte_val;
            if (std::isprint(byte_val)) {
                oss << " '" << (char)byte_val << "'";
            }
            evt.label  = oss.str();
            evt.detail = "Baud: " + std::to_string(baud_)
                       + "  Value: 0x" + oss.str();
            events.push_back(evt);
        }

        // Advance to after this frame
        double frame_end = frame_start + bit_ns * (1 + data_bits_ + (two_stop_ ? 2 : 1));
        while (ei < edges.size() && edges[ei].timestamp_ns < frame_end) ++ei;
    }

    return events;
}

} // namespace escope
