#include <gtest/gtest.h>
#include "session/CaptureSession.h"
#include "session/EdgeCodec.h"
#include "session/SessionSerializer.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <unistd.h>

using namespace escope;
namespace fs = std::filesystem;

namespace {

// A unique scratch folder that removes itself.
struct TempDir {
    fs::path path;
    TempDir() {
        static int n = 0;
        path = fs::temp_directory_path() / ("escope_sess_test_" + std::to_string(::getpid()) + "_" + std::to_string(n++));
        fs::create_directories(path);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
};

// Realistic edges: a 1 MHz clock plus irregular data, a snapshot at the start of every burst.
void fill(CaptureSession& s, int bursts, int edges_per_burst) {
    std::mt19937_64 rng(42);
    std::vector<double> ends;
    for (int b = 0; b < bursts; ++b) {
        const double t0 = 1e9 + b * 250e6;
        std::vector<DigitalEdge> batch;
        for (uint8_t ch = 0; ch < 3; ++ch) batch.push_back({t0, ch, (rng() & 1) != 0, true});
        double t = t0;
        for (int i = 0; i < edges_per_burst; ++i) {
            t += 20.8333333 * (1 + rng() % 40);
            for (uint8_t ch = 0; ch < 3; ++ch)
                if (rng() % 3 == 0) batch.push_back({t + ch * 0.01, ch, (i & 1) == 0, false});
        }
        s.digital_buffer().push_batch(batch.data(), batch.size());
        ends.push_back(t + 100.0);
    }
    s.digital_buffer().set_burst_ends(ends);
    s.digital_buffer().mark_trigger(1e9 + 12345.0);
}

void expect_same(const DigitalBuffer& a, const DigitalBuffer& b) {
    ASSERT_EQ(a.total_edges(), b.total_edges());
    for (uint8_t ch = 0; ch < a.num_channels(); ++ch) {
        const auto ea = a.edges_for_channel(ch), eb = b.edges_for_channel(ch);
        ASSERT_EQ(ea.size(), eb.size()) << "channel " << int(ch);
        for (std::size_t i = 0; i < ea.size(); ++i) {
            ASSERT_EQ(ea[i].timestamp_ns, eb[i].timestamp_ns) << "channel " << int(ch) << " edge " << i;
            ASSERT_EQ(ea[i].rising, eb[i].rising);
            ASSERT_EQ(ea[i].snapshot, eb[i].snapshot);
        }
    }
    EXPECT_EQ(a.burst_ends(), b.burst_ends());
    EXPECT_EQ(a.last_trigger_ns(), b.last_trigger_ns());
}

} // namespace

TEST(EdgeCodec, Crc32MatchesTheStandardCheckValue) {
    const char* s = "123456789";
    EXPECT_EQ(codec::crc32(s, 9), 0xCBF43926u);          // the published CRC-32 test vector
    EXPECT_EQ(codec::crc32("", 0), 0u);
}

TEST(EdgeCodec, BlockRoundTripIsBitExact) {
    std::vector<DigitalEdge> in;
    const double ts[] = {0.0, 0.0, 1e-9, 20.8333333333333, 20.8333333333334, 1e9, 1e12, 3.5e12, 3.5e12 + 1.0};
    for (std::size_t i = 0; i < sizeof ts / sizeof ts[0]; ++i)
        in.push_back({ts[i], 5, (i % 3) == 0, (i % 4) == 0});
    std::vector<uint8_t> payload;
    codec::encode_block(in.data(), in.size(), payload);
    std::vector<DigitalEdge> out;
    ASSERT_TRUE(codec::decode_block(payload.data(), payload.size(), in.size(), 5, out));
    ASSERT_EQ(out.size(), in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        EXPECT_EQ(out[i].timestamp_ns, in[i].timestamp_ns);
        EXPECT_EQ(out[i].rising, in[i].rising);
        EXPECT_EQ(out[i].snapshot, in[i].snapshot);
        EXPECT_EQ(out[i].channel, 5);
    }
}

TEST(EdgeCodec, NegativeAndUnsortedTimestampsStillRoundTrip) {
    std::vector<DigitalEdge> in = {{-5.5, 0, true, false}, {-1.0, 0, false, false}, {100.0, 0, true, false},
                                   {50.0, 0, false, true}, {-0.0, 0, true, false}};
    std::vector<uint8_t> payload;
    codec::encode_block(in.data(), in.size(), payload);
    std::vector<DigitalEdge> out;
    ASSERT_TRUE(codec::decode_block(payload.data(), payload.size(), in.size(), 0, out));
    for (std::size_t i = 0; i < in.size(); ++i) {
        EXPECT_EQ(std::signbit(out[i].timestamp_ns), std::signbit(in[i].timestamp_ns));
        EXPECT_EQ(out[i].timestamp_ns, in[i].timestamp_ns);
    }
}

TEST(EdgeCodec, MalformedPayloadIsRejected) {
    std::vector<DigitalEdge> in(10, DigitalEdge{1.0, 0, true, false});
    for (int i = 0; i < 10; ++i) in[i].timestamp_ns = i * 7.0;
    std::vector<uint8_t> payload;
    codec::encode_block(in.data(), in.size(), payload);
    std::vector<DigitalEdge> out;
    EXPECT_FALSE(codec::decode_block(payload.data(), payload.size() - 1, in.size(), 0, out));   // truncated
    payload.push_back(0);
    EXPECT_FALSE(codec::decode_block(payload.data(), payload.size(), in.size(), 0, out));       // trailing byte
    EXPECT_FALSE(codec::decode_block(payload.data(), 3, in.size(), 0, out));                    // too short
}

TEST(SessionSerializer, RoundTripKeepsEveryEdgeFlagBurstAndTrigger) {
    TempDir tmp;
    CaptureSession a;
    fill(a, 30, 3000);                                    // enough for several blocks per channel
    const auto sr = SessionSerializer::save(a, tmp.path / "s");
    ASSERT_TRUE(sr.ok) << sr.message;

    CaptureSession* raw = nullptr;
    SessionSerializer::LoadReport lr;
    auto b = SessionSerializer::load(tmp.path / "s", &lr);
    ASSERT_TRUE(b) << lr.message;
    raw = b.get();
    EXPECT_TRUE(lr.ok);
    EXPECT_EQ(lr.damaged_blocks, 0u);
    EXPECT_FALSE(lr.legacy_format);
    expect_same(a.digital_buffer(), raw->digital_buffer());
}

TEST(SessionSerializer, SavingDoesNotLeaveTemporaryFilesBehind) {
    TempDir tmp;
    CaptureSession a;
    fill(a, 3, 100);
    ASSERT_TRUE(SessionSerializer::save(a, tmp.path / "s").ok);
    for (const auto& e : fs::directory_iterator(tmp.path / "s"))
        EXPECT_NE(e.path().extension(), ".tmp") << e.path();
}

TEST(SessionSerializer, FlippedByteIsDetectedAndOnlyThatBlockIsLost) {
    TempDir tmp;
    CaptureSession a;
    fill(a, 40, 3000);
    ASSERT_TRUE(SessionSerializer::save(a, tmp.path / "s").ok);

    const auto file = tmp.path / "s" / "digital_edges.bin";
    const auto size = fs::file_size(file);
    {   // flip one bit in the middle of the file
        std::fstream f(file, std::ios::in | std::ios::out | std::ios::binary);
        f.seekg(static_cast<std::streamoff>(size / 2));
        char c; f.read(&c, 1);
        c ^= 0x10;
        f.seekp(static_cast<std::streamoff>(size / 2));
        f.write(&c, 1);
    }
    SessionSerializer::LoadReport lr;
    auto b = SessionSerializer::load(tmp.path / "s", &lr);
    ASSERT_TRUE(b) << lr.message;
    EXPECT_EQ(lr.damaged_blocks, 1u);
    EXPECT_FALSE(lr.message.empty());
    EXPECT_LT(b->digital_buffer().total_edges(), a.digital_buffer().total_edges());
    // exactly one block (32768 edges at most) is lost, nothing else
    EXPECT_LE(a.digital_buffer().total_edges() - b->digital_buffer().total_edges(), 32768u);
}

TEST(SessionSerializer, TruncatedFileKeepsTheCompleteBlocksAndSaysSo) {
    TempDir tmp;
    CaptureSession a;
    fill(a, 40, 3000);
    ASSERT_TRUE(SessionSerializer::save(a, tmp.path / "s").ok);
    const auto file = tmp.path / "s" / "digital_edges.bin";
    fs::resize_file(file, fs::file_size(file) * 6 / 10);

    SessionSerializer::LoadReport lr;
    auto b = SessionSerializer::load(tmp.path / "s", &lr);
    ASSERT_TRUE(b) << lr.message;
    EXPECT_GE(lr.damaged_blocks, 1u);
    EXPECT_NE(lr.message.find("incomplete"), std::string::npos) << lr.message;
    EXPECT_GT(b->digital_buffer().total_edges(), 0u);
}

TEST(SessionSerializer, DamagedHeaderIsRefusedWithAClearMessage) {
    TempDir tmp;
    CaptureSession a;
    fill(a, 3, 100);
    ASSERT_TRUE(SessionSerializer::save(a, tmp.path / "s").ok);
    {
        std::fstream f(tmp.path / "s" / "digital_edges.bin", std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(13); char c = 99; f.write(&c, 1);
    }
    SessionSerializer::LoadReport lr;
    EXPECT_FALSE(SessionSerializer::load(tmp.path / "s", &lr));
    EXPECT_FALSE(lr.ok);
    EXPECT_NE(lr.message.find("damaged"), std::string::npos) << lr.message;
}

TEST(SessionSerializer, CancelledSaveLeavesNoFileAndCancelledLoadReturnsNothing) {
    TempDir tmp;
    CaptureSession a;
    fill(a, 20, 2000);
    const auto sr = SessionSerializer::save(a, tmp.path / "s", [](double) { return false; });
    EXPECT_FALSE(sr.ok);
    EXPECT_FALSE(fs::exists(tmp.path / "s" / "digital_edges.bin"));
    EXPECT_FALSE(fs::exists(tmp.path / "s" / "metadata.json"));   // nothing claims the save is complete

    ASSERT_TRUE(SessionSerializer::save(a, tmp.path / "t").ok);
    SessionSerializer::LoadReport lr;
    EXPECT_FALSE(SessionSerializer::load(tmp.path / "t", &lr, [](double) { return false; }));
    EXPECT_EQ(lr.message, "Cancelled");
}

TEST(SessionSerializer, ReadsTheOlderVersion1File) {
    TempDir tmp;
    CaptureSession a;
    fill(a, 3, 200);
    ASSERT_TRUE(SessionSerializer::save(a, tmp.path / "s").ok);

    // Replace the digital file by a version 1 one: u64 count, then 10-byte records
    const auto all = a.digital_buffer().all_edges();
    {
        std::ofstream f(tmp.path / "s" / "digital_edges.bin", std::ios::binary | std::ios::trunc);
        const uint64_t n = all.size();
        f.write(reinterpret_cast<const char*>(&n), 8);
        for (const auto& e : all) {
            f.write(reinterpret_cast<const char*>(&e.timestamp_ns), 8);
            f.put(static_cast<char>(e.channel));
            f.put(e.rising ? 1 : 0);
        }
    }
    SessionSerializer::LoadReport lr;
    auto b = SessionSerializer::load(tmp.path / "s", &lr);
    ASSERT_TRUE(b) << lr.message;
    EXPECT_TRUE(lr.legacy_format);
    EXPECT_EQ(b->digital_buffer().total_edges(), a.digital_buffer().total_edges());
}

TEST(SessionSerializer, MissingFolderAndMissingMetadataGiveReadableErrors) {
    TempDir tmp;
    SessionSerializer::LoadReport lr;
    EXPECT_FALSE(SessionSerializer::load(tmp.path / "nope", &lr));
    EXPECT_FALSE(lr.message.empty());
    fs::create_directories(tmp.path / "empty");
    EXPECT_FALSE(SessionSerializer::load(tmp.path / "empty", &lr));
    EXPECT_NE(lr.message.find("metadata.json"), std::string::npos) << lr.message;
}

TEST(SessionSerializer, FileIsMuchSmallerThanTheOldRecordFormat) {
    TempDir tmp;
    CaptureSession a;
    fill(a, 40, 3000);
    const auto sr = SessionSerializer::save(a, tmp.path / "s");
    ASSERT_TRUE(sr.ok);
    const double old_bytes = 8.0 + 10.0 * static_cast<double>(a.digital_buffer().total_edges());
    EXPECT_LT(static_cast<double>(sr.bytes), 0.6 * old_bytes);
}
