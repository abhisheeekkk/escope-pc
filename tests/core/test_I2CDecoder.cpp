#include <gtest/gtest.h>
#include "decoders/i2c/I2CDecoder.h"
#include "acquisition/DigitalBuffer.h"

using namespace escope;

namespace {
constexpr uint8_t SCL = 0, SDA = 1;
constexpr double  T   = 5000.0;   // quarter... half clock period in ns

struct Bus {
    DigitalBuffer buf{2};
    double t = 0;
    bool scl = true, sda = true;

    Bus() { buf.push_edge(0, SCL, true); buf.push_edge(0, SDA, true); t = T; }
    void set_sda(bool v) { if (v != sda) { sda = v; buf.push_edge(t, SDA, v); } }
    void set_scl(bool v) { if (v != scl) { scl = v; buf.push_edge(t, SCL, v); } }
    void step() { t += T; }

    void start() { set_sda(true); step(); set_scl(true); step(); set_sda(false); step(); set_scl(false); step(); }
    void stop()  { set_sda(false); set_scl(false); step(); set_scl(true); step(); set_sda(true); step(); }
    void bit(bool b) { set_sda(b); step(); set_scl(true); step(); set_scl(false); step(); }
    void byte(uint8_t v, bool ack) {
        for (int i = 7; i >= 0; --i) bit((v >> i) & 1);
        bit(!ack);   // ACK = SDA low
    }
};

std::vector<DecodedEvent> run(Bus& b) {
    I2CDecoder d;
    return d.decode(b.buf, {{"SCL", SCL}, {"SDA", SDA}});
}
}

TEST(I2CDecoder, WriteTransaction) {
    Bus b;
    b.start();
    b.byte(0xA0, true);     // addr 0x50 write
    b.byte(0x12, true);
    b.byte(0x34, false);    // NACK
    b.stop();

    auto ev = run(b);
    ASSERT_EQ(ev.size(), 5u);
    EXPECT_EQ(ev[0].label, "START");
    EXPECT_EQ(ev[1].label, "ADDR 0x50 W ACK");
    EXPECT_EQ(ev[1].value, 0xA0);
    EXPECT_EQ(ev[2].label, "0x12 ACK");
    EXPECT_EQ(ev[3].label, "0x34 NACK");
    EXPECT_EQ(ev[3].value, 0x34);
    EXPECT_EQ(ev[4].label, "STOP");
}

TEST(I2CDecoder, RepeatedStartAndRead) {
    Bus b;
    b.start();
    b.byte(0xA0, true);
    b.byte(0x00, true);
    b.start();              // repeated
    b.byte(0xA1, true);     // read
    b.byte(0xFF, false);
    b.stop();

    auto ev = run(b);
    ASSERT_EQ(ev.size(), 7u);
    EXPECT_EQ(ev[3].label, "Sr");
    EXPECT_EQ(ev[4].label, "ADDR 0x50 R ACK");
}

TEST(I2CDecoder, MissingChannelsGivesNothing) {
    Bus b;
    I2CDecoder d;
    EXPECT_TRUE(d.decode(b.buf, {{"SCL", SCL}}).empty());
}

// SDA changing on the same sample as SCL falling (zero hold time at the
// capture's sample rate) must not look like a START/STOP or shift bits.
TEST(I2CDecoder, SdaChangesOnSameSampleAsSclFall) {
    Bus b;
    b.start();                               // SDA fell, SCL then falls
    auto send = [&](uint8_t v, bool ack) {
        auto bit = [&](bool x) {
            b.set_scl(false); b.set_sda(x);  // same timestamp
            b.step();
            b.set_scl(true);
            b.step();
        };
        for (int i = 7; i >= 0; --i) bit((v >> i) & 1);
        bit(!ack);
    };
    send(0xA0, true);
    send(0x12, true);
    send(0xFF, false);
    b.set_scl(false); b.set_sda(false); b.step();   // STOP set-up, same sample
    b.set_scl(true);  b.step();
    b.set_sda(true);  b.step();

    auto ev = run(b);
    ASSERT_EQ(ev.size(), 5u);
    EXPECT_EQ(ev[0].label, "START");
    EXPECT_EQ(ev[1].label, "ADDR 0x50 W ACK");
    EXPECT_EQ(ev[2].label, "0x12 ACK");
    EXPECT_EQ(ev[3].label, "0xFF NACK");
    EXPECT_EQ(ev[4].label, "STOP");
}

TEST(I2CDecoder, SdaHasOlderHistoryThanScl) {
    // SCL's oldest edges were dropped by the per-channel cap, so SDA has
    // edges from before SCL's first one. They must not decode as START/STOP.
    DigitalBuffer buf(2);
    buf.push_edge(0,    SDA, true);
    buf.push_edge(1000, SDA, false);
    buf.push_edge(2000, SDA, true);
    buf.push_edge(1e6,  SCL, true);

    Bus b;                         // reuse the helper's timing on a fresh buffer
    // Replay the helper's transaction into `buf` after t0 = 1e6.
    b.start(); b.byte(0xA0, true); b.byte(0x12, true); b.stop();
    for (uint8_t ch : {SCL, SDA})
        for (const auto& e : b.buf.edges_for_channel(ch))
            if (e.timestamp_ns > 0) buf.push_edge(e.timestamp_ns + 1e6, ch, e.rising);

    I2CDecoder d;
    auto ev = d.decode(buf, {{"SCL", SCL}, {"SDA", SDA}});
    ASSERT_EQ(ev.size(), 4u);
    EXPECT_EQ(ev[0].label, "START");
    EXPECT_EQ(ev[1].label, "ADDR 0x50 W ACK");
    EXPECT_EQ(ev[2].label, "0x12 ACK");
    EXPECT_EQ(ev[3].label, "STOP");
}

