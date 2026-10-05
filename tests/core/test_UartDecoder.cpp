#include <gtest/gtest.h>
#include <cmath>
#include "decoders/uart/UartDecoder.h"
#include "decoders/base/DecoderRegistry.h"
#include "acquisition/DigitalBuffer.h"

using namespace escope;

// Helper: inject a UART byte into a DigitalBuffer at the given start time
// Idle = high, start bit = low, LSB first, stop bit = high
static void push_uart_byte(DigitalBuffer& buf, uint8_t ch,
                            double start_ns, uint32_t baud, uint8_t byte_val) {
    const double bit_ns = 1e9 / baud;
    double t = start_ns;

    // Start bit (falling)
    buf.push_edge(t, ch, false);
    t += bit_ns;

    // Data bits LSB first
    bool prev = false;
    for (int b = 0; b < 8; ++b) {
        bool level = (byte_val >> b) & 1;
        if (level != prev) {
            buf.push_edge(t, ch, level);
            prev = level;
        }
        t += bit_ns;
    }

    // Stop bit (rising if last data bit was low)
    if (!prev) buf.push_edge(t, ch, true);
    // (if prev was already high, no edge needed — line stays high)
}

TEST(UartDecoder, DecodesASCIIByte) {
    DigitalBuffer buf(2);
    // Idle high initially
    buf.push_edge(0.0, 0, true);

    push_uart_byte(buf, 0, 1000.0, 115200, 'A'); // 'A' = 0x41

    UartDecoder dec;
    dec.configure({{"baud", "115200"}});

    std::vector<DecoderChannelMap> chmap = {{"TX", 0}};
    auto events = dec.decode(buf, chmap);

    ASSERT_FALSE(events.empty());
    // Should contain 'A' / 0x41
    bool found = false;
    for (const auto& e : events) {
        if (e.label.find("41") != std::string::npos ||
            e.label.find("'A'") != std::string::npos) {
            found = true;
        }
    }
    EXPECT_TRUE(found) << "Expected 0x41/'A' in decoded events";
}

TEST(UartDecoder, DecodesMultipleBytes) {
    DigitalBuffer buf(2);
    buf.push_edge(0.0, 0, true); // idle high

    const uint32_t baud = 115200;
    const double   bit_ns = 1e9 / baud;
    const double   frame_ns = 10 * bit_ns; // 1 start + 8 data + 1 stop

    const std::string msg = "Hi";
    double t = 5000.0;
    for (char c : msg) {
        push_uart_byte(buf, 0, t, baud, static_cast<uint8_t>(c));
        t += frame_ns * 1.5; // inter-frame gap
    }

    UartDecoder dec;
    dec.configure({{"baud", "115200"}});
    std::vector<DecoderChannelMap> chmap = {{"TX", 0}};
    auto events = dec.decode(buf, chmap);

    // Should have decoded at least 2 events
    EXPECT_GE(events.size(), 2u);
}

TEST(UartDecoder, EmptyBufferReturnsNoEvents) {
    DigitalBuffer buf(2);
    UartDecoder dec;
    dec.configure({{"baud", "115200"}});
    std::vector<DecoderChannelMap> chmap = {{"TX", 0}};
    auto events = dec.decode(buf, chmap);
    EXPECT_TRUE(events.empty());
}

TEST(UartDecoder, EventsAreSortedByTime) {
    DigitalBuffer buf(2);
    buf.push_edge(0.0, 0, true);

    const uint32_t baud   = 9600;
    const double   bit_ns = 1e9 / baud;
    const double   frame_ns = 10 * bit_ns;
    double t = 1000.0;
    for (uint8_t c : {0x55u, 0xAAu, 0x0Fu}) {
        push_uart_byte(buf, 0, t, baud, c);
        t += frame_ns * 2.0;
    }

    UartDecoder dec;
    dec.configure({{"baud", "9600"}});
    std::vector<DecoderChannelMap> chmap = {{"TX", 0}};
    auto events = dec.decode(buf, chmap);

    for (std::size_t i = 1; i < events.size(); ++i) {
        EXPECT_LE(events[i-1].start_ns, events[i].start_ns);
    }
}

TEST(UartDecoder, DecoderRegistered) {
    // Verify REGISTER_DECODER macro worked
    auto& reg = DecoderRegistry::instance();
    auto names = reg.available();
    bool found = false;
    for (const auto& n : names) {
        if (n == "UART") { found = true; break; }
    }
    EXPECT_TRUE(found) << "UART decoder should be in registry";
}

