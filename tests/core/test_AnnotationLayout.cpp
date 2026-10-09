#include <gtest/gtest.h>
#include "decoders/base/AnnotationLayout.h"
#include "decoders/i2c/I2CDecoder.h"
#include "acquisition/DigitalBuffer.h"

using namespace escope;
using Kind = AnnotationItem::Kind;
using T    = DecodedEvent::Type;

namespace {
DecodedEvent ev(double t0, double t1, T type, const char* label, int value = -1, bool err = false) {
    DecodedEvent e{};
    e.start_ns = t0; e.end_ns = t1; e.label = label; e.type = type; e.value = value;
    e.is_error = err; e.channel = 0;
    return e;
}

// START, address 0x78 (3C W), bytes 0x40 and 0x3E, STOP, starting at t0. A byte is 9 us.
std::vector<DecodedEvent> transfer(double t0, bool nack_last = false) {
    std::vector<DecodedEvent> v;
    v.push_back(ev(t0,          t0,           T::Control, "START"));
    v.push_back(ev(t0 + 5000,   t0 + 14000,   T::Address, "ADDR 0x3C W ACK", 0x78));
    v.push_back(ev(t0 + 14000,  t0 + 23000,   T::Data,    "0x40 ACK", 0x40));
    v.push_back(ev(t0 + 23000,  t0 + 32000,   T::Data,    nack_last ? "0x3E NACK" : "0x3E ACK", 0x3E));
    v.push_back(ev(t0 + 36000,  t0 + 36000,   T::Control, "STOP"));
    return v;
}
std::shared_ptr<const std::vector<DecodedEvent>> share(std::vector<DecodedEvent> v) {
    return std::make_shared<const std::vector<DecodedEvent>>(std::move(v));
}
AnnotationView view(double t0, double t1, double width_px) {
    AnnotationView a; a.t0_ns = t0; a.t1_ns = t1; a.px_per_ns = width_px / (t1 - t0); return a;
}
}

TEST(AnnotationLayout, ZoomedInShowsEveryEventWithHexAndAck) {
    AnnotationIndex idx; idx.build(share(transfer(1000)));
    auto items = idx.items(view(0, 50000, 1500));       // 30 px per us: the transfer is ~1000 px
    ASSERT_EQ(items.size(), 5u);
    EXPECT_EQ(items[0].kind, Kind::Start);   EXPECT_EQ(items[0].text, "S");
    EXPECT_EQ(items[1].kind, Kind::Address); EXPECT_EQ(items[1].text, "3C W"); EXPECT_EQ(items[1].ack, 1);
    EXPECT_EQ(items[2].kind, Kind::Data);    EXPECT_EQ(items[2].text, "40");   EXPECT_EQ(items[2].ack, 1);
    EXPECT_EQ(items[3].kind, Kind::Data);    EXPECT_EQ(items[3].text, "3E");
    EXPECT_EQ(items[4].kind, Kind::Stop);    EXPECT_EQ(items[4].text, "P");
}

TEST(AnnotationLayout, NackAndReadDirectionAreShown) {
    AnnotationIndex idx; idx.build(share(transfer(0, true)));
    auto items = idx.items(view(-1000, 50000, 1500));
    ASSERT_EQ(items.size(), 5u);
    EXPECT_EQ(items[3].ack, 0);
    std::vector<DecodedEvent> r = {ev(0, 0, T::Control, "START"),
                                   ev(100, 9100, T::Address, "ADDR 0x50 R ACK", 0xA1)};
    AnnotationIndex ridx; ridx.build(share(r));
    auto ri = ridx.items(view(-1000, 20000, 1500));
    ASSERT_GE(ri.size(), 2u);
    EXPECT_EQ(ri[1].text, "50 R");
}

TEST(AnnotationLayout, ZoomedOutCollapsesATransferIntoOneSummaryBar) {
    AnnotationIndex idx; idx.build(share(transfer(1000)));
    auto items = idx.items(view(0, 5e6, 1500));         // 0.0003 px per ns: transfer is ~10 px
    ASSERT_EQ(items.size(), 1u);
    EXPECT_EQ(items[0].kind, Kind::Group);
    EXPECT_EQ(items[0].text, "3C W . 2B");
    EXPECT_NE(items[0].detail.find("2 data bytes"), std::string::npos);
}