// Bursts from the capture hardware start with a level snapshot (not a real
// edge) and usually begin in the middle of a transfer.
namespace {
void snapshot(Bus& b, bool scl, bool sda) {
    DigitalEdge e[2] = {{b.t, SCL, scl, true}, {b.t, SDA, sda, true}};
    b.buf.push_batch(e, 2);
    b.scl = scl; b.sda = sda;
}
}

TEST(I2CDecoder, BurstStartingMidTransferRecoversBytes) {
    Bus b;
    b.t = 1e6;
    snapshot(b, false, false);          // burst opens with SCL low, SDA low
    b.step();
    for (uint8_t v : {0x13, 0x34, 0x57, 0x78, 0x9B, 0xBC}) b.byte(v, true);
    b.stop();

    auto ev = run(b);
    // "~" (alignment inferred), six data bytes, STOP -- and no false START.
    ASSERT_EQ(ev.size(), 8u);
    EXPECT_EQ(ev[0].label, "~");
    EXPECT_EQ(ev[1].label, "0x13 ACK");
    EXPECT_EQ(ev[6].label, "0xBC ACK");
    EXPECT_EQ(ev[7].label, "STOP");
    for (const auto& e : ev) EXPECT_NE(e.label, "START");
}

TEST(I2CDecoder, SnapshotWithSdaLowSclHighIsNotAStart) {
    Bus b;
    b.t = 1e6;
    snapshot(b, true, false);           // would look like a START if it were an edge
    b.step();
    b.set_scl(false); b.step();
    for (uint8_t v : {0x13, 0x34, 0x57, 0x78, 0x9B}) b.byte(v, true);
    b.stop();

    for (const auto& e : run(b)) EXPECT_NE(e.label, "START");
}

TEST(I2CDecoder, RealStartInsideBurstStillGivesAddress) {
    Bus b;
    b.t = 1e6;
    snapshot(b, true, true);            // idle at burst start
    b.step();
    b.start();
    b.byte(0xA0, true);
    b.byte(0x12, true);
    b.stop();

    auto ev = run(b);
    ASSERT_EQ(ev.size(), 4u);
    EXPECT_EQ(ev[1].label, "ADDR 0x50 W ACK");
}

// A spike on SCL (e.g. chatter as a slow edge crosses the input threshold) must
// not be counted as an extra clock: it would shift every following bit.
TEST(I2CDecoder, ShortSclSpikeIsIgnored) {
    Bus b;
    b.start();
    auto bit = [&](bool x, bool spike) {
        b.set_sda(x); b.step();
        if (spike) {                         // runt pulse while SCL should be low
            b.buf.push_edge(b.t,        SCL, true);
            b.buf.push_edge(b.t + 40.0, SCL, false);
            b.t += 100.0;
        }
        b.set_scl(true);  b.step();
        b.set_scl(false); b.step();
    };
    auto byte = [&](uint8_t v, bool ack, int spike_bit) {
        for (int i = 7; i >= 0; --i) bit((v >> i) & 1, i == spike_bit);
        bit(!ack, false);
    };
    byte(0xA0, true, 3);      // spike in the middle of the address byte
    byte(0x12, true, 5);      // and another in the data byte
    b.stop();

    I2CDecoder d;
    auto ev = d.decode(b.buf, {{"SCL", SCL}, {"SDA", SDA}});
    EXPECT_EQ(d.glitches_filtered(), 2u);
    ASSERT_EQ(ev.size(), 4u);
    EXPECT_EQ(ev[1].label, "ADDR 0x50 W ACK");
    EXPECT_EQ(ev[2].label, "0x12 ACK");

    // And without the filter the same data decodes wrongly (what we saw live).
    I2CDecoder raw;
    raw.configure({{"glitch_ns", "0"}});
    auto bad = raw.decode(b.buf, {{"SCL", SCL}, {"SDA", SDA}});
    bool all_good = bad.size() == 4 && bad[1].label == "ADDR 0x50 W ACK" && bad[2].label == "0x12 ACK";
    EXPECT_FALSE(all_good);
}

TEST(I2CDecoder, GlitchFilterKeepsRealPulses) {
    Bus b;
    b.start(); b.byte(0xA0, true); b.byte(0x12, true); b.stop();
    I2CDecoder d;
    auto ev = d.decode(b.buf, {{"SCL", SCL}, {"SDA", SDA}});
    EXPECT_EQ(d.glitches_filtered(), 0u);
    ASSERT_EQ(ev.size(), 4u);
    EXPECT_EQ(ev[1].label, "ADDR 0x50 W ACK");
}
