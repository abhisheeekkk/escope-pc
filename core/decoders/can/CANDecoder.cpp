#include "decoders/can/CANDecoder.h"
#include "decoders/base/DecoderRegistry.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

namespace escope {
REGISTER_DECODER(CANDecoder);

namespace {

struct Edge { double t; bool lvl; };

// One continuously observed stretch of the line (a capture burst).
struct Segment {
    std::vector<Edge> e;       // real edges, time ordered
    double t0 = 0, t1 = 0;     // observed from t0 to t1
    bool   lvl0 = true;        // level at t0
};

bool level_at(const Segment& s, double t) {
    if (t < s.t0) return s.lvl0;
    auto it = std::upper_bound(s.e.begin(), s.e.end(), t,
        [](double v, const Edge& x) { return v < x.t; });
    return it == s.e.begin() ? s.lvl0 : (it - 1)->lvl;
}

std::string hexn(uint32_t v, int digits) {
    char b[16];
    std::snprintf(b, sizeof b, "%0*X", digits, v);
    return b;
}

// Pulses shorter than min_ns are noise (a rise and fall, or fall and rise, closer
// together than any real bit): remove both edges.
void drop_short_pulses(std::vector<Edge>& e, double min_ns) {
    std::vector<Edge> out;
    out.reserve(e.size());
    for (const auto& x : e) {
        if (out.size() >= 2 && x.lvl != out.back().lvl && x.t - out.back().t < min_ns) {
            out.pop_back();
            continue;
        }
        out.push_back(x);
    }
    e.swap(out);
}

const char* kind_of_droneCAN_type(uint32_t type) {
    return type == 341 ? " (uavcan.protocol.NodeStatus)"
         : type == 1   ? " (uavcan.protocol.dynamic_node_id.Allocation)" : "";
}

std::string dronecan_note(uint32_t id, const std::vector<uint8_t>& data) {
    // 29-bit DroneCAN / UAVCAN v0 ID: priority[28:24] message type[23:8] service[7] node[6:0]
    // (a broadcast message when the service bit is 0).
    char b[240];
    const unsigned prio = (id >> 24) & 0x1F, node = id & 0x7F;
    const unsigned type = (id >> 8) & 0xFFFF, svc = (id >> 7) & 1;
    std::string s;
    if (!svc && node == 0) {
        // Anonymous message (no node ID yet): bits 23:10 are a random discriminator and
        // only the low 2 bits of the type are used. Type 1 is the dynamic node ID request.
        const unsigned t2 = type & 3, disc = (id >> 10) & 0x3FFF;
        std::snprintf(b, sizeof b, "\nIf this is DroneCAN: anonymous message (the sender has no node ID), type %u%s, "
                      "priority %u, discriminator 0x%04X", t2,
                      t2 == 1 ? " (dynamic node ID allocation request)" : "", prio, disc);
        s = b;
        if (t2 == 1 && data.size() >= 2) {
            s += "\nRequest data: node ID wanted " + std::to_string(data[0] >> 1) +
                 ((data[0] & 1) ? ", first part of the unique ID: " : ", unique ID part: ");
            for (std::size_t i = 1; i + 1 < data.size(); ++i) {
                std::snprintf(b, sizeof b, "%02X", data[i]);
                s += b;
            }
            s += "\nNo allocator answered: set a node ID on the sensor or connect a DroneCAN flight controller";
        }
    } else if (!svc) {
        std::snprintf(b, sizeof b, "\nIf this is DroneCAN: broadcast message type %u%s, priority %u, source node %u",
                      type, kind_of_droneCAN_type(type), prio, node);
        s = b;
        const std::size_t n = data.size();
        const uint8_t tail = n ? data.back() : 0;
        const bool start = tail & 0x80, end = tail & 0x40;
        if (type == 341 && n == 8 && start && end) {
            const uint32_t up = data[0] | (data[1] << 8) | (data[2] << 16) | (uint32_t(data[3]) << 24);
            static const char* const health[] = {"OK", "WARNING", "ERROR", "CRITICAL"};
            static const char* const mode[] = {"OPERATIONAL", "INITIALIZATION", "MAINTENANCE", "SOFTWARE_UPDATE", "?", "?", "?", "OFFLINE"};
            std::snprintf(b, sizeof b, "\nNodeStatus: uptime %u s, health %s, mode %s, vendor status %u",
                          up, health[(data[4] >> 6) & 3], mode[(data[4] >> 3) & 7], data[5] | (data[6] << 8));
            s += b;
        } else if (type == 1 && n >= 2 && start) {
            // Allocation reply from a server: node_id is the first payload byte (after the
            // 2 byte CRC when the reply is a multi-frame transfer)
            const std::size_t o = end ? 0 : 2;
            if (n > o + 1) {
                const unsigned granted = data[o] >> 1;
                std::snprintf(b, sizeof b, "\nAllocation reply from node %u: %s", node,
                              granted ? "node ID granted: " : "request being answered, unique ID received so far echoed");
                s += b;
                if (granted) s += std::to_string(granted);
            }
        }
    } else {
        std::snprintf(b, sizeof b, "\nIf this is DroneCAN: service %u %s, priority %u, node %u to node %u",
                      (id >> 16) & 0xFF, ((id >> 15) & 1) ? "request" : "response", prio,
                      (id >> 8) & 0x7F, node);
        s = b;
    }
    if (!data.empty()) {
        const uint8_t tail = data.back();
        std::snprintf(b, sizeof b, "\nTail byte 0x%02X: %s%stoggle %u, transfer id %u",
                      tail, (tail & 0x80) ? "start, " : "", (tail & 0x40) ? "end, " : "",
                      (tail >> 5) & 1, tail & 0x1F);
        s += b;
    }
    return s;
}

struct Params { double bit_ns; double sample_point; };

// Walks the bits of one frame: samples at the sample point, resynchronises on every
// edge, and removes stuff bits.
class BitReader {
public:
    BitReader(const Segment& s, const Params& p, std::size_t first_edge, double sof_t)
        : s_(s), p_(p), ei_(first_edge), origin_(sof_t) {}

