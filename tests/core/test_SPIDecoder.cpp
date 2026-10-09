#include <gtest/gtest.h>
#include "decoders/spi/SPIDecoder.h"
#include "acquisition/DigitalBuffer.h"

using namespace escope;

namespace {
constexpr uint8_t CLK = 0, MOSI = 1, MISO = 2, CS = 3, DC = 4;

// A simulated SPI master. One bit lasts `bit` ns; data changes `hold` ns after the shifting edge.
struct Master {
    DigitalBuffer buf{5};
    double t = 1000, bit = 1000, hold = 10;
    int cpol, cpha;
    bool lvl[5] = {false, false, false, true, true};

    Master(int cpol_, int cpha_, bool with_cs_idle_high = true) : cpol(cpol_), cpha(cpha_) {
        lvl[CLK] = cpol; lvl[CS] = with_cs_idle_high;
        for (int c = 0; c < 5; ++c) buf.push_edge(0, c, lvl[c]);
    }
    void set(uint8_t ch, bool v) { if (lvl[ch] != v) { lvl[ch] = v; buf.push_edge(t, ch, v); } }
    void cs(bool active) { set(CS, !active); t += bit; }
    // data is stable well before the sampling edge; the clock edges are `bit/2` apart
    void send(uint32_t mosi, uint32_t miso, int bits = 8, bool msb = true) {
        for (int i = 0; i < bits; ++i) {
            const int k = msb ? bits - 1 - i : i;
            const bool m = (mosi >> k) & 1, s = (miso >> k) & 1;
            if (cpha == 0) {                       // data first, then the leading (sampling) edge
                set(MOSI, m); set(MISO, s);
                t += bit / 2;
                set(CLK, !cpol);
                t += bit / 2;
                set(CLK, cpol);
            } else {                               // leading edge shifts, trailing edge samples
                set(CLK, !cpol);
                t += hold;
                set(MOSI, m); set(MISO, s);
                t += bit / 2 - hold;
                set(CLK, cpol);
                t += bit / 2;
            }
        }
    }
};

std::vector<DecodedEvent> run(Master& m, SPIDecoder& d, bool use_cs = true, bool use_dc = false, bool use_miso = true) {
    std::vector<DecoderChannelMap> ch = {{"CLK", CLK}, {"MOSI", MOSI}};
    if (use_miso) ch.push_back({"MISO", MISO});
    if (use_cs) ch.push_back({"CS", CS});
    if (use_dc) ch.push_back({"DC", DC});
    return d.decode(m.buf, ch);
}

std::vector<int> values(const std::vector<DecodedEvent>& ev, uint8_t ch, DecodedEvent::Type type) {
    std::vector<int> v;
    for (auto& e : ev) if (e.channel == ch && e.type == type) v.push_back(e.value);
    return v;
}
}

TEST(SPIDecoder, AllFourModesAreDetectedAndDecoded) {
    for (int mode = 0; mode < 4; ++mode) {
        Master m(mode >> 1, mode & 1);
        m.cs(true);
        m.send(0xA5, 0x3C);
        m.send(0x00, 0xFF);
        m.send(0x81, 0x00);
        m.cs(false);
        SPIDecoder d;
        auto ev = run(m, d);
        EXPECT_EQ(d.mode_used(), mode) << "mode " << mode;
        EXPECT_EQ((values(ev, MOSI, DecodedEvent::Type::Data)), (std::vector<int>{0xA5, 0x00, 0x81})) << "mode " << mode;
        EXPECT_EQ((values(ev, MISO, DecodedEvent::Type::Data)), (std::vector<int>{0x3C, 0xFF, 0x00})) << "mode " << mode;
        EXPECT_EQ(d.errors(), 0u);
    }
}

TEST(SPIDecoder, ClockRateIsMeasured) {
    Master m(0, 0);
    m.bit = 2000;                    // 500 kHz
    m.cs(true); m.send(0x55, 0x00); m.cs(false);
    SPIDecoder d; run(m, d);
    EXPECT_NEAR(d.clock_hz(), 500e3, 5e3);
}