TEST(AnnotationLayout, LongTransferWithTooNarrowBytesBecomesOneBar) {
    // 36 data bytes of 9 us: 330 us in total. 440 px wide is over the 150 px group limit,
    // but each byte would only be 12 px, too narrow for any text.
    std::vector<DecodedEvent> v = {ev(0, 0, T::Control, "START"),
                                   ev(5000, 14000, T::Address, "ADDR 0x3C W ACK", 0x78)};
    double t = 14000;
    for (int i = 0; i < 36; ++i, t += 9000)
        v.push_back(ev(t, t + 9000, T::Data, "0x3E ACK", 0x3E));
    v.push_back(ev(t + 4000, t + 4000, T::Control, "STOP"));
    AnnotationIndex idx; idx.build(share(v));
    auto narrow = idx.items(view(0, 750000, 1000));          // 12 px per byte
    ASSERT_EQ(narrow.size(), 1u);
    EXPECT_EQ(narrow[0].kind, Kind::Group);
    EXPECT_EQ(narrow[0].text, "3C W . 36B");
    auto wide = idx.items(view(0, 375000, 1000));            // 24 px per byte: text fits
    EXPECT_GT(wide.size(), 30u);
    EXPECT_EQ(wide[2].text, "3E");
}

TEST(AnnotationLayout, OffscreenEventsAreCulled) {
    AnnotationIndex idx; idx.build(share(transfer(1e6)));
    EXPECT_TRUE(idx.items(view(0, 500000, 1500)).empty());
    EXPECT_TRUE(idx.items(view(2e6, 3e6, 1500)).empty());
    EXPECT_FALSE(idx.items(view(1e6, 1.04e6, 1500)).empty());
}

TEST(AnnotationLayout, ViewCuttingATransferKeepsOnlyVisibleBytes) {
    AnnotationIndex idx; idx.build(share(transfer(0)));
    auto items = idx.items(view(15000, 50000, 1500));    // starts inside the 0x40 byte
    ASSERT_EQ(items.size(), 3u);
    EXPECT_EQ(items[0].text, "40");
    EXPECT_EQ(items[2].kind, Kind::Stop);
}

TEST(AnnotationLayout, ManyTransfersFarZoomedOutMergeIntoDenseBars) {
    std::vector<DecodedEvent> all;
    for (int i = 0; i < 2000; ++i) {
        auto t = transfer(i * 100000.0);                 // one every 100 us
        all.insert(all.end(), t.begin(), t.end());
    }
    AnnotationIndex idx; idx.build(share(std::move(all)));
    auto items = idx.items(view(0, 200e6, 1500));        // 200 ms across 1500 px
    EXPECT_LT(items.size(), 1500u);                      // never more than about a pixel each
    bool dense = false;
    for (auto& it : items) dense = dense || it.kind == Kind::Dense;
    EXPECT_TRUE(dense);
    int covered = 0;
    for (auto& it : items) covered += it.count;
    EXPECT_GE(covered, 2000);                            // nothing silently dropped
}

TEST(AnnotationLayout, SpacedOutTransfersStaySeparateGroups) {
    std::vector<DecodedEvent> all;
    for (int i = 0; i < 5; ++i) { auto t = transfer(i * 20e6); all.insert(all.end(), t.begin(), t.end()); }
    AnnotationIndex idx; idx.build(share(std::move(all)));
    auto items = idx.items(view(0, 100e6, 1500));        // 20 ms apart = 300 px apart
    ASSERT_EQ(items.size(), 5u);
    for (auto& it : items) EXPECT_EQ(it.kind, Kind::Group);
}

TEST(AnnotationLayout, UartFramesShowHexAndCharacter) {
    std::vector<DecodedEvent> v = {ev(0, 87000, T::Data, "TX 0x41 'A'", 0x41),
                                   ev(90000, 177000, T::Data, "TX 0x0A", 0x0A)};
    AnnotationIndex idx; idx.build(share(v));
    auto items = idx.items(view(0, 200000, 1500));
    ASSERT_EQ(items.size(), 2u);
    EXPECT_EQ(items[0].text, "41"); EXPECT_EQ(items[0].sub, "A");
    EXPECT_EQ(items[1].text, "0A"); EXPECT_EQ(items[1].sub, "LF");
}

