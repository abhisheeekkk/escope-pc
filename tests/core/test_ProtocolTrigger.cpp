#include <gtest/gtest.h>
#include "acquisition/ProtocolTrigger.h"
#include "session/SessionSerializer.h"

using namespace escope;
using Kind = ProtocolTriggerConfig::Kind;

namespace {

// All edges of a saved capture in time order, the way a burst arrives from the device.
std::vector<DigitalEdge> burst_of(const char* fixture) {
    auto s = SessionSerializer::load(std::filesystem::path(ESCOPE_FIXTURE_DIR) / fixture);
    if (!s) return {};
    const auto [t0, t1] = s->digital_buffer().time_range_ns();
    auto e = s->digital_buffer().edges_in_range(t0 - 1.0, t1 + 1.0);
    std::stable_sort(e.begin(), e.end(), [](const DigitalEdge& a, const DigitalEdge& b) {
        if (a.snapshot != b.snapshot) return a.snapshot;
        return a.timestamp_ns < b.timestamp_ns;
    });
    return e;
}

ProtocolTriggerConfig i2c(uint32_t addr) {
    ProtocolTriggerConfig c;
    c.kind = Kind::I2CAddress; c.protocol = "I2C"; c.channels = {{"SCL", 0}, {"SDA", 1}}; c.value = addr;
    return c;
}

std::optional<double> run(const ProtocolTriggerConfig& c, const char* fixture, ProtocolTrigger* out = nullptr) {
    ProtocolTrigger t;
    t.set_config(c);
    const auto e = burst_of(fixture);
    EXPECT_FALSE(e.empty());
    auto r = t.evaluate(e.data(), e.size());
    if (out) { out->set_config(c); }
    return r;
}
}

TEST(ProtocolTrigger, I2CAddressMatchesAndGivesTheFrameTime) {
    auto hit = run(i2c(0x50), "i2c_eeprom");
    ASSERT_TRUE(hit);
    EXPECT_NEAR(*hit, 1112083.3, 1.0);                     // first ADDR 0x50 W in the fixture
}

TEST(ProtocolTrigger, I2COtherAddressIsRejected) {
    EXPECT_FALSE(run(i2c(0x51), "i2c_eeprom"));
}

TEST(ProtocolTrigger, I2CDirectionAndAck) {
    auto c = i2c(0x50);
    c.direction = ProtocolTriggerConfig::Direction::Read;
    auto r = run(c, "i2c_eeprom");
    ASSERT_TRUE(r);
    EXPECT_NEAR(*r, 2262100.0, 1000.0);                    // the read comes after the repeated start

    c = i2c(0x68);
    c.ack = ProtocolTriggerConfig::Ack::Ack;
    EXPECT_FALSE(run(c, "i2c_eeprom")) << "0x68 only ever gets a NACK";
    c.ack = ProtocolTriggerConfig::Ack::Nack;
    EXPECT_TRUE(run(c, "i2c_eeprom"));
}

TEST(ProtocolTrigger, UartByte) {
    ProtocolTriggerConfig c;
    c.kind = Kind::UartByte; c.protocol = "UART"; c.channels = {{"TX", 0}}; c.params = {{"baud", "115200"}};
    c.value = 'E';
    EXPECT_TRUE(run(c, "uart_text"));
    c.value = 'z';
    EXPECT_FALSE(run(c, "uart_text"));
}

TEST(ProtocolTrigger, CanIdStandardAndExtended) {
    ProtocolTriggerConfig c;
    c.kind = Kind::CanId; c.protocol = "CAN"; c.channels = {{"RX", 0}}; c.params = {{"bitrate", "500000"}};
    c.value = 0x123;
    EXPECT_TRUE(run(c, "can_frames"));
    c.value = 0x124;
    EXPECT_FALSE(run(c, "can_frames"));
    c.value = 0x1F00AB12; c.extended = true;
    EXPECT_TRUE(run(c, "can_frames"));
    c.extended = false;                                    // same number as an 11-bit id is a different frame
    EXPECT_FALSE(run(c, "can_frames"));
}

TEST(ProtocolTrigger, SpiByteOnTheChosenLine) {
    ProtocolTriggerConfig c;
    c.kind = Kind::SpiByte; c.protocol = "SPI"; c.params = {{"mode", "auto"}};
    c.channels = {{"CLK", 0}, {"MOSI", 1}, {"MISO", 2}, {"CS", 3}};
    c.value = 0x9F; c.spi_line = "MOSI";
    EXPECT_TRUE(run(c, "spi_flash"));
    c.spi_line = "MISO";                                   // 0x9F is only ever sent, never returned
    EXPECT_FALSE(run(c, "spi_flash"));
    c.value = 0xEF;
    EXPECT_TRUE(run(c, "spi_flash"));
}

TEST(ProtocolTrigger, AnyErrorFindsTheDamagedCanFrame) {
    ProtocolTriggerConfig c;
    c.kind = Kind::AnyError; c.protocol = "CAN"; c.channels = {{"RX", 0}}; c.params = {{"bitrate", "500000"}};
    EXPECT_TRUE(run(c, "can_frames"));
    c.protocol = "UART"; c.channels = {{"TX", 0}}; c.params = {{"baud", "115200"}};
    EXPECT_FALSE(run(c, "uart_text")) << "a clean capture has no errors";
}

TEST(ProtocolTrigger, CountsAndRequiredChannels) {
    ProtocolTrigger t;
    EXPECT_FALSE(t.enabled());
    EXPECT_EQ(t.required_mask(), 0);
    EXPECT_FALSE(t.hardware_edge());
    t.set_config(i2c(0x50));
    EXPECT_TRUE(t.enabled());
    EXPECT_EQ(t.required_mask(), 0b11);
    ASSERT_TRUE(t.hardware_edge());
    EXPECT_EQ(t.hardware_edge()->channel, 1);              // SDA
    EXPECT_EQ(t.hardware_edge()->mode, 1);                 // falling
    const auto e = burst_of("i2c_eeprom");
    t.evaluate(e.data(), e.size());
    auto other = i2c(0x42);
    t.set_config(other);                                   // changing the target restarts the counts
    EXPECT_EQ(t.matched() + t.rejected(), 0u);
    t.evaluate(e.data(), e.size());
    EXPECT_EQ(t.rejected(), 1u);
}

TEST(ProtocolTrigger, HardwareEdgePerProtocol) {
    ProtocolTrigger t;
    ProtocolTriggerConfig c;
    c.kind = Kind::UartByte; c.protocol = "UART"; c.channels = {{"RX", 4}};
    t.set_config(c);
    ASSERT_TRUE(t.hardware_edge());
    EXPECT_EQ(t.hardware_edge()->channel, 4);
    c.kind = Kind::SpiByte; c.protocol = "SPI"; c.channels = {{"CLK", 0}, {"MOSI", 1}};
    t.set_config(c);
    ASSERT_TRUE(t.hardware_edge());                        // no chip select: the first clock edge
    EXPECT_EQ(t.hardware_edge()->channel, 0);
    EXPECT_EQ(t.hardware_edge()->mode, 2);
}

TEST(ProtocolTrigger, Describe) {
    auto c = i2c(0x50);
    c.direction = ProtocolTriggerConfig::Direction::Write;
    c.ack = ProtocolTriggerConfig::Ack::Nack;
    EXPECT_EQ(c.describe(), "I2C address 0x50 write NACK");
}
