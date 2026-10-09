// Regression tests for the protocol decoders against saved capture sessions.
//
// tests/fixtures/<name>/ holds a session written by SessionSerializer (so the loader is exercised too)
// and tests/fixtures/<name>.golden holds the decoded events that session must keep producing.
// Besides the golden comparison every case asserts the payload it knows is on the wire, so a golden file
// can never quietly bless wrong output.
//
// Replacing a fixture with a real capture: save the session from the app into tests/fixtures/<name>,
// then run   ESCOPE_UPDATE_GOLDEN=1 ./escope_tests --gtest_filter='DecoderFixtures*'   and review the diff.
// To rebuild the synthetic fixtures from scratch use ESCOPE_REGEN_FIXTURES=1.
#include <gtest/gtest.h>
#include "decoders/base/DecoderRegistry.h"
#include "decoders/can/CANDecoder.h"
#include "session/CaptureSession.h"
#include "session/SessionSerializer.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace escope;
namespace fs = std::filesystem;

namespace {

constexpr double kSampleNs = 1e9 / 48e6;      // the capture grid of the board
constexpr double kT0       = 1e6;             // first burst starts at 1 ms

using Params = std::vector<std::pair<std::string, std::string>>;
using Map    = std::vector<DecoderChannelMap>;

struct Scenario {
    std::string name, decoder;
    Params      params;
    Map         map;
};

// Wire builder: edges land on the sample grid like they do on the board.
struct Wire {
    std::unique_ptr<CaptureSession> s = std::make_unique<CaptureSession>();
    bool lvl[8];
    double t = kT0;
    std::vector<DigitalEdge> edges;

    explicit Wire(std::initializer_list<uint8_t> idle_high, int channels = 4) {
        for (int c = 0; c < 8; ++c) lvl[c] = false;
        for (uint8_t c : idle_high) lvl[c] = true;
        for (int c = 0; c < channels; ++c) edges.push_back({kT0, uint8_t(c), lvl[c], true});   // burst snapshot
        t = kT0 + 100 * kSampleNs;
    }
    static double snap(double t) { return kT0 + std::round((t - kT0) / kSampleNs) * kSampleNs; }
    void set(uint8_t ch, bool v) { if (lvl[ch] != v) { lvl[ch] = v; edges.push_back({snap(t), ch, v, false}); } }
    void wait(double ns) { t += ns; }

