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
        // A burst-start snapshot is a level, not a transition: the gap to or from it
        // (snapshot to the first edge, or the unobserved gap between bursts) is not a bit
        // time. The first edge is often one sample after the snapshot in EVERY burst, which
        // would otherwise be the most common "smallest gap" and read as the sample rate.
        if (edges[i].snapshot || edges[i - 1].snapshot) continue;
        const double g = edges[i].timestamp_ns - edges[i - 1].timestamp_ns;
        if (g > 0) gaps.push_back(g);
    }
    if (gaps.size() < 3) return 0;
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

// Decoding model
// --------------
// The capture is delivered as bursts: a few ms of samples, then a gap (the device
// does not sample while a burst uploads). Each burst begins with a level snapshot
// (not a real edge). So a burst almost always starts and ends in the MIDDLE of a
// byte, and the time between bursts is unobserved. Therefore:
//   - edges are split into bursts at the snapshots and each is decoded on its own
//     (nothing is sampled across a gap);
//   - where a byte boundary falls inside a burst is not known from its first edge
//     (a data edge looks like a start bit), so the first few start-bit candidates are
//     tried and the alignment with the fewest framing errors wins;
//   - a byte cut off by the end of a burst is dropped, not decoded from the level the
//     line happened to be left at.
std::vector<DecodedEvent> UartDecoder::decode_channel(const DigitalBuffer& buf,
                                                       uint8_t ch_idx,
                                                       const std::string& role) const {
    const auto edges = buf.edges_for_channel(ch_idx);
    if (edges.empty()) return {};

    std::vector<DecodedEvent> events;
    const double bit_ns     = 1e9 / baud_;
    const bool   idle_level = !invert_;           // idle is high normally
    const int    frame_bits = 1 + data_bits_ + (two_stop_ ? 2 : 1);

    // Split into bursts: a snapshot edge starts a new one.
    std::size_t seg_begin = 0;
    for (std::size_t i = 1; i <= edges.size(); ++i) {
        if (i < edges.size() && !edges[i].snapshot) continue;
        decode_burst(edges.data() + seg_begin, i - seg_begin, ch_idx, role, bit_ns,
                     idle_level, frame_bits, events);
        seg_begin = i;
    }
    return events;
}

void UartDecoder::decode_burst(const DigitalEdge* e, std::size_t n, uint8_t ch_idx,
                               const std::string& role, double bit_ns, bool idle_level,
                               int frame_bits, std::vector<DecodedEvent>& out) const {
    if (n == 0) return;
    const double last_edge_ns = e[n - 1].timestamp_ns;

    // Line level inside this burst only: the snapshot (or first edge) gives the level at its start.
    auto level_at = [&](double t) {
        std::size_t lo = 0, hi = n;                // last edge with timestamp <= t
        while (lo < hi) {
            const std::size_t mid = (lo + hi) / 2;
            if (e[mid].timestamp_ns <= t) lo = mid + 1; else hi = mid;
        }
        bool level = lo == 0 ? idle_level : e[lo - 1].rising;
        return invert_ ? !level : level;
    };
    auto is_start_edge = [&](std::size_t i) {
        return !e[i].snapshot && (e[i].rising ? !idle_level : idle_level);
    };

    struct Frame { double start; double end; uint8_t val; bool err; };

    // Decode frames one after another beginning at start edge @p first. A frame is only
    // trusted if the burst shows the line at or after its stop bit (the stop-bit rising
    // edge or the next start bit); otherwise the burst ended inside it.
    auto run = [&](std::size_t first, std::vector<Frame>& frames) {
        std::size_t i = first;
        while (i < n) {
            if (!is_start_edge(i)) { ++i; continue; }
            const double fs = e[i].timestamp_ns;
            double t = fs + bit_ns * 1.5;           // middle of bit 0
            uint8_t v = 0;
            for (int b = 0; b < data_bits_; ++b, t += bit_ns)
                if (level_at(t)) v |= static_cast<uint8_t>(1u << b);
            const double stop_boundary = fs + bit_ns * (1 + data_bits_);
            if (last_edge_ns < stop_boundary - bit_ns * 0.1) break;     // cut off by the end of the burst
            frames.push_back({fs, t + bit_ns, v, !level_at(t)});
            // The next start bit may follow the stop bit immediately (and rounding or
            // baud drift can put it a hair early): resume looking from the stop-bit sample point.
            const double frame_end = fs + bit_ns * (frame_bits - 0.5);
            while (i < n && e[i].timestamp_ns < frame_end) ++i;
        }
    };

    // Pick the alignment: the first few start-bit candidates, fewest framing errors wins
    // (an aligned run has none; a misaligned one hits stop-bit errors quickly).
    std::vector<Frame> best;
    std::size_t best_err = static_cast<std::size_t>(-1);
    int tried = 0;
    for (std::size_t i = 0; i < n && tried < 12; ++i) {
        if (!is_start_edge(i)) continue;
        ++tried;
        std::vector<Frame> cand;
        run(i, cand);
        std::size_t err = 0;
        for (const auto& f : cand) err += f.err;
        // Prefer fewer errors; on a tie the earlier candidate (more frames) wins.
        if (err < best_err || (err == best_err && cand.size() > best.size())) {
            best = std::move(cand);
            best_err = err;
        }
        if (best_err == 0) break;
    }

    for (const auto& f : best) {
        DecodedEvent ev;
        ev.start_ns = f.start;
        ev.channel  = ch_idx;
        if (f.err) {
            ev.end_ns   = f.end;
            ev.label    = "[FRAME ERR]";
            ev.type     = DecodedEvent::Type::Error;
            ev.is_error = true;
        } else {
            ev.end_ns = f.end - bit_ns + bit_ns * (two_stop_ ? 2.0 : 1.0);
            ev.type   = DecodedEvent::Type::Data;
            ev.value  = f.val;
            std::ostringstream oss;
            oss << role << " 0x" << std::uppercase << std::hex
                << std::setw(2) << std::setfill('0') << (int)f.val;
            if (std::isprint(f.val)) oss << " '" << (char)f.val << "'";
            ev.label  = oss.str();
            ev.detail = "Baud: " + std::to_string(baud_) + "  Value: 0x" + oss.str();
        }
        out.push_back(std::move(ev));
    }
}

} // namespace escope