TEST(UartDecoder, DetectsBaudRate) {
    for (uint32_t baud : {9600u, 115200u, 500000u, 921600u}) {
        DigitalBuffer buf(8);
        double t = 1000.0;
        for (int i = 0; i < 10; ++i) {
            push_uart_byte(buf, 0, t, baud, 'U');
            t += 20.0 * 1e9 / baud;
        }
        EXPECT_EQ(UartDecoder::detect_baud(buf, 0), baud);
    }
    DigitalBuffer empty(8);
    EXPECT_EQ(UartDecoder::detect_baud(empty, 0), 0u);
}

// The capture arrives as bursts (about 4.8 ms of samples, then a gap while the burst is
// uploaded), so a burst almost always starts and ends in the MIDDLE of a byte. A start
// bit must not be taken from a data edge there, and a byte cut off by the end of a burst
// must not be decoded from the line level that happened to be held.
TEST(UartDecoder, BurstsStartingMidFrameGiveNoFramingErrors) {
    constexpr uint32_t baud = 115200;
    const double bit = 1e9 / baud;
    const std::string msg = "Hello World";

    // Whole transmission as (time, level) transitions, one message every ~1.3 ms.
    struct Tr { double t; bool level; };
    std::vector<Tr> line;
    std::vector<std::pair<double, uint8_t>> frames;   // start time, byte
    bool cur = true;
    auto set = [&](double t, bool l) { if (l != cur) { line.push_back({t, l}); cur = l; } };
    double t = 20000.0;
    for (int rep = 0; rep < 40; ++rep) {
        for (char c : msg) {
            frames.push_back({t, static_cast<uint8_t>(c)});
            set(t, false); t += bit;
            for (int b = 0; b < 8; ++b) { set(t, (c >> b) & 1); t += bit; }
            set(t, true); t += bit;
        }
        t += 40000.0;                                   // idle between messages
    }
    const double end = t;

    // Cut it into bursts the way the device does: a level snapshot at the start of each
    // burst, then the real transitions inside it.
    DigitalBuffer buf(1);
    std::vector<DigitalEdge> batch;
    auto level_at = [&](double tt) { bool l = true; for (const auto& x : line) { if (x.t <= tt) l = x.level; else break; } return l; };
    for (double b0 = 5000.0; b0 + 4.8e6 < end; b0 += 4.8e6 + 3.1e6 + 777.0) {   // odd gap: varied alignment
        const double b1 = b0 + 4.8e6;
        batch.push_back({b0, 0, level_at(b0), true});
        for (const auto& x : line) if (x.t > b0 && x.t <= b1) batch.push_back({x.t, 0, x.level, false});
    }
    buf.push_batch(batch.data(), batch.size());

    UartDecoder dec;
    dec.configure({{"baud", "115200"}});
    auto ev = dec.decode(buf, {{"RX", 0}});

    int good = 0;
    for (const auto& e : ev) {
        EXPECT_FALSE(e.is_error) << "framing error at " << e.start_ns;
        if (e.is_error) continue;
        bool matched = false;
        for (const auto& f : frames)
            if (std::abs(f.first - e.start_ns) < bit * 0.5) { matched = true; EXPECT_EQ(e.value, f.second) << "wrong byte at " << e.start_ns; }
        EXPECT_TRUE(matched) << "decoded a frame that was never sent, at " << e.start_ns;
        ++good;
    }
    EXPECT_GT(good, 100);                               // most of the message still comes through
}

// Every burst starts with a level snapshot, and the device triggers on an edge right at
// the start of the burst, so the first real edge lands one sample (20.8 ns at 48 MS/s)
// after the snapshot in every burst. That gap is not a bit time and must not be used for
// auto baud (it used to read as 48 Mbaud, and every frame then failed).
TEST(UartDecoder, AutoBaudIgnoresSnapshotToFirstEdgeGap) {
    constexpr double sample = 1e9 / 48e6;
    const double bit = 2000.0;                          // 500 kbaud
    DigitalBuffer buf(1);
    std::vector<DigitalEdge> batch;
    for (int burst = 0; burst < 40; ++burst) {
        const double t0 = burst * 9e6;
        batch.push_back({t0, 0, true, true});            // snapshot: line idle high
        double t = t0 + sample;                          // first edge one sample in
        bool level = true;
        for (int i = 0; i < 30; ++i) {                   // alternating bits of 1..3 bit times
            level = !level;
            batch.push_back({t, 0, level, false});
            t += bit * (1 + (i % 3));
        }
    }
    buf.push_batch(batch.data(), batch.size());
    EXPECT_EQ(UartDecoder::detect_baud(buf, 0), 500000u);
}
