#include <gtest/gtest.h>
#include "hal/BurstFrame.h"
#include <vector>
#include <cstring>

using namespace escope;

namespace {
void put32(std::vector<uint8_t>& v, std::size_t off, uint32_t x) {
    for (int i = 0; i < 4; ++i) v[off + i] = uint8_t(x >> (8 * i));
}
std::vector<uint8_t> frame(uint8_t ver, uint32_t nsamp, uint32_t rate, uint32_t trig,
                           uint64_t t0 = 0) {
    std::vector<uint8_t> v(ver == 2 ? 32 : 24, 0);
    v[0] = BURST_MAGIC; v[1] = ver;
    put32(v, 4, rate); put32(v, 8, nsamp); put32(v, 12, trig); put32(v, 16, 7);
    if (ver == 2) { put32(v, 24, uint32_t(t0)); put32(v, 28, uint32_t(t0 >> 32)); }
    return v;
}
BurstHeader hdr_v2(uint64_t t0) {
    BurstHeader h; auto f = frame(2, 229376, 48000000, 100, t0);
    EXPECT_EQ(parse_burst_header(f.data(), f.size(), h), BurstParse::Ok);
    return h;
}
constexpr double DUR = 229376.0 * 1e9 / 48e6;      // 4.778 ms
}

TEST(BurstFrame, ParsesVersion2WithDeviceTime) {
    auto f = frame(2, 229376, 48000000, 1234, 0x0000000500000007ULL);
    BurstHeader h;
    ASSERT_EQ(parse_burst_header(f.data(), f.size(), h), BurstParse::Ok);
    EXPECT_EQ(h.version, 2);
    EXPECT_EQ(h.header_size, 32u);
    EXPECT_TRUE(h.has_t0);
    EXPECT_EQ(h.t0_ns, 0x0000000500000007ULL);
    EXPECT_EQ(h.nsamp, 229376u);
    EXPECT_EQ(h.trigger_index, 1234u);
    EXPECT_EQ(h.seq, 7u);
}

TEST(BurstFrame, ParsesVersion1WithoutDeviceTime) {
    auto f = frame(1, 1000, 48000000, 5);
    BurstHeader h;
    ASSERT_EQ(parse_burst_header(f.data(), f.size(), h), BurstParse::Ok);
    EXPECT_EQ(h.header_size, 24u);
    EXPECT_FALSE(h.has_t0);
}

TEST(BurstFrame, NeedsMoreBytesAndRejectsGarbage) {
    auto f = frame(2, 1000, 48000000, 5, 1);
    BurstHeader h;
    EXPECT_EQ(parse_burst_header(f.data(), 10, h), BurstParse::NeedMore);
    EXPECT_EQ(parse_burst_header(f.data(), 28, h), BurstParse::NeedMore);   // v2 needs 32
    auto bad = frame(3, 1000, 48000000, 5);       bad.resize(32);
    EXPECT_EQ(parse_burst_header(bad.data(), bad.size(), h), BurstParse::Bad);   // unknown version
    auto zero = frame(2, 0, 48000000, 0, 1);
    EXPECT_EQ(parse_burst_header(zero.data(), zero.size(), h), BurstParse::Bad);
    auto huge = frame(2, BURST_MAX_SAMPLES + 1, 48000000, 0, 1);
    EXPECT_EQ(parse_burst_header(huge.data(), huge.size(), h), BurstParse::Bad);
}

// The point of the device clock: bursts are placed by it, so arrival jitter on
// the PC (USB, scheduling) no longer moves them.
TEST(BurstClock, VersionTwoIgnoresArrivalJitter) {
    BurstClock c;
    const double jitter[] = {0.0, 1.3e6, -0.9e6, 0.4e6, 2.0e6, -1.5e6};
    double first = 0, prev = 0;
    for (int i = 0; i < 6; ++i) {
        const uint64_t dev = 5000000000ULL + uint64_t(i) * 399940000ULL;   // exact 399.94 ms apart
        const double wall = 2e9 + i * 400e6 + jitter[i];                   // noisy arrival
        double t0 = c.place(hdr_v2(dev), wall);
        if (i == 0) first = t0; else EXPECT_NEAR(t0 - prev, 399.94e6, 1.0);
        prev = t0;
    }
    EXPECT_NEAR(prev - first, 5 * 399.94e6, 1.0);
}

TEST(BurstClock, VersionOneFallsBackToArrivalTime) {
    BurstClock c;
    BurstHeader h; auto f = frame(1, 229376, 48000000, 100);
    ASSERT_EQ(parse_burst_header(f.data(), f.size(), h), BurstParse::Ok);
    EXPECT_NEAR(c.place(h, 1e9), 1e9 - DUR, 1.0);
    EXPECT_NEAR(c.place(h, 1.4e9), 1.4e9 - DUR, 1.0);
}

TEST(BurstClock, DeviceClockGoingBackwardsContinuesTheTimeline) {
    BurstClock c;
    double a = c.place(hdr_v2(9000000000ULL), 3e9);
    double b = c.place(hdr_v2(9400000000ULL), 3.4e9);
    EXPECT_NEAR(b - a, 400e6, 1.0);
    // The board was reset: its clock restarts near zero.
    double r = c.place(hdr_v2(120000000ULL), 3.8e9);
    EXPECT_GE(r, b + DUR);                          // never jumps back
    double r2 = c.place(hdr_v2(520000000ULL), 4.2e9);
    EXPECT_NEAR(r2 - r, 400e6, 1.0);                // and spacing is exact again
}

TEST(BurstClock, TriggerPhaseNoLongerWanders) {
    // Same event 0.2 s after each burst's start in device time: it must land at
    // an exactly regular spacing on the timeline whatever the arrival delay is.
    BurstClock c;
    double prev = 0;
    for (int i = 0; i < 5; ++i) {
        const uint64_t dev = 1000000000ULL + uint64_t(i) * 400000000ULL;
        double t0 = c.place(hdr_v2(dev), 1e9 + i * 400e6 + (i % 2 ? 0.8e6 : -0.8e6));
        double trig_time = t0 + 100.0 * 1e9 / 48e6;
        if (i) EXPECT_NEAR(trig_time - prev, 400e6, 1.0);
        prev = trig_time;
    }
}
