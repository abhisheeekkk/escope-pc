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

TEST(DigitalBuffer, StoreMaskDropsUnselectedChannels) {
    DigitalBuffer buf(8);
    buf.set_store_mask(0b00000101);                       // record D0 and D2 only
    const DigitalEdge edges[] = {
        {10.0, 0, true, false}, {11.0, 1, true, false}, {12.0, 2, true, false}, {13.0, 7, true, false},
    };
    buf.push_batch(edges, 4);
    buf.push_edge(14.0, 1, false);
    EXPECT_EQ(buf.edge_count(0), 1u);
    EXPECT_EQ(buf.edge_count(1), 0u);
    EXPECT_EQ(buf.edge_count(2), 1u);
    EXPECT_EQ(buf.edge_count(7), 0u);
    buf.set_store_mask(0xFF);
    buf.push_edge(15.0, 1, true);
    EXPECT_EQ(buf.edge_count(1), 1u);
}

TEST(DigitalBuffer, ColumnsSummariseDenseEdges) {
    DigitalBuffer buf(8);
    for (int i = 0; i < 1000; ++i) buf.push_edge(i * 10.0, 0, (i & 1) == 0);   // 1000 edges over 10 us
    std::vector<DigitalBuffer::EdgeColumn> cols;
    buf.columns(0, 0.0, 10000.0, 10, cols);                                    // 10 columns of 100 edges
    ASSERT_EQ(cols.size(), 10u);
    uint32_t total = 0;
    for (const auto& c : cols) total += c.count;
    EXPECT_EQ(total, 1000u);
    buf.columns(0, 0.0, 10000.0, 1000, cols);                                  // one edge per column
    EXPECT_EQ(cols.size(), 1000u);
    EXPECT_EQ(cols.front().count, 1u);
}