    struct Bit { bool v; double t0, t1; };

    enum class Fail { None, Truncated, Stuff };
    Fail fail = Fail::None;
    double fail_t = 0;

    // next sampled bit, no destuffing
    bool raw(Bit& b) {
        const double bit = p_.bit_ns;
        double ts = origin_ + (k_ + p_.sample_point) * bit;
        while (ei_ < s_.e.size() && s_.e[ei_].t <= ts) {
            const double dt = s_.e[ei_].t - origin_;
            const double j  = std::round(dt / bit);
            const double err = dt - j * bit;
            if (std::fabs(err) <= 0.25 * bit && j >= static_cast<double>(k_) - 1.0)
                origin_ += err;                       // hard resync on a bit edge
            ++ei_;
            ts = origin_ + (k_ + p_.sample_point) * bit;
        }
        if (ts > s_.t1) { fail = Fail::Truncated; fail_t = origin_ + k_ * bit; return false; }
        b.v  = level_at(s_, ts);
        b.t0 = origin_ + k_ * bit;
        b.t1 = b.t0 + bit;
        ++k_;
        return true;
    }

    // next data bit, with stuff bits removed while stuffing is on
    bool get(Bit& b) {
        for (;;) {
            if (!raw(b)) return false;
            if (!stuffing_) return true;
            if (run_ == 5) {                           // this must be a stuff bit
                if (b.v == last_) { fail = Fail::Stuff; fail_t = b.t0; return false; }
                last_ = b.v; run_ = 1;
                continue;
            }
            if (have_last_ && b.v == last_) ++run_; else { run_ = 1; last_ = b.v; }
            have_last_ = true;
            return true;
        }
    }