TEST(SPIDecoder, CommandsAreSeparatedByTheDcLine) {
    Master m(0, 0);
    m.cs(true);
    m.set(DC, false); m.send(0x2A, 0);          // command
    m.set(DC, true);  m.send(0x00, 0); m.send(0xEF, 0);   // two data bytes
    m.cs(false);
    SPIDecoder d;
    auto ev = run(m, d, true, true);
    EXPECT_EQ((values(ev, MOSI, DecodedEvent::Type::Address)), (std::vector<int>{0x2A}));
    EXPECT_EQ((values(ev, MOSI, DecodedEvent::Type::Data)), (std::vector<int>{0x00, 0xEF}));
    bool saw = false;
    for (auto& e : ev) if (e.type == DecodedEvent::Type::Address) { EXPECT_EQ(e.label, "CMD 0x2A"); saw = true; }
    EXPECT_TRUE(saw);
}

TEST(SPIDecoder, ChipSelectWindowsAreMarked) {
    Master m(0, 0);
    m.cs(true); m.send(0x11, 0); m.cs(false);
    m.t += 5000;
    m.cs(true); m.send(0x22, 0); m.send(0x33, 0); m.cs(false);
    SPIDecoder d;
    auto ev = run(m, d);
    int starts = 0, stops = 0;
    for (auto& e : ev) if (e.channel == MOSI && e.type == DecodedEvent::Type::Control) { starts += e.label == "CS"; stops += e.label == "/CS"; }
    EXPECT_EQ(starts, 2);
    EXPECT_EQ(stops, 2);
}

TEST(SPIDecoder, WithoutChipSelectBytesStillAlign) {
    Master m(0, 0);
    m.send(0xDE, 0); m.send(0xAD, 0);
    m.t += 100000;                    // long pause
    m.send(0xBE, 0); m.send(0xEF, 0);
    SPIDecoder d;
    auto ev = run(m, d, false);
    EXPECT_EQ((values(ev, MOSI, DecodedEvent::Type::Data)), (std::vector<int>{0xDE, 0xAD, 0xBE, 0xEF}));
}

TEST(SPIDecoder, ShortWordWhenChipSelectEndsEarlyIsAnError) {
    Master m(0, 0);
    m.cs(true); m.send(0xA5, 0); m.send(0x7, 0, 3); m.cs(false);
    SPIDecoder d;
    auto ev = run(m, d);
    int errs = 0;
    for (auto& e : ev) if (e.type == DecodedEvent::Type::Error && e.channel == MOSI) { ++errs; EXPECT_EQ(e.label, "[3 BITS]"); }
    EXPECT_EQ(errs, 1);
}

TEST(SPIDecoder, LsbFirstAndSixteenBitWords) {
    Master m(0, 0);
    m.cs(true); m.send(0x1234, 0, 16, false); m.cs(false);
    SPIDecoder d; d.configure({{"bits", "16"}, {"lsb", "1"}});
    auto ev = run(m, d);
    EXPECT_EQ((values(ev, MOSI, DecodedEvent::Type::Data)), (std::vector<int>{0x1234}));
}

TEST(SPIDecoder, FastClockWithTightHoldStillDecodes) {
    Master m(0, 0);
    m.bit = 125;                      // 8 MHz
    m.cs(true); m.send(0xF0, 0x0F); m.send(0x5A, 0xA5); m.cs(false);
    SPIDecoder d;
    auto ev = run(m, d);
    EXPECT_EQ((values(ev, MOSI, DecodedEvent::Type::Data)), (std::vector<int>{0xF0, 0x5A}));
    EXPECT_EQ((values(ev, MISO, DecodedEvent::Type::Data)), (std::vector<int>{0x0F, 0xA5}));
}

TEST(SPIDecoder, MissingLinesGiveNothing) {
    Master m(0, 0);
    SPIDecoder d;
    EXPECT_TRUE(d.decode(m.buf, {{"CLK", CLK}}).empty());
}

