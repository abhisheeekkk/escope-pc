#include "decoders/spi/SPIDecoder.h"
#include "decoders/base/DecoderRegistry.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace escope {
REGISTER_DECODER(SPIDecoder);

namespace {

/// One logic line: its edges (real transitions and per-burst level snapshots).
struct Line {
    std::vector<DigitalEdge> e;
    bool present = false;

    /// Level just before @p t: 1 or 0, or -1 if nothing is known yet.
    int level_before(double t) const {
        auto it = std::lower_bound(e.begin(), e.end(), t,
            [](const DigitalEdge& x, double v) { return x.timestamp_ns < v; });
        return it == e.begin() ? -1 : ((it - 1)->rising ? 1 : 0);
    }
};

Line load(const DigitalBuffer& buf, const std::vector<DecoderChannelMap>& ch, const char* role) {
    Line l;
    for (const auto& c : ch)
        if (c.role == role) { l.e = buf.edges_for_channel(c.channel); l.present = true; break; }
    return l;
}

int channel_of(const std::vector<DecoderChannelMap>& ch, const char* role) {
    for (const auto& c : ch) if (c.role == role) return c.channel;
    return -1;
}

/// The level the line spends most of its time at (its idle level), from the stretches between
/// consecutive observed edges of a burst.
int idle_level(const Line& l) {
    double hi = 0, lo = 0;
    for (std::size_t i = 0; i + 1 < l.e.size(); ++i) {
        if (l.e[i + 1].snapshot) continue;                 // the gap to the next burst was not observed
        (l.e[i].rising ? hi : lo) += l.e[i + 1].timestamp_ns - l.e[i].timestamp_ns;
    }
    return hi > lo ? 1 : 0;
}

double percentile(std::vector<double> v, double q) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[static_cast<std::size_t>(q * static_cast<double>(v.size() - 1))];
}

struct Txn {
    double t0 = 0, t1 = 0;                 // chip select window (or first/last clock edge)
    bool   has_cs = false;
    bool   began_mid = false;              // already active when the burst started
    bool   cut = false;                    // the burst ended before chip select went inactive
    std::vector<double> edges;             // sampling clock edges
};

std::string hexw(unsigned v, int bits) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%0*X", (bits + 3) / 4, v);
    return b;
}

std::string binw(unsigned v, int bits) {
    std::string s;
    for (int i = bits - 1; i >= 0; --i) s += ((v >> i) & 1) ? '1' : '0';
    return s;
}

} // namespace

void SPIDecoder::configure(const std::vector<std::pair<std::string,std::string>>& params) {
    for (const auto& [k, v] : params) {
        if (k == "mode")           mode_cfg_ = (v == "auto") ? -1 : std::clamp(std::stoi(v), 0, 3);
        else if (k == "bits")      bits_ = std::clamp(std::stoi(v), 1, 16);
        else if (k == "lsb")       lsb_ = (v == "1" || v == "true");
        else if (k == "dc_active") dc_cmd_high_ = (v == "high");
        else if (k == "cs_active") cs_high_ = (v == "high");
    }
}

