#include <gtest/gtest.h>
#include "decoders/can/CANDecoder.h"
#include "acquisition/DigitalBuffer.h"

using namespace escope;

namespace {
constexpr double BIT = 2000.0;   // 500 kbit/s

// Independent CRC-15: long division of the message (padded with 15 zeros) by the
// generator polynomial x^15+x^14+x^10+x^8+x^7+x^4+x^3+1.
std::vector<uint8_t> crc_bits(std::vector<uint8_t> m) {
    m.insert(m.end(), 15, 0);
    const int gen[16] = {1,1,0,0,0,1,0,0,1,1,0,0,1,1,0,1}; // 0xC599 = 1 0xC599... see below
    (void)gen;
    const uint32_t g = 0x4599 | 0x8000;                      // 16-bit generator with x^15 term
    const std::size_t n = m.size() - 15;
    for (std::size_t i = 0; i < n; ++i)
        if (m[i]) for (int k = 0; k < 16; ++k) m[i + k] ^= (g >> (15 - k)) & 1;
    return {m.begin() + n, m.end()};
}

void push_bits(std::vector<uint8_t>& v, uint32_t x, int n) {
    for (int i = n - 1; i >= 0; --i) v.push_back((x >> i) & 1);
}

std::vector<uint8_t> stuff(const std::vector<uint8_t>& in) {
    std::vector<uint8_t> o; int run = 0; uint8_t last = 2;
    for (uint8_t b : in) {
        o.push_back(b);
        run = (b == last) ? run + 1 : 1; last = b;
        if (run == 5) { o.push_back(!b); last = !b; run = 1; }
    }
    return o;
}

struct Frame { uint32_t id; bool ext; bool rtr; std::vector<uint8_t> data; bool ack = true; };

std::vector<uint8_t> encode(const Frame& f, bool corrupt_crc = false) {
    std::vector<uint8_t> b = {0};                            // SOF
    if (!f.ext) { push_bits(b, f.id, 11); b.push_back(f.rtr); b.push_back(0); b.push_back(0); }
    else {
        push_bits(b, f.id >> 18, 11); b.push_back(1); b.push_back(1);
        push_bits(b, f.id & 0x3FFFF, 18); b.push_back(f.rtr); b.push_back(0); b.push_back(0);
    }
    push_bits(b, f.data.size(), 4);
    for (uint8_t d : f.data) push_bits(b, d, 8);
    auto crc = crc_bits(b);
    if (corrupt_crc) crc[3] ^= 1;
    b.insert(b.end(), crc.begin(), crc.end());
    auto w = stuff(b);
    w.push_back(1); w.push_back(f.ack ? 0 : 1); w.push_back(1);
    w.insert(w.end(), 7, 1);
    return w;
}

struct Bus {
    DigitalBuffer buf{1};
    double t = 20 * BIT;
    bool lvl = true;
    Bus() { buf.push_edge(0, 0, true); }
    void bits(const std::vector<uint8_t>& w) {
        for (uint8_t b : w) { if (bool(b) != lvl) { lvl = b; buf.push_edge(t, 0, lvl); } t += BIT; }
    }
    void idle(int n = 14) { bits(std::vector<uint8_t>(n, 1)); }
};

std::vector<DecodedEvent> run(Bus& b, CANDecoder* out = nullptr) {
    CANDecoder d;
    auto ev = d.decode(b.buf, {{"RX", 0}});
    if (out) *out = d;
    return ev;
}
const DecodedEvent* find(const std::vector<DecodedEvent>& ev, const std::string& pre) {
    for (auto& e : ev) if (e.label.rfind(pre, 0) == 0) return &e;
    return nullptr;
}
}

TEST(CANDecoder, CrcMatchesTwoWays) {
    std::vector<uint8_t> m; push_bits(m, 0x123, 11); push_bits(m, 0x0A55, 14);
    uint16_t a = CANDecoder::crc15(m), b = 0;
    for (uint8_t x : crc_bits(m)) b = (b << 1) | x;
    EXPECT_EQ(a, b);
}