TEST(AnnotationLayout, ErrorsAndMidTransferMarkersAreKept) {
    std::vector<DecodedEvent> v = {ev(0, 0, T::Control, "~"),
                                   ev(100, 9100, T::Data, "0x12 ACK", 0x12),
                                   ev(9100, 9500, T::Error, "[INCOMPLETE]", -1, true)};
    AnnotationIndex idx; idx.build(share(v));
    auto items = idx.items(view(-1000, 20000, 1500));
    ASSERT_EQ(items.size(), 3u);
    EXPECT_EQ(items[0].kind, Kind::Marker);
    EXPECT_EQ(items[2].kind, Kind::Error);
    EXPECT_TRUE(items[2].error);
}

// End to end: real decoder output on a simulated bus, then laid out.
TEST(AnnotationLayout, WorksOnRealDecoderOutput) {
    DigitalBuffer buf(2);
    buf.push_edge(0, 0, true); buf.push_edge(0, 1, true);
    double t = 2000;
    auto step = [&] { t += 5000; };
    buf.push_edge(t, 1, false); step(); buf.push_edge(t, 0, false); step();           // START
    auto bit = [&](bool x, bool& sda) {
        if (x != sda) { buf.push_edge(t, 1, x); sda = x; }
        step(); buf.push_edge(t, 0, true); step(); buf.push_edge(t, 0, false); step();
    };
    bool sda = false;
    for (uint8_t b : {0x78, 0x40}) {
        for (int i = 7; i >= 0; --i) bit((b >> i) & 1, sda);
        bit(false, sda);
    }
    if (sda) { buf.push_edge(t, 1, false); sda = false; }
    buf.push_edge(t + 5000, 0, true); buf.push_edge(t + 10000, 1, true);               // STOP

    I2CDecoder dec;
    auto events = dec.decode(buf, {{"SCL", 0}, {"SDA", 1}});
    AnnotationIndex idx; idx.build(share(events));
    auto items = idx.items(view(0, t + 20000, 2000));
    ASSERT_EQ(items.size(), 4u);        // S, 3C W, 40, P
    EXPECT_EQ(items[1].text, "3C W");
    EXPECT_EQ(items[2].text, "40");
}

TEST(AnnotationLayout, CanFrameMapsToMarkersAndFields) {
    DecodedEvent sof{}; sof.type = DecodedEvent::Type::Control; sof.label = "SOF";
    DecodedEvent eof = sof; eof.label = "EOF";
    DecodedEvent id{}; id.type = DecodedEvent::Type::Address; id.label = "ID 0x123"; id.value = 0x123;
    DecodedEvent dlc{}; dlc.type = DecodedEvent::Type::Annotation; dlc.label = "DLC 8";
    EXPECT_EQ(make_annotation_item(sof, 0).kind, AnnotationItem::Kind::Start);
    EXPECT_EQ(make_annotation_item(eof, 1).kind, AnnotationItem::Kind::Stop);
    auto a = make_annotation_item(id, 2);
    EXPECT_EQ(a.kind, AnnotationItem::Kind::Address);
    EXPECT_EQ(a.text, "0x123");
    EXPECT_EQ(make_annotation_item(dlc, 3).kind, AnnotationItem::Kind::Field);
}

TEST(AnnotationLayout, SpiEventsMapToMarkersCommandsAndData) {
    DecodedEvent cs{}; cs.type = DecodedEvent::Type::Control; cs.label = "CS";
    DecodedEvent rel = cs; rel.label = "/CS";
    DecodedEvent cmd{}; cmd.type = DecodedEvent::Type::Address; cmd.label = "CMD 0x2A"; cmd.value = 0x2A;
    EXPECT_EQ(make_annotation_item(cs, 0).kind, AnnotationItem::Kind::Start);
    EXPECT_EQ(make_annotation_item(rel, 1).kind, AnnotationItem::Kind::Stop);
    auto c = make_annotation_item(cmd, 2);
    EXPECT_EQ(c.kind, AnnotationItem::Kind::Address);
    EXPECT_EQ(c.text, "CMD 2A");
}