std::vector<DecodedEvent> SPIDecoder::decode(const DigitalBuffer& buf,
                                             const std::vector<DecoderChannelMap>& channels) {
    words_ = errors_ = 0;
    clock_hz_ = 0;
    mode_assumed_ = false;
    std::vector<DecodedEvent> out;

    Line clk = load(buf, channels, "CLK"), mosi = load(buf, channels, "MOSI"),
         miso = load(buf, channels, "MISO"), cs = load(buf, channels, "CS"), dc = load(buf, channels, "DC");
    if (!clk.present || (!mosi.present && !miso.present) || clk.e.size() < 4) return out;
    const int ch_mosi = channel_of(channels, "MOSI"), ch_miso = channel_of(channels, "MISO");

    // ---- clock polarity, then the clock edges --------------------------------------------
    const int cs_active = cs_high_ ? 1 : 0;
    int cpol;
    if (mode_cfg_ >= 0) {
        cpol = (mode_cfg_ >> 1) & 1;
    } else if (cs.present) {
        // the clock must be idle when chip select goes active: vote over those moments
        int idle_hi = 0, idle_lo = 0;
        for (std::size_t i = 1; i < cs.e.size(); ++i) {
            if (cs.e[i].snapshot || (cs.e[i].rising ? 1 : 0) != cs_active) continue;
            const int lv = clk.level_before(cs.e[i].timestamp_ns);
            if (lv == 1) ++idle_hi; else if (lv == 0) ++idle_lo;
        }
        cpol = (idle_hi + idle_lo) ? (idle_hi > idle_lo ? 1 : 0) : idle_level(clk);
    } else {
        cpol = idle_level(clk);
    }
    std::vector<double> snaps;                           // burst starts as seen on the clock line
    std::vector<DigitalEdge> clk_edges;                  // real transitions only
    for (const auto& x : clk.e) {
        if (x.snapshot) snaps.push_back(x.timestamp_ns);
        else clk_edges.push_back(x);
    }
    auto seg_of = [&](double t) {
        return static_cast<std::size_t>(std::upper_bound(snaps.begin(), snaps.end(), t) - snaps.begin());
    };

    // half period estimate: the short intervals between consecutive clock edges
    auto estimate_half = [&]() {
        std::vector<double> gaps;
        for (std::size_t i = 1; i < clk_edges.size(); ++i)
            if (seg_of(clk_edges[i].timestamp_ns) == seg_of(clk_edges[i - 1].timestamp_ns))
                gaps.push_back(clk_edges[i].timestamp_ns - clk_edges[i - 1].timestamp_ns);
        return gaps.empty() ? 0.0 : percentile(gaps, 0.25);
    };
    double half = estimate_half();
    if (half <= 0) return out;

    // Drop clock pulses shorter than a quarter of a half period: crosstalk or ringing on the
    // clock wire, which would otherwise be counted as extra bits and shift every byte after it.
    {
        std::vector<DigitalEdge> kept;
        kept.reserve(clk_edges.size());
        for (const auto& x : clk_edges) {
            if (!kept.empty() && x.rising != kept.back().rising &&
                x.timestamp_ns - kept.back().timestamp_ns < 0.25 * half &&
                seg_of(x.timestamp_ns) == seg_of(kept.back().timestamp_ns)) {
                kept.pop_back();                          // the glitch pulse: both of its edges go
                continue;
            }
            kept.push_back(x);
        }
        clk_edges.swap(kept);
        half = estimate_half();
        if (half <= 0) return out;
    }
    clock_hz_ = 1e9 / (2.0 * half);

    // ---- clock phase: data changes follow the shifting edge, data is read on the other ----
    int cpha;
    if (mode_cfg_ >= 0) {
        cpha = mode_cfg_ & 1;
    } else {
        long after_rising = 0, after_falling = 0;
        for (const Line* d : {&mosi, &miso}) {
            if (!d->present) continue;
            for (const auto& x : d->e) {
                if (x.snapshot) continue;
                auto it = std::upper_bound(clk_edges.begin(), clk_edges.end(), x.timestamp_ns,
                    [](double v, const DigitalEdge& c) { return v < c.timestamp_ns; });
                if (it == clk_edges.begin()) continue;
                const DigitalEdge& c = *(it - 1);
                if (x.timestamp_ns - c.timestamp_ns > 0.75 * half) continue;   // not caused by this edge
                (c.rising ? after_rising : after_falling)++;
            }
        }
        if (after_rising == after_falling) { cpha = 0; mode_assumed_ = true; }
        else {
            const bool sample_rising = after_falling > after_rising;
            cpha = (sample_rising == (cpol == 0)) ? 0 : 1;      // leading edge is rising when CPOL = 0
        }
    }
    mode_used_ = (cpol << 1) | cpha;
    const bool sample_rising = (cpol == cpha);

    std::vector<double> samples_t;                       // sampling edges
    for (const auto& c : clk_edges) if (c.rising == sample_rising) samples_t.push_back(c.timestamp_ns);
    if (samples_t.empty()) return out;
    const double period = 2.0 * half;

    // ---- transactions -----------------------------------------------------------------
    std::vector<Txn> txns;
    if (cs.present) {
        const int active = cs_active;
        Txn cur;
        bool in = false;
        auto close = [&](double t, bool cut) {
            if (!in) return;
            cur.t1 = t; cur.cut = cut;
            txns.push_back(std::move(cur));
            cur = Txn{};
            in = false;
        };
        for (const auto& x : cs.e) {
            const bool is_active = (x.rising ? 1 : 0) == active;
            if (x.snapshot) {
                close(x.timestamp_ns, true);             // the previous burst ended inside the window
                if (is_active) { cur.t0 = x.timestamp_ns; cur.has_cs = true; cur.began_mid = true; in = true; }
            } else if (is_active && !in) {
                cur.t0 = x.timestamp_ns; cur.has_cs = true; in = true;
            } else if (!is_active && in) {
                close(x.timestamp_ns, false);
            }
        }
        if (in) { cur.t1 = clk_edges.back().timestamp_ns; cur.cut = true; txns.push_back(std::move(cur)); }
        for (auto& t : txns) {
            auto a = std::lower_bound(samples_t.begin(), samples_t.end(), t.t0);
            auto b = std::lower_bound(samples_t.begin(), samples_t.end(), t.t1);
            t.edges.assign(a, b);
        }
    } else {
        const double gap_limit = 16.0 * period;
        Txn cur;
        for (std::size_t i = 0; i < samples_t.size(); ++i) {
            // A pause of more than a few bit times while a word is incomplete means the master
            // stopped mid-word, so a clock edge was miscounted: start again after the pause
            // instead of carrying the wrong bit alignment on.
            const bool stalled = !cur.edges.empty() && samples_t[i] - cur.edges.back() > 3.0 * period &&
                                 cur.edges.size() % static_cast<std::size_t>(bits_) != 0;
            const bool split = !cur.edges.empty() &&
                (stalled || samples_t[i] - cur.edges.back() > gap_limit ||
                 seg_of(samples_t[i]) != seg_of(cur.edges.back()));
            if (split) { cur.t1 = cur.edges.back(); txns.push_back(std::move(cur)); cur = Txn{}; }
            if (cur.edges.empty()) cur.t0 = samples_t[i];
            cur.edges.push_back(samples_t[i]);
        }
        if (!cur.edges.empty()) { cur.t1 = cur.edges.back(); txns.push_back(std::move(cur)); }
    }

    // ---- words ------------------------------------------------------------------------
    using T = DecodedEvent::Type;
    auto emit = [&](double t0, double t1, T type, const std::string& label, const std::string& detail,
                    int ch, int value, bool err) {
        DecodedEvent e{};
        e.start_ns = t0; e.end_ns = t1; e.type = type; e.label = label; e.detail = detail;
        e.channel = static_cast<uint8_t>(ch); e.value = value; e.is_error = err;
        out.push_back(std::move(e));
    };
    auto markers = [&](const Txn& t, bool start) {
        for (int ch : {ch_mosi, ch_miso}) {
            if (ch < 0) continue;
            if (start && t.began_mid)
                emit(t.t0, t.t0, T::Control, "~", "Capture began inside a chip-select window; the word alignment is assumed", ch, -1, false);
            else if (start) emit(t.t0, t.t0, T::Control, "CS", "Chip select asserted", ch, -1, false);
            else if (!t.cut) emit(t.t1, t.t1, T::Control, "/CS", "Chip select released", ch, -1, false);
        }
    };

    for (const Txn& t : txns) {
        if (t.has_cs) markers(t, true);
        const std::size_t n_edges = t.edges.size();
        std::size_t i = 0;
        while (i < n_edges) {
            const std::size_t left = n_edges - i;
            const int nbits = static_cast<int>(std::min<std::size_t>(left, static_cast<std::size_t>(bits_)));
            const double t_first = t.edges[i], t_last = t.edges[i + static_cast<std::size_t>(nbits) - 1];
            const double w = nbits > 1 ? (t_last - t_first) / (nbits - 1) : period;   // bit time in this word
            const double b0 = t_first - 0.5 * w, b1 = t_last + 0.5 * w;

            int dcl = -1;
            if (dc.present) dcl = dc.level_before(t_first - 0.25 * w);
            const bool command = dc.present && dcl >= 0 && ((dcl == 1) == dc_cmd_high_);

            for (const Line* d : {&mosi, &miso}) {
                if (!d->present) continue;
                const bool is_mosi = (d == &mosi);
                const int ch = is_mosi ? ch_mosi : ch_miso;
                unsigned v = 0;
                bool known = true;
                for (int k = 0; k < nbits; ++k) {
                    // read the line a quarter bit before the sampling edge, in the middle of the stable
                    // part of the bit, so a spike coupled in from the clock edge itself is not read
                    const int lv = d->level_before(t.edges[i + static_cast<std::size_t>(k)] - 0.25 * w);
                    if (lv < 0) { known = false; break; }
                    if (lsb_) v |= static_cast<unsigned>(lv) << k;
                    else      v = (v << 1) | static_cast<unsigned>(lv);
                }
                if (!known) continue;
                if (nbits < bits_) {                          // the window ended inside a word
                    char b[48];
                    std::snprintf(b, sizeof b, "[%d BITS]", nbits);
                    emit(b0, b1, T::Error, b, std::string(is_mosi ? "MOSI" : "MISO") + ": the word was cut off after " +
                         std::to_string(nbits) + " of " + std::to_string(bits_) + " bits (" + binw(v, nbits) + "b)",
                         ch, -1, true);
                    ++errors_;
                    continue;
                }
                if (is_mosi && command) {
                    emit(b0, b1, T::Address, "CMD " + hexw(v, bits_), "Command " + hexw(v, bits_) + " (DC low), MOSI " + binw(v, bits_) + "b",
                         ch, static_cast<int>(v), false);
                } else {
                    const char* nm = is_mosi ? "MOSI" : "MISO";
                    emit(b0, b1, T::Data, std::string(nm) + " " + hexw(v, bits_),
                         std::string(nm) + " " + hexw(v, bits_) + " (" + binw(v, bits_) + "b)" + (dc.present && is_mosi ? ", DC high" : ""),
                         ch, static_cast<int>(v), false);
                }
                ++words_;
            }
            i += static_cast<std::size_t>(nbits);
        }
        if (t.has_cs) markers(t, false);
    }

    std::stable_sort(out.begin(), out.end(),
        [](const DecodedEvent& a, const DecodedEvent& b) { return a.start_ns < b.start_ns; });
    return out;
}

} // namespace escope
