#include "session/SessionSerializer.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstring>
#include <algorithm>
#include <thread>

namespace escope {

// ─── Save ─────────────────────────────────────────────────────────────────────

void SessionSerializer::save(const CaptureSession& session,
                             const std::filesystem::path& path) {
    std::filesystem::create_directories(path);
    save_metadata(session, path);
    save_analog(session, path);
    save_digital(session, path);
    std::cout << "[SessionSerializer] Saved to " << path << "\n";
}

void SessionSerializer::save_metadata(const CaptureSession& session,
                                      const std::filesystem::path& dir) {
    const auto& m = session.metadata();
    std::ofstream f(dir / "metadata.json");
    f << "{\n"
      << "  \"format_version\": "  << FORMAT_VERSION       << ",\n"
      << "  \"session_id\": \""    << m.session_id         << "\",\n"
      << "  \"device_name\": \""   << m.device_name        << "\",\n"
      << "  \"firmware_ver\": \""  << m.firmware_ver       << "\",\n"
      << "  \"sample_rate_hz\": "  << m.sample_rate_hz     << ",\n"
      << "  \"digital_rate_hz\": " << m.digital_rate_hz    << ",\n"
      << "  \"timestamp\": \""     << m.timestamp          << "\",\n"
      << "  \"notes\": \""         << m.notes              << "\"\n"
      << "}\n";
}

void SessionSerializer::save_analog(const CaptureSession& session,
                                    const std::filesystem::path& dir) {
    for (std::size_t ch = 0; ch < CaptureSession::MAX_ANALOG_CH; ++ch) {
        std::vector<AnalogSample> samples;
        session.analog_buffer(ch).snapshot(samples);
        if (samples.empty()) continue;

        auto fname = dir / ("analog_ch" + std::to_string(ch) + ".bin");

        // Use a large write buffer to avoid per-sample syscalls
        constexpr std::size_t BUF_BYTES = 4 * 1024 * 1024; // 4 MB
        std::vector<char> wbuf(BUF_BYTES);
        std::ofstream f(fname, std::ios::binary);
        f.rdbuf()->pubsetbuf(wbuf.data(), BUF_BYTES);

        uint64_t count = samples.size();
        f.write(reinterpret_cast<const char*>(&count), sizeof(count));

        // Write entire array in one call — each AnalogSample is 12 bytes
        // (8 byte double + 4 byte float, no padding if aligned)
        // Verify and write field by field to be safe across platforms
        for (const auto& s : samples) {
            f.write(reinterpret_cast<const char*>(&s.timestamp_ns), sizeof(s.timestamp_ns));
            f.write(reinterpret_cast<const char*>(&s.voltage),      sizeof(s.voltage));
        }
    }
}

void SessionSerializer::save_digital(const CaptureSession& session,
                                     const std::filesystem::path& dir) {
    const auto& buf = session.digital_buffer();
    if (buf.total_edges() == 0) return;

    // Record layout (10 bytes, little endian): f64 timestamp, u8 channel, u8 level. Records are
    // written channel by channel (each channel is already in time order), so nothing is merged or
    // sorted, the buffer lock is only held while one channel is copied, and every write is a large
    // block instead of three tiny ones per edge.
    constexpr std::size_t REC = 10, CHUNK = 65536;
    std::ofstream f(dir / "digital_edges.bin", std::ios::binary);
    std::vector<char> wbuf(8u << 20);
    f.rdbuf()->pubsetbuf(wbuf.data(), static_cast<std::streamsize>(wbuf.size()));

    uint64_t count = 0;
    f.write(reinterpret_cast<const char*>(&count), sizeof(count));   // patched below

    std::vector<char> block(CHUNK * REC);
    for (uint8_t ch = 0; ch < buf.num_channels(); ++ch) {
        const auto edges = buf.edges_for_channel(ch);
        for (std::size_t i = 0; i < edges.size(); i += CHUNK) {
            const std::size_t n = std::min(CHUNK, edges.size() - i);
            char* p = block.data();
            for (std::size_t k = 0; k < n; ++k, p += REC) {
                const auto& e = edges[i + k];
                std::memcpy(p, &e.timestamp_ns, sizeof(double));
                p[8] = static_cast<char>(e.channel);
                p[9] = e.rising ? 1 : 0;
            }
            f.write(block.data(), static_cast<std::streamsize>(n * REC));
        }
        count += edges.size();
    }
    f.seekp(0);
    f.write(reinterpret_cast<const char*>(&count), sizeof(count));
}

// ─── Load ─────────────────────────────────────────────────────────────────────

static std::string json_string(const std::string& json, const std::string& key) {
    auto pos = json.find("\"" + key + "\"");
    if (pos == std::string::npos) return "";
    pos = json.find(":", pos);
    if (pos == std::string::npos) return "";
    pos = json.find("\"", pos);
    if (pos == std::string::npos) return "";
    auto end = json.find("\"", pos + 1);
    if (end == std::string::npos) return "";
    return json.substr(pos + 1, end - pos - 1);
}

static double json_number(const std::string& json, const std::string& key) {
    auto pos = json.find("\"" + key + "\"");
    if (pos == std::string::npos) return 0.0;
    pos = json.find(":", pos);
    if (pos == std::string::npos) return 0.0;
    pos = json.find_first_not_of(" \t\n\r", pos + 1);
    return std::stod(json.substr(pos));
}

std::unique_ptr<CaptureSession> SessionSerializer::load(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        std::cerr << "[SessionSerializer] Path not found: " << path << "\n";
        return nullptr;
    }

