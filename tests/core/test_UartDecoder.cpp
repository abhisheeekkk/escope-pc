#include <gtest/gtest.h>
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
