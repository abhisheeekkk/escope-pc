#include <gtest/gtest.h>
#include "acquisition/SampleBuffer.h"

using namespace escope;

TEST(SampleBuffer, InitiallyEmpty) {
    SampleBuffer buf(1024);
    EXPECT_EQ(buf.size(), 0u);
    EXPECT_FALSE(buf.has_overflowed());
}

TEST(SampleBuffer, PushAndSize) {
    SampleBuffer buf(1024);
    buf.push(0.0, 1.0f);
    buf.push(100.0, 2.0f);
    EXPECT_EQ(buf.size(), 2u);
}

TEST(SampleBuffer, SnapshotOrdering) {
    SampleBuffer buf(1024);
    for (int i = 0; i < 100; ++i) {
        buf.push(i * 100.0, (float)i);
    }
    std::vector<AnalogSample> snap;
    buf.snapshot(snap);
    ASSERT_EQ(snap.size(), 100u);
    for (int i = 0; i < 100; ++i) {
        EXPECT_NEAR(snap[i].timestamp_ns, i * 100.0, 1e-6);
        EXPECT_NEAR(snap[i].voltage, (float)i, 1e-5f);
    }
}

TEST(SampleBuffer, VoltageRange) {
    SampleBuffer buf(256);
    buf.push(0.0,  -1.5f);
    buf.push(1.0,   0.0f);
    buf.push(2.0,   3.3f);
    auto [vmin, vmax] = buf.voltage_range();
    EXPECT_NEAR(vmin, -1.5f, 1e-5f);
    EXPECT_NEAR(vmax,  3.3f, 1e-5f);
}

TEST(SampleBuffer, Overflow) {
    SampleBuffer buf(8); // capacity rounds to 8
    for (int i = 0; i < 20; ++i) buf.push(i, (float)i);
    EXPECT_TRUE(buf.has_overflowed());
    EXPECT_EQ(buf.size(), buf.capacity());
}

TEST(SampleBuffer, Clear) {
    SampleBuffer buf(64);
    buf.push(0.0, 1.f);
    buf.clear();
    EXPECT_EQ(buf.size(), 0u);
    EXPECT_FALSE(buf.has_overflowed());
}