    auto meta_path = path / "metadata.json";
    if (!std::filesystem::exists(meta_path)) {
        std::cerr << "[SessionSerializer] metadata.json missing\n";
        return nullptr;
    }
    std::ifstream mf(meta_path);
    std::string json((std::istreambuf_iterator<char>(mf)),
                      std::istreambuf_iterator<char>());

    int version = static_cast<int>(json_number(json, "format_version"));
    if (version != FORMAT_VERSION) {
        std::cerr << "[SessionSerializer] Unsupported format version: " << version << "\n";
        return nullptr;
    }

    auto session = std::make_unique<CaptureSession>();
    auto& m = session->metadata();
    m.session_id      = json_string(json, "session_id");
    m.device_name     = json_string(json, "device_name");
    m.firmware_ver    = json_string(json, "firmware_ver");
    m.sample_rate_hz  = json_number(json, "sample_rate_hz");
    m.digital_rate_hz = json_number(json, "digital_rate_hz");
    m.timestamp       = json_string(json, "timestamp");
    m.notes           = json_string(json, "notes");

    for (std::size_t ch = 0; ch < CaptureSession::MAX_ANALOG_CH; ++ch) {
        auto fname = path / ("analog_ch" + std::to_string(ch) + ".bin");
        if (!std::filesystem::exists(fname)) continue;

        std::ifstream f(fname, std::ios::binary);
        if (!f) continue;

        uint64_t count = 0;
        f.read(reinterpret_cast<char*>(&count), sizeof(count));

        constexpr std::size_t BATCH = 4096;
        std::vector<AnalogSample> batch(BATCH);
        uint64_t remaining = count;

        while (remaining > 0 && f.good()) {
            std::size_t n = std::min(remaining, (uint64_t)BATCH);
            for (std::size_t i = 0; i < n; ++i) {
                f.read(reinterpret_cast<char*>(&batch[i].timestamp_ns), sizeof(double));
                f.read(reinterpret_cast<char*>(&batch[i].voltage),      sizeof(float));
            }
            session->analog_buffer(ch).push_batch(batch.data(), n);
            remaining -= n;
        }
        std::cout << "[SessionSerializer] Loaded " << count
                  << " samples for CH" << ch + 1 << "\n";
    }

    auto dig_path = path / "digital_edges.bin";
    if (std::filesystem::exists(dig_path)) {
        std::ifstream f(dig_path, std::ios::binary);
        std::vector<char> rbuf(8u << 20);
        f.rdbuf()->pubsetbuf(rbuf.data(), static_cast<std::streamsize>(rbuf.size()));
        uint64_t count = 0;
        f.read(reinterpret_cast<char*>(&count), sizeof(count));

        // Read big blocks and hand each one to the buffer in a single call; the old loop did three
        // tiny reads and a lock per edge, which took minutes for a long capture.
        constexpr std::size_t REC = 10, CHUNK = 65536;
        std::vector<char> block(CHUNK * REC);
        std::vector<DigitalEdge> batch;
        batch.reserve(CHUNK);
        uint64_t remaining = count;
        while (remaining > 0 && f.good()) {
            const std::size_t want = static_cast<std::size_t>(std::min<uint64_t>(remaining, CHUNK));
            f.read(block.data(), static_cast<std::streamsize>(want * REC));
            const std::size_t got = static_cast<std::size_t>(f.gcount()) / REC;
            if (got == 0) break;
            batch.clear();
            const char* p = block.data();
            for (std::size_t k = 0; k < got; ++k, p += REC) {
                DigitalEdge e{};
                std::memcpy(&e.timestamp_ns, p, sizeof(double));
                e.channel = static_cast<uint8_t>(p[8]);
                e.rising  = p[9] != 0;
                batch.push_back(e);
            }
            session->digital_buffer().push_batch(batch.data(), batch.size());
            remaining -= got;
        }
    }