TEST(CANDecoder, StandardFrame) {
    Bus b; b.idle();
    b.bits(encode({0x123, false, false, {1,2,3,4,5,6,7,8}}));
    b.idle();
    CANDecoder d; auto ev = d.decode(b.buf, {{"RX", 0}});
    ASSERT_NE(find(ev, "SOF"), nullptr);
    auto id = find(ev, "ID 0x123"); ASSERT_NE(id, nullptr); EXPECT_EQ(id->value, 0x123);
    EXPECT_NE(find(ev, "DLC 8"), nullptr);
    EXPECT_NE(find(ev, "0x08"), nullptr);
    EXPECT_NE(find(ev, "CRC"), nullptr);
    EXPECT_NE(find(ev, "CRC")->label.find("OK"), std::string::npos);
    EXPECT_NE(find(ev, "ACK"), nullptr);
    EXPECT_NE(find(ev, "EOF"), nullptr);
    EXPECT_EQ(d.bitrate_used(), 500000u);
    EXPECT_EQ(d.errors(), 0u);
}

TEST(CANDecoder, ExtendedFrameWithStuffing) {
    Bus b; b.idle();
    b.bits(encode({0x1801550A, true, false, {0xFF,0xFF,0xFF,0xFF,0x00,0x00,0xC1}}));
    b.idle();
    CANDecoder d; auto ev = d.decode(b.buf, {{"RX", 0}});
    auto id = find(ev, "IDE 0x1801550A"); ASSERT_NE(id, nullptr);
    EXPECT_NE(id->detail.find("node 10"), std::string::npos);
    EXPECT_EQ(d.errors(), 0u);
}

TEST(CANDecoder, RemoteAndEmptyFrames) {
    Bus b; b.idle();
    b.bits(encode({0x321, false, true, {}}));
    b.idle();
    b.bits(encode({0x7FF, false, false, {}}));
    b.idle();
    auto ev = run(b);
    EXPECT_NE(find(ev, "RTR DLC"), nullptr);
    EXPECT_NE(find(ev, "ID 0x7FF"), nullptr);
}

TEST(CANDecoder, BadCrcAndNoAck) {
    Bus b; b.idle();
    b.bits(encode({0x123, false, false, {9}}, true));
    b.idle();
    b.bits(encode({0x124, false, false, {9}, false}));
    b.idle();
    CANDecoder d; auto ev = d.decode(b.buf, {{"RX", 0}});
    bool bad = false, noack = false;
    for (auto& e : ev) { if (e.label.find("BAD") != std::string::npos) bad = true; if (e.label == "NO ACK") noack = true; }
    EXPECT_TRUE(bad); EXPECT_TRUE(noack);
}

TEST(CANDecoder, StuffError) {
    Bus b; b.idle();
    b.bits({0, 0,0,0,0,0,0, 1, 0});       // six dominant bits after SOF
    b.idle();
    CANDecoder d; d.configure({{"bitrate", "500000"}});
    auto ev = d.decode(b.buf, {{"RX", 0}});
    EXPECT_NE(find(ev, "[STUFF ERR]"), nullptr);
}

TEST(CANDecoder, TruncatedByBurstEnd) {
    Bus b; b.idle();
    const double start = b.t;
    auto w = encode({0x123, false, false, {1,2,3,4}});
    b.bits(w);
    b.buf.mark_burst_end(start + 30 * BIT);
    auto ev = run(b);
    EXPECT_NE(find(ev, "[INCOMPLETE]"), nullptr);
    EXPECT_EQ(find(ev, "[STUFF ERR]"), nullptr);
}

TEST(CANDecoder, ClockDriftTolerated) {
    Bus b; b.idle();
    // bit time 1.5 % slow relative to the configured 500 kbit/s
    DigitalBuffer& buf = b.buf; double t = b.t; bool lvl = true;
    for (uint8_t x : encode({0x1F0, false, false, {0xA5,0x5A,0xFF,0x00}})) {
        if (bool(x) != lvl) { lvl = x; buf.push_edge(t, 0, lvl); } t += BIT * 1.015;
    }
    if (!lvl) buf.push_edge(t, 0, true);
    CANDecoder d; d.configure({{"bitrate", "500000"}});
    auto ev = d.decode(buf, {{"RX", 0}});
    EXPECT_NE(find(ev, "ID 0x1F0"), nullptr);
    EXPECT_EQ(d.errors(), 0u);
}