    // After the CRC sequence: a stuff bit may still follow it, then stuffing is over.
    bool end_stuffing() {
        if (stuffing_ && run_ == 5) {
            Bit b;
            if (!raw(b)) return false;
            if (b.v == last_) { fail = Fail::Stuff; fail_t = b.t0; return false; }
        }
        stuffing_ = false;
        return true;
    }

private:
    const Segment& s_;
    Params         p_;
    std::size_t    ei_;
    double         origin_;
    long           k_ = 0;
    bool           stuffing_ = true, have_last_ = false, last_ = false;
    int            run_ = 0;
};

uint16_t crc_step(uint16_t crc, bool bit) {
    const bool nxt = bit ^ ((crc >> 14) & 1);
    crc = static_cast<uint16_t>((crc << 1) & 0x7FFF);
    if (nxt) crc ^= 0x4599;
    return crc;
}

DecodedEvent make(double t0, double t1, DecodedEvent::Type type, std::string label,
                  std::string detail, uint8_t ch, int value = -1, bool err = false) {
    DecodedEvent e{};
    e.start_ns = t0; e.end_ns = t1; e.type = type; e.label = std::move(label);
    e.detail = std::move(detail); e.channel = ch; e.value = value; e.is_error = err;
    return e;
}

} // namespace


// ---- DroneCAN transfer reassembly -------------------------------------------------------
// A DroneCAN message longer than 7 bytes is split over several CAN frames: the first one
// starts with a 2 byte transfer CRC, every frame ends in a tail byte (start, end, toggle,
// 5 bit transfer id). The reassembled payload is described in the tooltip of each frame's
// ID box.
namespace {

float f32le(const std::vector<uint8_t>& d, std::size_t o) {
    uint32_t u = d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | (uint32_t(d[o + 3]) << 24);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

std::string describe_payload(unsigned type, const std::vector<uint8_t>& p) {
    char b[300];
    if (type == 20200 && p.size() == 21) {
        // com.hex.equipment.flow.Measurement (ARK Flow): float32 integration interval [s],
        // float32[2] gyro integral [rad], float32[2] flow integral [rad], uint8 quality
        std::snprintf(b, sizeof b,
            "Optical flow measurement: integration %.1f ms, gyro integral (%.3g, %.3g) rad, "
            "flow integral (%.3g, %.3g) rad, quality %u",
            f32le(p, 0) * 1000.0, f32le(p, 4), f32le(p, 8), f32le(p, 12), f32le(p, 16), p[20]);
        return b;
    }
    std::snprintf(b, sizeof b, "Payload %zu bytes", p.size());
    std::string s = b;
    for (std::size_t i = 0; i < p.size() && i < 24; ++i) { std::snprintf(b, sizeof b, " %02X", p[i]); s += b; }
    return s;
}

void annotate_transfers(std::vector<DecodedEvent>& ev) {
    struct Frame { std::size_t id_ev = SIZE_MAX; uint32_t id = 0; std::vector<uint8_t> data; bool bad = false; };
    struct Transfer { std::vector<std::size_t> id_events; std::vector<uint8_t> bytes; bool toggle = false; };
    std::map<uint32_t, Transfer> open;          // key: source node, message type, transfer id
    Frame f;
    bool in_frame = false;
    for (std::size_t i = 0; i < ev.size(); ++i) {
        const auto& e = ev[i];
        if (e.label == "SOF") { f = Frame{}; in_frame = true; continue; }
        if (!in_frame) continue;
        if (e.is_error && e.type == DecodedEvent::Type::Error) { in_frame = false; continue; }
        if (e.type == DecodedEvent::Type::Address) { f.id_ev = i; f.id = static_cast<uint32_t>(e.value); }
        else if (e.type == DecodedEvent::Type::Data && e.value >= 0) f.data.push_back(static_cast<uint8_t>(e.value));
        else if (e.label == "EOF") {
            in_frame = false;
            const bool ext = f.id_ev != SIZE_MAX && ev[f.id_ev].label.compare(0, 3, "IDE") == 0;
            if (!ext || f.data.empty() || ((f.id >> 7) & 1) || (f.id & 0x7F) == 0) continue;   // not a DroneCAN broadcast
            const uint8_t tail = f.data.back();
            const bool start = tail & 0x80, end = tail & 0x40, toggle = tail & 0x20;
            const uint32_t key = ((f.id & 0x7F) << 24) | (((f.id >> 8) & 0xFFFF) << 8) | (tail & 0x1F);
            if (start && end) continue;                               // single frame: handled elsewhere
            if (start) { open[key] = Transfer{}; open[key].toggle = false; }
            auto it = open.find(key);
            if (it == open.end() || toggle != it->second.toggle) { if (it != open.end()) open.erase(it); continue; }
            Transfer& t = it->second;
            t.id_events.push_back(f.id_ev);
            t.bytes.insert(t.bytes.end(), f.data.begin(), f.data.end() - 1);
            t.toggle = !t.toggle;
            if (end) {
                if (t.bytes.size() > 2) {
                    std::vector<uint8_t> payload(t.bytes.begin() + 2, t.bytes.end());
                    const std::string text = describe_payload((f.id >> 8) & 0xFFFF, payload);
                    char hdr[80];
                    std::snprintf(hdr, sizeof hdr, "\nMulti-frame transfer (%zu frames). ", t.id_events.size());
                    for (std::size_t k : t.id_events) ev[k].detail += std::string(hdr) + text;
                }
                open.erase(it);
            }
        }
    }
}

} // namespace

void CANDecoder::configure(const std::vector<std::pair<std::string,std::string>>& params) {
    for (const auto& [k, v] : params) {
        if (k == "bitrate")      bitrate_cfg_  = static_cast<uint32_t>(std::stoul(v));
        else if (k == "sample_point") sample_point_ = std::clamp(std::stod(v) / 100.0, 0.5, 0.95);
    }
}

uint16_t CANDecoder::crc15(const std::vector<uint8_t>& bits) {
    uint16_t crc = 0;
    for (uint8_t b : bits) crc = crc_step(crc, b != 0);
    return crc;
}

namespace {
const uint32_t kStandardRates[] = {10000, 20000, 50000, 100000, 125000, 250000, 500000, 800000, 1000000};
}

uint32_t CANDecoder::detect_bitrate(const DigitalBuffer& buf, uint8_t channel) {
    auto edges = buf.last_edges(channel, 3000);
    std::vector<double> gaps;
    gaps.reserve(edges.size());
    const DigitalEdge* prev = nullptr;
    for (const auto& e : edges) {
        if (e.snapshot) { prev = nullptr; continue; }       // not a real transition
        if (prev) {
            const double g = e.timestamp_ns - prev->timestamp_ns;
            if (g >= 200.0) gaps.push_back(g);       // shorter is ringing, faster than any CAN rate
        }
        prev = &e;
    }
    if (gaps.size() < 8) return 0;

    // Every pulse on a CAN line is a whole number of bit times (1 to 6, plus the longer
    // idle). Take the slowest standard rate that explains most of the pulses; a faster
    // rate would also fit (it divides the same pulses) and ringing or noise would
    // otherwise pull the choice to a rate far too high.
    uint32_t best = 0;
    double best_score = 0;
    for (uint32_t rate : kStandardRates) {
        const double bit = 1e9 / rate;
        std::size_t fit = 0;
        for (double g : gaps) {
            if (g > 6.5 * bit) { ++fit; continue; }            // idle gap: no information
            const double n = std::round(g / bit);
            if (n >= 1 && std::fabs(g - n * bit) <= 0.18 * bit) ++fit;
        }
        const double score = static_cast<double>(fit) / static_cast<double>(gaps.size());
        if (score >= 0.85) return rate;
        if (score > best_score) { best_score = score; best = rate; }
    }
    return best_score >= 0.5 ? best : 0;
}

std::vector<DecodedEvent> CANDecoder::decode(const DigitalBuffer& buf,
                                             const std::vector<DecoderChannelMap>& channels) {
    frames_ = errors_ = 0;
    bitrate_used_ = 0;
    if (channels.empty()) return {};
    const uint8_t ch = channels.front().channel;

    if (bitrate_cfg_) { auto r = decode_at(buf, ch, bitrate_cfg_); annotate_transfers(r); return r; }

    const uint32_t detected = detect_bitrate(buf, ch);
    if (!detected) return {};
    auto best = decode_at(buf, ch, detected);
    auto good = [](const std::vector<DecodedEvent>& ev) {
        std::size_t n = 0;
        for (const auto& e : ev) n += (e.label == "EOF");
        return n;
    };
    if (good(best) == 0 && errors_ > 0) {
        // The estimate gave nothing usable: try the other standard rates, keep the one
        // that decodes the most complete frames.
        std::size_t best_good = 0;
        uint32_t best_rate = detected;
        for (uint32_t r : kStandardRates) {
            if (r == detected) continue;
            auto ev = decode_at(buf, ch, r);
            if (good(ev) > best_good) { best_good = good(ev); best_rate = r; best = std::move(ev); }
        }
        decode_at(buf, ch, best_rate);          // restore counters and bitrate_used_
    }
    annotate_transfers(best);
    return best;
}

std::vector<DecodedEvent> CANDecoder::decode_at(const DigitalBuffer& buf, uint8_t ch, uint32_t rate) {
    frames_ = errors_ = 0;
    std::vector<DecodedEvent> out;
    bitrate_used_ = rate;
    const Params prm{1e9 / rate, sample_point_};
    const double bit = prm.bit_ns;

    const auto raw = buf.edges_for_channel(ch);
    if (raw.empty()) return out;
    const auto ends = buf.burst_ends();

    // Split into observed segments: a level snapshot starts one (a capture burst).
    std::vector<Segment> segs;
    std::vector<double> next_start;                        // start time of the following segment
    for (const auto& e : raw) {
        if (e.snapshot) {
            if (!segs.empty() && segs.back().t0 == e.timestamp_ns) { segs.back().lvl0 = e.rising; continue; }
            segs.push_back({});
            segs.back().t0 = e.timestamp_ns;
            segs.back().lvl0 = e.rising;
        } else {
            if (segs.empty()) { segs.push_back({}); segs.back().t0 = e.timestamp_ns; segs.back().lvl0 = !e.rising; }
            segs.back().e.push_back({e.timestamp_ns, e.rising});
        }
    }
    for (std::size_t i = 0; i < segs.size(); ++i) {
        const double nxt = i + 1 < segs.size() ? segs[i + 1].t0 : 1e300;
        Segment& s = segs[i];
        auto be = std::lower_bound(ends.begin(), ends.end(), s.t0);
        if (be != ends.end() && *be <= nxt) s.t1 = *be;
        else s.t1 = std::min(nxt, (s.e.empty() ? s.t0 : s.e.back().t) + 30 * bit);
        drop_short_pulses(s.e, 0.25 * bit);
    }

    using T = DecodedEvent::Type;
    using F = BitReader::Fail;

    for (const Segment& s : segs) {
        std::size_t i = 0;
        while (i < s.e.size()) {
            // A frame starts at a falling edge after at least 11 recessive bits (bus idle).
            const double prev_rise = i > 0 ? s.e[i - 1].t : s.t0;
            const bool starts_low  = (i == 0 && !s.lvl0);
            if (s.e[i].lvl || starts_low || s.e[i].t - prev_rise < 10.5 * bit) { ++i; continue; }

            const double t_sof = s.e[i].t;
            BitReader r(s, prm, i + 1, t_sof);
            std::vector<DecodedEvent> fe;                  // this frame's events
            double frame_end = t_sof + bit;
            bool   ok = false, fd = false;

            auto fail = [&](const char* label, double t, const char* what) {
                fe.push_back(make(t, t + bit * 0.5, T::Error, label, what, ch, -1, true));
                frame_end = t + bit;
            };
            auto bail = [&] {
                if (r.fail == F::Stuff)          fail("[STUFF ERR]", r.fail_t, "Six equal bits in a row: bit stuffing violated. A node signalling an error (for example a missing ACK) sends exactly this, an error flag of 6 dominant bits");
                else if (r.fail == F::Truncated) fail("[INCOMPLETE]", r.fail_t, "The capture ended in the middle of this frame");
            };

            BitReader::Bit b{};
            uint16_t crc = 0;
            std::vector<uint8_t> bits_for_check;
            auto data_bit = [&](bool crc_on) -> bool {
                if (!r.get(b)) return false;
                if (crc_on) crc = crc_step(crc, b.v);
                return true;
            };
            auto read = [&](int n, uint32_t& v, double& t_a, double& t_b, bool crc_on) -> bool {
                v = 0;
                for (int k = 0; k < n; ++k) {
                    if (!data_bit(crc_on)) return false;
                    if (k == 0) t_a = b.t0;
                    v = (v << 1) | (b.v ? 1u : 0u);
                    t_b = b.t1;
                }
                return true;
            };

            do {
                // SOF
                if (!data_bit(true)) { bail(); break; }
                if (b.v) break;                            // not a dominant start: a glitch
                fe.push_back(make(t_sof, t_sof, T::Control, "SOF", "Start of frame", ch));

                uint32_t id = 0, tmp = 0; double ta = 0, tb = 0, tid0 = 0;
                if (!read(11, id, tid0, tb, true)) { bail(); break; }
                double t_id_end = tb;
                if (!data_bit(true)) { bail(); break; }            // RTR (base) or SRR (extended)
                const bool rtr_or_srr = b.v;
                const double t_rtr0 = b.t0;
                if (!read(1, tmp, ta, tb, true)) { bail(); break; }   // IDE
                const bool ext = tmp != 0;
                bool rtr = rtr_or_srr;
                if (ext) {
                    uint32_t low = 0;
                    if (!read(18, low, ta, tb, true)) { bail(); break; }
                    id = (id << 18) | low;
                    t_id_end = tb;
                    if (!read(1, tmp, ta, tb, true)) { bail(); break; }   // RTR
                    rtr = tmp != 0;
                    const double t_rtr = ta;
                    (void)t_rtr;
                    if (!read(1, tmp, ta, tb, true)) { bail(); break; }   // r1 (FDF in an FD frame)
                    if (tmp) { fd = true; break; }
                    if (!read(1, tmp, ta, tb, true)) { bail(); break; }   // r0
                } else {
                    if (!read(1, tmp, ta, tb, true)) { bail(); break; }   // r0 (FDF in an FD frame)
                    if (tmp) { fd = true; break; }
                }
                const double t_ctrl0 = ext ? t_id_end : t_rtr0;
                char lb[48];
                std::snprintf(lb, sizeof lb, ext ? "IDE 0x%08X" : "ID 0x%03X", id);
                fe.push_back(make(tid0, t_id_end, T::Address, lb,
                                  ext ? "Extended (29-bit) identifier" : "Standard (11-bit) identifier",
                                  ch, static_cast<int>(id)));
                const std::size_t id_event = fe.size() - 1;

                uint32_t dlc = 0;
                if (!read(4, dlc, ta, tb, true)) { bail(); break; }
                std::snprintf(lb, sizeof lb, rtr ? "RTR DLC %u" : "DLC %u", dlc);
                fe.push_back(make(t_ctrl0, tb, T::Annotation, lb,
                                  rtr ? "Remote frame (requests data, carries none)" : "Data length code",
                                  ch, static_cast<int>(dlc)));

                const uint32_t len = rtr ? 0 : std::min<uint32_t>(dlc, 8);
                std::vector<uint8_t> data;
                bool bad = false;
                for (uint32_t k = 0; k < len; ++k) {
                    uint32_t v = 0;
                    if (!read(8, v, ta, tb, true)) { bail(); bad = true; break; }
                    data.push_back(static_cast<uint8_t>(v));
                    std::snprintf(lb, sizeof lb, "0x%02X", v);
                    fe.push_back(make(ta, tb, T::Data, lb, "Data byte " + std::string(lb), ch, static_cast<int>(v)));
                }
                if (bad) break;
                if (ext && !fe.empty()) fe[id_event].detail += dronecan_note(id, data);

                // CRC sequence (still stuffed), then the stuffing stops
                const uint16_t want = crc;
                uint32_t got = 0;
                if (!read(15, got, ta, tb, false)) { bail(); break; }
                if (!r.end_stuffing()) { bail(); break; }
                const bool crc_ok = got == want;
                std::snprintf(lb, sizeof lb, crc_ok ? "CRC 0x%04X OK" : "CRC 0x%04X BAD", got);
                {
                    std::string det = crc_ok ? "CRC-15 matches" :
                        "CRC-15 mismatch: the frame carries 0x" + hexn(got, 4) + ", computed 0x" + hexn(want, 4);
                    fe.push_back(make(ta, tb, T::Annotation, lb, det, ch, static_cast<int>(got), !crc_ok));
                }

                // CRC delimiter, ACK slot, ACK delimiter, EOF: fixed form, no stuffing
                BitReader::Bit x{};
                if (!r.raw(x)) { bail(); break; }
                if (x.v == false) { fail("[FORM ERR]", x.t0, "CRC delimiter must be recessive"); break; }
                if (!r.raw(x)) { bail(); break; }
                const bool acked = !x.v;
                fe.push_back(make(x.t0, x.t1, T::Annotation, acked ? "ACK" : "NO ACK",
                                  acked ? "Acknowledged by a receiver" : "No receiver acknowledged this frame",
                                  ch, acked ? 1 : 0, !acked));
                if (!r.raw(x)) { bail(); break; }
                if (x.v == false) { fail("[FORM ERR]", x.t0, "ACK delimiter must be recessive"); break; }
                double t_eof0 = 0, t_eof1 = 0;
                bool eof_ok = true;
                for (int k = 0; k < 7; ++k) {
                    if (!r.raw(x)) { bail(); eof_ok = false; break; }
                    if (k == 0) t_eof0 = x.t0;
                    t_eof1 = x.t1;
                    if (!x.v) { fail("[FORM ERR]", x.t0, "End of frame must be 7 recessive bits"); eof_ok = false; break; }
                }
                if (!eof_ok) break;
                (void)t_eof0;
                fe.push_back(make(t_eof1, t_eof1, T::Control, "EOF", "End of frame", ch));
                frame_end = t_eof1;
                ok = true;
            } while (false);

            if (fd) {
                fe.push_back(make(t_sof, t_sof + 20 * bit, T::Annotation, "CAN FD frame (not decoded)",
                                  "A CAN FD frame was seen (the FDF bit is recessive); it is skipped", ch, -1, true));
                frame_end = t_sof + 20 * bit;
            }
            if (!fe.empty()) {
                // a failed frame still shows what was decoded up to the failure
                bool had_error = !ok;
                for (auto& e : fe) had_error = had_error || e.is_error;
                ++frames_;
                if (had_error) ++errors_;
                for (auto& e : fe) out.push_back(std::move(e));
            }

            // continue after this frame (or after where it went wrong)
            const double resume = std::max(frame_end - 0.5 * bit, t_sof + 0.5 * bit);
            while (i < s.e.size() && s.e[i].t < resume) ++i;
        }
    }

    std::sort(out.begin(), out.end(),
        [](const DecodedEvent& a, const DecodedEvent& b) { return a.start_ns < b.start_ns; });
    return out;
}

} // namespace escope