    session->set_state(CaptureSession::State::Complete);
    std::cout << "[SessionSerializer] Session loaded from " << path << "\n";
    return session;
}

// ─── Export CSV ───────────────────────────────────────────────────────────────
//
// Design decisions:
//   1. Decimate to max_rows (default 10,000) — exporting raw 10 MS/s data
//      produces files nobody can open. 10k rows covers any reasonable analysis.
//   2. Use a large string buffer, flush once — avoids per-row syscalls.
//   3. Pre-format numbers with snprintf (faster than std::fixed streams).
//   4. CH2 lookup: sort once, single linear scan alongside CH1.

void SessionSerializer::export_csv(const CaptureSession& session,
                                   const std::filesystem::path& path,
                                   double t_start_ns, double t_end_ns,
                                   std::size_t max_rows) {
    // ── Snapshot and clip to time window ─────────────────────────────────────
    std::vector<AnalogSample> s0, s1;
    session.analog_buffer(0).snapshot(s0);
    session.analog_buffer(1).snapshot(s1);

    auto clip = [&](std::vector<AnalogSample>& v) {
        if (t_end_ns <= t_start_ns) return;
        auto lo = std::lower_bound(v.begin(), v.end(), t_start_ns,
            [](const AnalogSample& a, double t){ return a.timestamp_ns < t; });
        auto hi = std::upper_bound(lo, v.end(), t_end_ns,
            [](double t, const AnalogSample& a){ return t < a.timestamp_ns; });
        v = std::vector<AnalogSample>(lo, hi);
    };
    clip(s0); clip(s1);

    if (s0.empty()) {
        std::cerr << "[SessionSerializer] No data to export\n";
        return;
    }

    // ── Compute decimation stride ─────────────────────────────────────────────
    // stride=1 means every sample, stride=N means 1-in-N samples
    const std::size_t stride = std::max(std::size_t(1), s0.size() / max_rows);
    const std::size_t n_rows = (s0.size() + stride - 1) / stride;

    // ── Pre-build a fast CH2 lookup by index (parallel arrays same rate) ─────
    // Since both channels run at the same sample rate they have the same stride.
    // Use index-based access — no binary search in the inner loop.
    bool has_ch2 = !s1.empty();

    // ── Write to memory buffer, then flush once ───────────────────────────────
    // Reserve ~60 bytes per row: "123456789.000,-1.234,-1.234\n" = ~30 chars
    std::string buf;
    buf.reserve(n_rows * 40 + 64);

    // -- Rich header (# lines are comments, parseable by EmbeddedScope loader) --
    // Format: KEY=VALUE, one per line
    char hdr[1024];
    std::snprintf(hdr, sizeof(hdr),
        "# EmbeddedScope CSV v1\n"
        "# session_id=%s\n"
        "# device=%s\n"
        "# sample_rate_hz=%.0f\n"
        "# digital_rate_hz=%.0f\n"
        "# timestamp=%s\n"
        "# ch1_label=%s\n"
        "# ch2_label=%s\n"
        "# decimation_stride=%zu\n"
        "# total_raw_samples=%zu\n"
        "# window_start_ns=%.1f\n"
        "# window_end_ns=%.1f\n"
        "# --- data below this line ---\n"
        "timestamp_ns,CH1_V,CH2_V\n",
        session.metadata().session_id.c_str(),
        session.metadata().device_name.c_str(),
        session.metadata().sample_rate_hz,
        session.metadata().digital_rate_hz,
        session.metadata().timestamp.c_str(),
        session.analog_channel_count()>0 ? session.analog_info(0).label.c_str() : "CH1",
        session.analog_channel_count()>1 ? session.analog_info(1).label.c_str() : "CH2",
        stride,
        s0.size() * stride,  // approximate original count
        s0.empty() ? 0.0 : s0.front().timestamp_ns,
        s0.empty() ? 0.0 : s0.back().timestamp_ns
    );
    buf += hdr;

    char row[64];
    for (std::size_t i = 0; i < s0.size(); i += stride) {
        float v2 = 0.f;
        if (has_ch2 && i < s1.size()) v2 = s1[i].voltage;

        // snprintf is ~3x faster than std::fixed stream formatting
        int len = std::snprintf(row, sizeof(row),
            "%.1f,%.4f,%.4f\n",
            s0[i].timestamp_ns, s0[i].voltage, v2);
        buf.append(row, len);
    }

    // Single write — no per-row flush
    std::ofstream f(path);
    f.write(buf.data(), buf.size());

    std::cout << "[SessionSerializer] CSV exported: " << n_rows << " rows"
              << " (1-in-" << stride << " decimation)"
              << " → " << path << "\n";
}

} // namespace escope