TEST(SPIDecoder, BurstThatStartsInsideATransactionIsMarkedAndDecoded) {
    Master m(0, 0);
    m.cs(true); m.send(0x11, 0); m.cs(false);
    m.t += 1e6;                                   // a gap nobody observed
    // next burst: per-line level snapshots, chip select already active
    const bool levels[5] = {false, false, false, false, true};
    for (uint8_t c = 0; c < 5; ++c) {
        DigitalEdge e{m.t, c, levels[c], true};
        m.buf.push_batch(&e, 1);
        m.lvl[c] = levels[c];
    }
    m.t += 1000;
    m.send(0xC3, 0);
    m.cs(false);
    SPIDecoder d;
    auto ev = run(m, d);
    EXPECT_EQ((values(ev, MOSI, DecodedEvent::Type::Data)), (std::vector<int>{0x11, 0xC3}));
    bool mid = false;
    for (auto& e : ev) if (e.type == DecodedEvent::Type::Control && e.label == "~") mid = true;
    EXPECT_TRUE(mid);
}

TEST(SPIDecoder, SpikeOnDataAtTheClockEdgeIsNotRead) {
    // a 1 -> 0 spike on MOSI that starts one sample before each rising clock edge and ends just after it
    Master m(0, 0);
    m.cs(true);
    const uint8_t v = 0xFF;
    for (int i = 0; i < 8; ++i) {
        m.set(MOSI, true);
        m.t += m.bit / 2 - 20;
        m.set(MOSI, false);                 // spike starts 20 ns before the edge
        m.t += 20;
        m.set(CLK, true);
        m.t += 20;
        m.set(MOSI, true);                  // and ends 20 ns after it
        m.t += m.bit / 2 - 20;
        m.set(CLK, false);
    }
    m.cs(false);
    SPIDecoder d; d.configure({{"mode", "0"}});
    auto ev = run(m, d, true, false, false);
    EXPECT_EQ((values(ev, MOSI, DecodedEvent::Type::Data)), (std::vector<int>{v}));
}

TEST(SPIDecoder, ShortGlitchPulsesOnTheClockAreIgnored) {
    // 40 ns pulses on SCK while it is low (crosstalk): each would be read as an extra bit and shift
    // every byte after it
    Master m(0, 0);
    m.cs(true);
    for (uint8_t v : {0x2A, 0x00, 0x62, 0x73}) {
        for (int i = 7; i >= 0; --i) {
            m.set(MOSI, (v >> i) & 1);
            if (i == 3) {                                           // a glitch before one bit in every byte
                m.t += m.bit / 4;
                m.set(CLK, true); m.t += 40; m.set(CLK, false);
                m.t += m.bit / 4;
            } else {
                m.t += m.bit / 2;
            }
            m.set(CLK, true); m.t += m.bit / 2; m.set(CLK, false);  // the real clock pulse
        }
    }
    m.cs(false);
    SPIDecoder d; d.configure({{"mode", "0"}});
    auto ev = run(m, d, true, false, false);
    EXPECT_EQ((values(ev, MOSI, DecodedEvent::Type::Data)), (std::vector<int>{0x2A, 0x00, 0x62, 0x73}));
}

TEST(SPIDecoder, WithoutChipSelectAMiscountedBitIsRecoveredAtTheNextPause) {
    Master m(0, 0);
    m.send(0x2A, 0);
    // a second byte with one extra, wide clock pulse (not short enough to be filtered)
    for (int i = 7; i >= 0; --i) {
        m.set(MOSI, (0x55 >> i) & 1); m.t += m.bit / 2; m.set(CLK, true); m.t += m.bit / 2; m.set(CLK, false);
        if (i == 4) { m.t += m.bit / 4; m.set(CLK, true); m.t += m.bit / 3; m.set(CLK, false); m.t += m.bit / 4; }
    }
    m.t += 20 * m.bit;                                  // the master pauses (CS and DC switching)
    m.send(0xAB, 0);
    m.send(0xCD, 0);
    SPIDecoder d; d.configure({{"mode", "0"}});
    auto ev = run(m, d, false, false, false);
    auto v = values(ev, MOSI, DecodedEvent::Type::Data);
    ASSERT_GE(v.size(), 3u);
    EXPECT_EQ(v[v.size() - 2], 0xAB);
    EXPECT_EQ(v[v.size() - 1], 0xCD);
    bool err = false;
    for (auto& e : ev) err = err || e.type == DecodedEvent::Type::Error;
    EXPECT_TRUE(err);
}