    void finish() {
        auto& b = s->digital_buffer();
        b.push_batch(edges.data(), edges.size());
        b.set_burst_ends({snap(t) + 1000.0});
        b.mark_trigger(kT0);
    }
};

// ---------- I2C: EEPROM page write, then random read with a repeated start (100 kHz) ----------
std::unique_ptr<CaptureSession> make_i2c() {
    Wire w({0, 1}, 2);
    const double H = 5000;                                   // half period
    auto start = [&] { w.set(1, true); w.set(0, true); w.wait(H); w.set(1, false); w.wait(H); w.set(0, false); w.wait(H); };
    auto stop  = [&] { w.set(1, false); w.wait(H); w.set(0, true); w.wait(H); w.set(1, true); w.wait(2 * H); };
    auto bit   = [&](bool b) { w.set(1, b); w.wait(H); w.set(0, true); w.wait(H); w.set(0, false); w.wait(H / 2); };
    auto byte  = [&](uint8_t v, bool ack) { for (int i = 7; i >= 0; --i) bit((v >> i) & 1); bit(!ack); };
    w.wait(20 * H);
    start(); byte(0xA0, true); byte(0x01, true); byte(0x00, true); byte(0xDE, true); byte(0xAD, true); stop();
    w.wait(40 * H);
    start(); byte(0xA0, true); byte(0x01, true); byte(0x00, true);
    start(); byte(0xA1, true); byte(0xDE, true); byte(0xAD, false); stop();
    w.wait(20 * H);
    start(); byte(0x68 << 1, false); stop();                 // nobody home: address NACK
    w.finish();
    return std::move(w.s);
}

// ---------- SPI mode 0: a flash chip, JEDEC ID then a 3-byte-address read (1 MHz) ----------
std::unique_ptr<CaptureSession> make_spi() {
    Wire w({3}, 4);                                          // CS idles high
    const double bit = 1000;
    auto xfer = [&](uint8_t out, uint8_t in) {
        for (int i = 7; i >= 0; --i) {
            w.set(1, (out >> i) & 1); w.set(2, (in >> i) & 1);
            w.wait(bit / 2); w.set(0, true); w.wait(bit / 2); w.set(0, false);
        }
    };
    w.wait(20 * bit);
    w.set(3, false); w.wait(2 * bit);
    xfer(0x9F, 0x00); xfer(0x00, 0xEF); xfer(0x00, 0x40); xfer(0x00, 0x18);
    w.wait(bit); w.set(3, true); w.wait(20 * bit);
    w.set(3, false); w.wait(2 * bit);
    xfer(0x03, 0x00); xfer(0x00, 0x00); xfer(0x10, 0x00); xfer(0x00, 0x00);
    xfer(0x00, 0xCA); xfer(0x00, 0xFE);
    w.wait(bit); w.set(3, true); w.wait(20 * bit);
    w.finish();
    return std::move(w.s);
}

// ---------- UART 115200 8N1: a text line ----------
const char* kUartText = "Hello, EmbeddedScope\r\n";
std::unique_ptr<CaptureSession> make_uart() {
    Wire w({0}, 1);
    const double bit = 1e9 / 115200.0;
    w.wait(50 * bit);
    for (const char* p = kUartText; *p; ++p) {
        const uint8_t v = uint8_t(*p);
        w.set(0, false); w.wait(bit);
        for (int i = 0; i < 8; ++i) { w.set(0, (v >> i) & 1); w.wait(bit); }
        w.set(0, true); w.wait(bit * 1.5);                   // a little gap between characters
    }
    w.wait(20 * bit);
    w.finish();
    return std::move(w.s);
}

// ---------- CAN 500 kbit/s: standard, extended (DroneCAN style), remote and a damaged frame ----------
void push_bits(std::vector<uint8_t>& v, uint32_t x, int n) { for (int i = n - 1; i >= 0; --i) v.push_back((x >> i) & 1); }

std::vector<uint8_t> can_frame(uint32_t id, bool ext, bool rtr, const std::vector<uint8_t>& data, bool bad_crc = false) {
    std::vector<uint8_t> b = {0};
    if (!ext) { push_bits(b, id, 11); b.push_back(rtr); b.push_back(0); b.push_back(0); }
    else {
        push_bits(b, id >> 18, 11); b.push_back(1); b.push_back(1);
        push_bits(b, id & 0x3FFFF, 18); b.push_back(rtr); b.push_back(0); b.push_back(0);
    }
    push_bits(b, uint32_t(data.size()), 4);
    for (uint8_t d : data) push_bits(b, d, 8);
    uint16_t crc = CANDecoder::crc15(b);   // checked against an independent long division in test_CANDecoder
    if (bad_crc) crc ^= 0x0010;
    push_bits(b, crc, 15);
    std::vector<uint8_t> w; int run = 0; uint8_t last = 2;   // bit stuffing over SOF..CRC
    for (uint8_t x : b) {
        w.push_back(x);
        run = (x == last) ? run + 1 : 1; last = x;
        if (run == 5) { w.push_back(!x); last = !x; run = 1; }
    }
    w.push_back(1); w.push_back(0); w.push_back(1);          // CRC delimiter, ACK slot (dominant), ACK delimiter
    w.insert(w.end(), 7, 1);                                 // EOF
    return w;
}

std::unique_ptr<CaptureSession> make_can() {
    Wire w({0}, 1);
    const double BIT = 2000;
    auto send = [&](const std::vector<uint8_t>& bits) { for (uint8_t b : bits) { w.set(0, b); w.wait(BIT); } };
    send(std::vector<uint8_t>(20, 1));
    send(can_frame(0x123, false, false, {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88}));
    send(std::vector<uint8_t>(6, 1));
    send(can_frame(0x1F00AB12 & 0x1FFFFFFF, true, false, {0xDE, 0xAD, 0xBE, 0xEF}));
    send(std::vector<uint8_t>(6, 1));
    send(can_frame(0x7FF, false, true, {}));
    send(std::vector<uint8_t>(6, 1));
    send(can_frame(0x055, false, false, {0x01, 0x02}, true));   // CRC error
    send(std::vector<uint8_t>(20, 1));
    w.finish();
    return std::move(w.s);
}

const std::vector<Scenario>& scenarios() {
    static const std::vector<Scenario> v = {
        {"i2c_eeprom",   "I2C",  {},                  {{"SCL", 0}, {"SDA", 1}}},
        {"spi_flash",    "SPI",  {{"mode", "auto"}},  {{"CLK", 0}, {"MOSI", 1}, {"MISO", 2}, {"CS", 3}}},
        {"uart_text",    "UART", {{"baud", "115200"}}, {{"TX", 0}}},
        {"can_frames",   "CAN",  {{"bitrate", "500000"}}, {{"RX", 0}}},
    };
    return v;
}

std::unique_ptr<CaptureSession> build(const std::string& name) {
    if (name == "i2c_eeprom") return make_i2c();
    if (name == "spi_flash")  return make_spi();
    if (name == "uart_text")  return make_uart();
    return make_can();
}

std::vector<DecodedEvent> decode(const Scenario& sc, const CaptureSession& s) {
    auto d = DecoderRegistry::instance().create(sc.decoder);
    if (!d) return {};
    d->configure(sc.params);
    return d->decode(s.digital_buffer(), sc.map);
}

std::string dump(const std::vector<DecodedEvent>& ev) {
    std::ostringstream o;
    char line[512];
    for (const auto& e : ev) {
        std::snprintf(line, sizeof line, "%.1f %.1f ch%d t%d e%d v%d | %s | %s\n", e.start_ns, e.end_ns, int(e.channel),
                      int(e.type), int(e.is_error), e.value, e.label.c_str(), e.detail.c_str());
        o << line;
    }
    return o.str();
}

fs::path fixture_dir() { return fs::path(ESCOPE_FIXTURE_DIR); }
bool env(const char* n) { const char* v = std::getenv(n); return v && *v && *v != '0'; }

std::string slurp(const fs::path& p) { std::ifstream f(p, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }

std::vector<DecodedEvent> by_type(const std::vector<DecodedEvent>& ev, DecodedEvent::Type t) {
    std::vector<DecodedEvent> o;
    for (auto& e : ev) if (e.type == t) o.push_back(e);
    return o;
}
bool has(const std::vector<DecodedEvent>& ev, const std::string& sub) {
    for (auto& e : ev) if (e.label.find(sub) != std::string::npos) return true;
    return false;
}

} // namespace

class DecoderFixtures : public ::testing::TestWithParam<Scenario> {};

TEST_P(DecoderFixtures, MatchesGolden) {
    const Scenario& sc = GetParam();
    const fs::path dir = fixture_dir() / sc.name, golden = fixture_dir() / (sc.name + ".golden");

    if (env("ESCOPE_REGEN_FIXTURES")) {
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(fixture_dir());
        auto made = build(sc.name);
        auto rep = SessionSerializer::save(*made, dir);
        ASSERT_TRUE(rep.ok) << rep.message;
    }

    SessionSerializer::LoadReport lr;
    auto s = SessionSerializer::load(dir, &lr);
    ASSERT_TRUE(s) << "cannot load fixture " << dir << ": " << lr.message;
    EXPECT_EQ(lr.damaged_blocks, 0u);

    const auto ev  = decode(sc, *s);
    const auto got = dump(ev);
    ASSERT_FALSE(ev.empty()) << sc.decoder << " produced nothing";

    if (env("ESCOPE_UPDATE_GOLDEN") || env("ESCOPE_REGEN_FIXTURES")) {
        std::ofstream(golden, std::ios::binary) << got;
        GTEST_SKIP() << "golden updated: " << golden;
    }
    ASSERT_TRUE(fs::exists(golden)) << "missing " << golden << " (run with ESCOPE_UPDATE_GOLDEN=1)";
    EXPECT_EQ(got, slurp(golden)) << "decoded events changed for " << sc.name;
}

// A saved session must decode exactly like the in-memory capture it was written from.
TEST_P(DecoderFixtures, LoadedSessionDecodesLikeTheOriginal) {
    const Scenario& sc = GetParam();
    auto s = SessionSerializer::load(fixture_dir() / sc.name);
    ASSERT_TRUE(s);
    EXPECT_EQ(dump(decode(sc, *s)), dump(decode(sc, *build(sc.name))));
}

INSTANTIATE_TEST_SUITE_P(Captures, DecoderFixtures, ::testing::ValuesIn(scenarios()),
                         [](const auto& i) { return i.param.name; });

// ---- what we know is on the wire, independent of the golden files --------------------------------

namespace {
std::vector<DecodedEvent> events_of(const char* name) {
    for (const auto& sc : scenarios())
        if (sc.name == name) { auto s = SessionSerializer::load(fixture_dir() / name); return s ? decode(sc, *s) : std::vector<DecodedEvent>{}; }
    return {};
}
}

TEST(DecoderFixturePayload, I2C) {
    const auto ev = events_of("i2c_eeprom");
    ASSERT_FALSE(ev.empty());
    EXPECT_TRUE(has(ev, "ADDR 0x50 W ACK"));
    EXPECT_TRUE(has(ev, "ADDR 0x50 R ACK"));
    EXPECT_TRUE(has(ev, "Sr"));
    EXPECT_TRUE(has(ev, "ADDR 0x68 W NACK"));
    std::vector<int> data;
    for (auto& e : by_type(ev, DecodedEvent::Type::Data)) data.push_back(e.value);
    const std::vector<int> want = {0x01, 0x00, 0xDE, 0xAD, 0x01, 0x00, 0xDE, 0xAD};
    EXPECT_EQ(data, want);
}

TEST(DecoderFixturePayload, SPI) {
    const auto ev = events_of("spi_flash");
    ASSERT_FALSE(ev.empty());
    std::vector<int> mosi, miso;
    for (auto& e : by_type(ev, DecodedEvent::Type::Data)) (e.channel == 1 ? mosi : miso).push_back(e.value);
    const std::vector<int> want_mosi = {0x9F, 0x00, 0x00, 0x00, 0x03, 0x00, 0x10, 0x00, 0x00, 0x00};
    const std::vector<int> want_miso = {0x00, 0xEF, 0x40, 0x18, 0x00, 0x00, 0x00, 0x00, 0xCA, 0xFE};
    EXPECT_EQ(mosi, want_mosi);
    EXPECT_EQ(miso, want_miso);
}

TEST(DecoderFixturePayload, UART) {
    const auto ev = events_of("uart_text");
    std::string text;
    for (auto& e : ev) if (!e.is_error && e.value >= 0) text += char(e.value);
    EXPECT_EQ(text, kUartText);
    for (auto& e : ev) EXPECT_FALSE(e.is_error) << e.label;
}

TEST(DecoderFixturePayload, CAN) {
    const auto ev = events_of("can_frames");
    ASSERT_FALSE(ev.empty());
    EXPECT_TRUE(has(ev, "123"));
    EXPECT_TRUE(has(ev, "7FF"));
    bool crc_error = false;
    for (auto& e : ev) if (e.is_error) crc_error = true;
    EXPECT_TRUE(crc_error) << "the damaged frame must be flagged";
}
