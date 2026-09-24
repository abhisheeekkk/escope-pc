#include <gtest/gtest.h>
#include "acquisition/DigitalBuffer.h"

using namespace escope;

TEST(DigitalBuffer, InitiallyEmpty) {
    DigitalBuffer buf(8);
    EXPECT_EQ(buf.total_edges(), 0u);
}

TEST(DigitalBuffer, PushAndCount) {
    DigitalBuffer buf(4);
    buf.push_edge(100.0, 0, true);
    buf.push_edge(200.0, 0, false);
    buf.push_edge(300.0, 1, true);
    EXPECT_EQ(buf.edge_count(0), 2u);
    EXPECT_EQ(buf.edge_count(1), 1u);
    EXPECT_EQ(buf.total_edges(), 3u);
}

TEST(DigitalBuffer, LevelAt) {
    DigitalBuffer buf(4);
    buf.push_edge(100.0, 0, true);   // rises at 100 ns
    buf.push_edge(500.0, 0, false);  // falls at 500 ns

    EXPECT_FALSE(buf.level_at(0,  50.0)); // before any edge → low
    EXPECT_TRUE (buf.level_at(0, 200.0)); // after rise → high
    EXPECT_FALSE(buf.level_at(0, 600.0)); // after fall → low
}

TEST(DigitalBuffer, EdgesInRange) {
    DigitalBuffer buf(4);
    for (int i = 0; i < 10; ++i) {
        buf.push_edge(i * 100.0, 0, i % 2 == 0);
    }
    auto edges = buf.edges_in_range(200.0, 500.0);
    EXPECT_EQ(edges.size(), 4u); // t=200,300,400,500
}

TEST(DigitalBuffer, Clear) {
    DigitalBuffer buf(4);
    buf.push_edge(0.0, 0, true);
    buf.clear();
    EXPECT_EQ(buf.total_edges(), 0u);
}