TEST(CANDecoder, RingingDoesNotFakeAHighBitRate) {
    // 1 Mbit/s frames with a 100 ns glitch pair on every edge (transceiver ringing)
    DigitalBuffer buf{1};
    const double bit = 1000.0;
    double t = 30 * bit; bool lvl = true; buf.push_edge(0, 0, true);
    for (int f = 0; f < 6; ++f) {
        for (uint8_t x : encode({0x123, false, false, {uint8_t(f), 0x55, 0xAA, 0, 1, 2, 3, 4}})) {
            if (bool(x) != lvl) {
                lvl = x; buf.push_edge(t, 0, lvl);
                buf.push_edge(t + 40, 0, !lvl); buf.push_edge(t + 90, 0, lvl);   // ring
            }
            t += bit;
        }
        t += 14 * bit;
    }
    CANDecoder d; auto ev = d.decode(buf, {{"RX", 0}});
    EXPECT_EQ(d.bitrate_used(), 1000000u);
    EXPECT_NE(find(ev, "ID 0x123"), nullptr);
}

TEST(CANDecoder, AnonymousDroneCanAllocationRequest) {
    Bus b; b.idle();
    b.bits(encode({0x1E113D00, true, false, {0x01, 0x20, 0x37, 0x37, 0x4C, 0x33, 0x31, 0xC0}}));
    b.idle();
    CANDecoder d; d.configure({{"bitrate", "500000"}});
    auto ev = d.decode(b.buf, {{"RX", 0}});
    auto id = find(ev, "IDE 0x1E113D00");
    ASSERT_NE(id, nullptr);
    EXPECT_NE(id->detail.find("anonymous"), std::string::npos);
    EXPECT_NE(id->detail.find("allocation request"), std::string::npos);
}

TEST(CANDecoder, DroneCanNodeStatusAndAllocationReply) {
    Bus b; b.idle();
    // NodeStatus from node 20: uptime 25 s, health OK, operational, tail start+end id 3
    b.bits(encode({(24u << 24) | (341u << 8) | 20u, true, false, {25, 0, 0, 0, 0, 0, 0, 0xC3}}));
    b.idle();
    // first frame of a multi-frame allocation reply from node 1: CRC, node_id 20, uid bytes
    b.bits(encode({(30u << 24) | (1u << 8) | 1u, true, false, {0x11, 0x22, 20 << 1, 1, 2, 3, 4, 0x80}}));
    b.idle();
    CANDecoder d; d.configure({{"bitrate", "500000"}});
    auto ev = d.decode(b.buf, {{"RX", 0}});
    std::string all;
    for (auto& e : ev) all += e.detail + "\n";
    EXPECT_NE(all.find("NodeStatus: uptime 25 s, health OK, mode OPERATIONAL"), std::string::npos);
    EXPECT_NE(all.find("node ID granted: 20"), std::string::npos);
}

TEST(CANDecoder, ReassemblesRealArkFlowMeasurement) {
    // The four frames of one transfer captured from a real ARK Flow (node 20, type 20200)
    const uint32_t id = 0x104EE814;
    Bus b; b.idle();
    b.bits(encode({id, true, false, {0x9C, 0x22, 0x0A, 0xD7, 0xA3, 0x3C, 0xFF, 0x9C}})); b.idle(4);
    b.bits(encode({id, true, false, {0x3E, 0x8B, 0xB8, 0xFE, 0x9B, 0xF4, 0xB8, 0x3C}})); b.idle(4);
    b.bits(encode({id, true, false, {0x19, 0xB8, 0x0B, 0x3B, 0x75, 0xED, 0x4C, 0x1C}})); b.idle(4);
    b.bits(encode({id, true, false, {0x2F, 0x5E, 0x7C}})); b.idle();
    CANDecoder d; d.configure({{"bitrate", "500000"}});
    auto ev = d.decode(b.buf, {{"RX", 0}});
    std::string all;
    for (auto& e : ev) all += e.detail + "\n";
    EXPECT_NE(all.find("integration 20.0 ms"), std::string::npos) << all;
    EXPECT_NE(all.find("quality 94"), std::string::npos);
    EXPECT_NE(all.find("Multi-frame transfer (4 frames)"), std::string::npos);
}
