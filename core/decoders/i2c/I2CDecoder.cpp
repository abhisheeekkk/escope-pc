#include "decoders/i2c/I2CDecoder.h"
#include "decoders/base/DecoderRegistry.h"
#include <algorithm>
#include <cstdio>

namespace escope {
REGISTER_DECODER(I2CDecoder);

std::vector<std::string> I2CDecoder::required_channels() const {
    return {"SCL", "SDA"};
}

namespace {
std::string hex2(unsigned v) {
    char b[8];
    std::snprintf(b, sizeof b, "%02X", v & 0xFF);
    return b;
}
}

void I2CDecoder::configure(const std::vector<std::pair<std::string,std::string>>& params) {
    for (const auto& [k, v] : params)
        if (k == "glitch_ns") glitch_ns_ = std::stod(v);
}

namespace {
// Remove pulses shorter than min_ns (a rise and fall, or fall and rise, closer
// together than any legal I2C phase): a spike on SCL would otherwise count as an
// extra clock and shift every following bit. Burst-start level snapshots are
// kept as they are. Returns how many pulses were removed.
std::size_t drop_glitches(std::vector<DigitalEdge>& e, double min_ns) {
    if (min_ns <= 0) return 0;
    std::vector<DigitalEdge> out;
    out.reserve(e.size());
    std::size_t removed = 0;
    for (const auto& x : e) {
        if (!x.snapshot && out.size() >= 2 && !out.back().snapshot &&
            x.rising != out.back().rising &&
            x.timestamp_ns - out.back().timestamp_ns < min_ns) {
            out.pop_back();               // the short pulse's first edge ...
            ++removed;                    // ... and this edge (not stored) cancel out
            continue;
        }
        out.push_back(x);
    }
    e.swap(out);
    return removed;
}
}

// Decoding model
// --------------
// "Xfer" mode follows a real START: the first byte is the address, the rest
// are data. A capture source delivers bursts that usually begin in the MIDDLE
// of a transfer, so the stream also has a "headless" mode (start of history,
// start of every burst, after a STOP): bits are collected on each rising SCL
// without knowing where byte boundaries are. When that mode ends (real START,
// STOP, next burst, end of data) the alignment is recovered from the ACK
// slots: every 9th bit is the ACK (SDA low), and only the true alignment puts
// all of those slots on low bits.
std::vector<DecodedEvent> I2CDecoder::decode(const DigitalBuffer& buf,
                                             const std::vector<DecoderChannelMap>& channels) {
    int scl_ch = -1, sda_ch = -1;
    for (const auto& c : channels) {
        if (c.role == "SCL") scl_ch = c.channel;
        else if (c.role == "SDA") sda_ch = c.channel;
    }
    if (scl_ch < 0 || sda_ch < 0 || scl_ch == sda_ch) return {};

    auto scl_e = buf.edges_for_channel(static_cast<uint8_t>(scl_ch));
    auto sda_e = buf.edges_for_channel(static_cast<uint8_t>(sda_ch));
    if (scl_e.empty() || sda_e.empty()) return {};
    glitches_ = drop_glitches(scl_e, glitch_ns_) + drop_glitches(sda_e, glitch_ns_);

    std::vector<DecodedEvent> events;
    auto push = [&](double t0, double t1, DecodedEvent::Type type, std::string label,
                    std::string detail, bool err, int value) {
        DecodedEvent e;
        e.start_ns = t0;
        e.end_ns   = t1;
        e.label    = std::move(label);
        e.detail   = std::move(detail);
        e.channel  = static_cast<uint8_t>(sda_ch);
        e.type     = type;
        e.is_error = err;
        e.value    = value;
        events.push_back(std::move(e));
    };

    // Each channel's history is capped independently (oldest edges dropped),
    // and a fast clock like SCL hits that cap long before SDA does. Start
    // decoding only where BOTH lines have history, with the levels in force
    // at that point.
    const double t_begin = std::max(scl_e.front().timestamp_ns, sda_e.front().timestamp_ns);
    auto first_after = [t_begin](const std::vector<DigitalEdge>& e) {
        return static_cast<std::size_t>(std::upper_bound(e.begin(), e.end(), t_begin,
            [](double t, const DigitalEdge& x) { return t < x.timestamp_ns; }) - e.begin());
    };
    std::size_t si = first_after(scl_e);   // >= 1: front edge is at or before t_begin
    std::size_t di = first_after(sda_e);
    bool scl = scl_e[si - 1].rising;
    bool sda = sda_e[di - 1].rising;

    bool     in_xfer   = false;   // Xfer mode (after a real START) vs headless
    bool     have_addr = false;
    int      nbits     = 0;
    unsigned byte      = 0;
    double   byte_t0   = 0;

    std::vector<uint8_t> hbits;   // headless: SDA sampled on each rising SCL
    std::vector<double>  hts;     //           and when

    // Recover byte alignment of the collected headless bits from the ACK slots
    // and emit them: a "~" marker (alignment was inferred) then the bytes.
    auto flush_headless = [&]() {
        const std::size_t n = hbits.size();
        if (n >= 18) {
            // Score each candidate alignment j (index of the first ACK slot) by
            // the fraction of its slots that are low. Real captures show
            // spurious NACKs, so a perfect score is not required: the best
            // offset must be mostly low and clearly beat the runner-up.
            double best = -1, second = -1;
            int best_j = -1;
            for (std::size_t j = 0; j < 9; ++j) {
                std::size_t zeros = 0, total = 0;
                for (std::size_t i = j; i < n; i += 9) { ++total; zeros += hbits[i] ? 0 : 1; }
                if (total < 3) continue;
                const double f = static_cast<double>(zeros) / static_cast<double>(total);
                if (f > best) { second = best; best = f; best_j = static_cast<int>(j); }
                else if (f > second) second = f;
            }
            if (best < 0.6 || best - second < 0.15) best_j = -1;   // only an unambiguous alignment is trusted

            if (best_j >= 0) {
                const std::size_t base0 = (static_cast<std::size_t>(best_j) + 1) % 9;
                bool marked = false;
                for (std::size_t base = base0; base + 8 < n; base += 9) {
                    unsigned v = 0;
                    for (int b = 0; b < 8; ++b) v = (v << 1) | (hbits[base + b] ? 1u : 0u);
                    const bool ack = !hbits[base + 8];
                    if (!marked) {
                        push(hts[base], hts[base], DecodedEvent::Type::Control, "~",
                             "Burst began mid-transfer: byte alignment inferred from ACK slots",
                             false, -1);
                        marked = true;
                    }
                    push(hts[base], hts[base + 8], DecodedEvent::Type::Data,
                         "0x" + hex2(v) + (ack ? " ACK" : " NACK"),
                         "Data byte 0x" + hex2(v) + " (alignment inferred)", false, static_cast<int>(v));
                }
            } else {
                push(hts.front(), hts.back(), DecodedEvent::Type::Error, "[NO SYNC]",
                     "Could not infer byte alignment from " + std::to_string(n) + " clocks", true, -1);
            }
        }
        hbits.clear();
        hts.clear();
    };

    auto abort_partial = [&](double t) {
        // A single stray clock is the SCL rise that precedes a STOP / repeated
        // START (SDA still held), not the start of a real byte.
        if (in_xfer && nbits > 1)
            push(byte_t0, t, DecodedEvent::Type::Error, "[INCOMPLETE]",
                 "Transfer ended mid-byte after " + std::to_string(nbits) + " bits", true, -1);
        nbits = 0;
        byte  = 0;
    };

    double last_snap_t = -1;

    while (si < scl_e.size() || di < sda_e.size()) {
        // Merge the two sorted streams. At 48 MS/s the SDA hold time after a
        // falling SCL is often under one sample, so the two edges can carry
        // the SAME timestamp. Order such ties the way a real bus behaves:
        // SCL falling first (data changes after the clock falls), SDA first
        // before SCL rises (data is set up before the clock rises). The wrong
        // order sees an SDA change while SCL is still high, i.e. a false
        // START/STOP, and shifts every following bit.
        const bool take_sda = di < sda_e.size() &&
            (si >= scl_e.size() ||
             sda_e[di].timestamp_ns <  scl_e[si].timestamp_ns ||
             (sda_e[di].timestamp_ns == scl_e[si].timestamp_ns && scl_e[si].rising));
        const DigitalEdge& e = take_sda ? sda_e[di++] : scl_e[si++];
        const double t   = e.timestamp_ns;
        const bool   lvl = e.rising;

        // Burst-start level snapshot: records the level, not a transition, and
        // the bus was unobserved since the previous burst. Whatever was in
        // progress ends here; the new burst almost always begins mid-transfer.
        if (e.snapshot) {
            if (t != last_snap_t) {
                last_snap_t = t;
                if (in_xfer) abort_partial(t); else flush_headless();
                in_xfer = false; have_addr = false; nbits = 0; byte = 0;
            }
            (take_sda ? sda : scl) = lvl;
            continue;
        }

        if (take_sda) {
            if (lvl == sda) continue;
            sda = lvl;
            if (!scl) continue;                       // data change while clock low
            if (!sda) {                               // SDA fell with SCL high: START
                const bool repeated = in_xfer;
                if (in_xfer) abort_partial(t); else flush_headless();
                in_xfer = true; have_addr = false; nbits = 0; byte = 0;
                push(t, t, DecodedEvent::Type::Control, repeated ? "Sr" : "START",
                     repeated ? "Repeated START" : "START condition", false, -1);
            } else {                                  // SDA rose with SCL high: STOP
                const bool had_bits = !hbits.empty();
                if (in_xfer) abort_partial(t); else flush_headless();
                if (in_xfer || had_bits)
                    push(t, t, DecodedEvent::Type::Control, "STOP", "STOP condition", false, -1);
                in_xfer = false; have_addr = false; nbits = 0; byte = 0;
            }
        } else {
            if (lvl == scl) continue;
            scl = lvl;
            if (!scl) continue;                       // only sample on rising SCL

            if (!in_xfer) {                           // headless: just collect the bit
                hbits.push_back(sda ? 1 : 0);
                hts.push_back(t);
                continue;
            }

            if (nbits == 0) byte_t0 = t;
            if (nbits < 8) {
                byte = (byte << 1) | (sda ? 1u : 0u);
                ++nbits;
            } else {                                  // 9th clock: ACK (SDA low) / NACK
                const bool ack = !sda;
                if (!have_addr) {
                    have_addr = true;
                    const unsigned addr = byte >> 1;
                    const bool read = byte & 1u;
                    push(byte_t0, t, DecodedEvent::Type::Address,
                         "ADDR 0x" + hex2(addr) + (read ? " R " : " W ") + (ack ? "ACK" : "NACK"),
                         "Address byte 0x" + hex2(byte) + " (7-bit 0x" + hex2(addr) + ", " +
                             (read ? "read" : "write") + ")", !ack, static_cast<int>(byte));
                } else {
                    push(byte_t0, t, DecodedEvent::Type::Data,
                         "0x" + hex2(byte) + (ack ? " ACK" : " NACK"),
                         "Data byte 0x" + hex2(byte), false, static_cast<int>(byte));
                }
                nbits = 0;
                byte  = 0;
            }
        }
    }
    if (!in_xfer) flush_headless();

    std::sort(events.begin(), events.end(),
        [](const DecodedEvent& a, const DecodedEvent& b) { return a.start_ns < b.start_ns; });
    return events;
}

} // namespace escope
