#include "session/SessionSerializer.h"
#include "session/EdgeCodec.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
#include <functional>
#include <vector>
#if defined(__unix__) || defined(__APPLE__)
#  include <fcntl.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstring>
#include <algorithm>
#include <thread>

namespace escope {

namespace {

using Clock = std::chrono::steady_clock;
double seconds_since(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

constexpr uint32_t BLOCK_EDGES = 32768;
constexpr char     MAGIC[4]    = {'E', 'S', 'D', '2'};
constexpr uint32_t SECTION_TAG = 0x4E414843;          // "CHAN"
constexpr std::size_t HEADER_BYTES  = 64;
constexpr std::size_t SECTION_BYTES = 24;
constexpr std::size_t BLOCK_HDR     = 12;

struct Cancelled {};

// ---- little-endian field helpers ---------------------------------------------------------
template <typename T> void put(uint8_t* p, T v) { std::memcpy(p, &v, sizeof(T)); }
template <typename T> T    get(const uint8_t* p) { T v; std::memcpy(&v, p, sizeof(T)); return v; }

/// Run fn(i) for i in [0, n) on up to hardware_concurrency() threads.
void parallel_for(std::size_t n, const std::function<void(std::size_t)>& fn) {
    if (n == 0) return;
    const std::size_t workers = std::min<std::size_t>(n, std::max(1u, std::thread::hardware_concurrency()));
    if (workers == 1) { for (std::size_t i = 0; i < n; ++i) fn(i); return; }
    std::atomic<std::size_t> next{0};
    std::vector<std::thread> pool;
    for (std::size_t w = 0; w < workers; ++w)
        pool.emplace_back([&] { for (std::size_t i; (i = next.fetch_add(1)) < n;) fn(i); });
    for (auto& t : pool) t.join();
}

/// Writes to "<name>.tmp", flushes it to disk and renames it over <name> only on commit(), so an
/// interrupted save leaves the previous file untouched.
class AtomicFile {
public:
    explicit AtomicFile(std::filesystem::path final_path)
        : final_(std::move(final_path)), tmp_(final_.string() + ".tmp") {
        f_ = std::fopen(tmp_.string().c_str(), "wb");
        if (!f_) throw std::runtime_error("cannot create " + tmp_.string());
        std::setvbuf(f_, nullptr, _IOFBF, 8u << 20);
    }
    ~AtomicFile() { if (f_) { std::fclose(f_); std::error_code ec; std::filesystem::remove(tmp_, ec); } }
    AtomicFile(const AtomicFile&) = delete;
    AtomicFile& operator=(const AtomicFile&) = delete;

    void write(const void* p, std::size_t n) {
        if (n && std::fwrite(p, 1, n, f_) != n) throw std::runtime_error("write failed (disk full?)");
        bytes_ += n;
    }
    void patch(std::size_t offset, const void* p, std::size_t n) {
        if (std::fseek(f_, static_cast<long>(offset), SEEK_SET) != 0 || std::fwrite(p, 1, n, f_) != n)
            throw std::runtime_error("write failed (disk full?)");
        std::fseek(f_, 0, SEEK_END);
    }
    std::size_t bytes() const { return bytes_; }
    void commit() {
        if (std::fflush(f_) != 0) throw std::runtime_error("flush failed (disk full?)");
#if defined(__unix__) || defined(__APPLE__)
#ifndef ESCOPE_NO_FSYNC
        ::fsync(::fileno(f_));
#endif
#endif
        std::fclose(f_); f_ = nullptr;
        std::filesystem::rename(tmp_, final_);
    }
private:
    std::filesystem::path final_, tmp_;
    std::FILE* f_ = nullptr;
    std::size_t bytes_ = 0;
};

/// Read-only view of a whole file: memory-mapped where the OS allows, otherwise read into memory.
class MappedFile {
public:
    explicit MappedFile(const std::filesystem::path& p) {
#if defined(__unix__) || defined(__APPLE__)
        const int fd = ::open(p.string().c_str(), O_RDONLY);
        if (fd >= 0) {
            struct stat st {};
            if (::fstat(fd, &st) == 0 && st.st_size > 0) {
                void* m = ::mmap(nullptr, static_cast<std::size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
                if (m != MAP_FAILED) {
                    map_ = m; size_ = static_cast<std::size_t>(st.st_size);
                    data_ = static_cast<const uint8_t*>(m);
                    ::madvise(m, size_, MADV_SEQUENTIAL);
                }
            }
            ::close(fd);
            if (data_) return;
        }
#endif
        std::ifstream f(p, std::ios::binary | std::ios::ate);
        if (!f) return;
        buf_.resize(static_cast<std::size_t>(f.tellg()));
        f.seekg(0);
        f.read(reinterpret_cast<char*>(buf_.data()), static_cast<std::streamsize>(buf_.size()));
        data_ = buf_.data(); size_ = buf_.size();
    }
    ~MappedFile() {
#if defined(__unix__) || defined(__APPLE__)
        if (map_) ::munmap(map_, size_);
#endif
    }
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    const uint8_t* data() const { return data_; }
    std::size_t    size() const { return size_; }
private:
    const uint8_t* data_ = nullptr;
    std::size_t    size_ = 0;
    void*          map_  = nullptr;
    std::vector<uint8_t> buf_;
};

} // namespace

// ─── Save ─────────────────────────────────────────────────────────────────────

SessionSerializer::SaveReport SessionSerializer::save(const CaptureSession& session,
                                                      const std::filesystem::path& path,
                                                      ProgressFn progress) {
    SaveReport rep;
    const auto t0 = Clock::now();
    try {
        std::filesystem::create_directories(path);
        save_analog(session, path);
        save_digital(session, path, progress, rep.bytes);
        save_metadata(session, path);                       // last: its presence marks a complete save
        rep.ok = true;
        rep.message = "Saved";
        std::cout << "[SessionSerializer] Saved to " << path << "\n";
    } catch (const Cancelled&) {
        rep.message = "Cancelled";
    } catch (const std::exception& e) {
        rep.message = e.what();
    }
    rep.seconds = seconds_since(t0);
    return rep;
}

void SessionSerializer::save_metadata(const CaptureSession& session,
                                      const std::filesystem::path& dir) {
    const auto& m = session.metadata();
    std::ostringstream f;
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
    const std::string text = f.str();
    AtomicFile out(dir / "metadata.json");
    out.write(text.data(), text.size());
    out.commit();
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

void SessionSerializer::save_digital(const CaptureSession& session, const std::filesystem::path& dir,
                                     const ProgressFn& progress, std::size_t& bytes) {
    const auto& buf = session.digital_buffer();
    const std::size_t grand_total = buf.total_edges();
    const auto final_path = dir / "digital_edges.bin";
    if (grand_total == 0) {                                  // nothing recorded: drop any stale file
        std::error_code ec; std::filesystem::remove(final_path, ec);
        return;
    }

    AtomicFile out(final_path);
    uint8_t header[HEADER_BYTES] = {};                       // written for real at the end
    out.write(header, sizeof header);

    // burst boundaries and trigger, so decoding a loaded session matches the live one
    const std::vector<double> bursts = buf.burst_ends();
    out.write(bursts.data(), bursts.size() * sizeof(double));
    const uint32_t bursts_crc = codec::crc32(bursts.data(), bursts.size() * sizeof(double));
    out.write(&bursts_crc, sizeof bursts_crc);

    uint64_t total_edges = 0;
    std::size_t done = 0;
    const uint32_t channels = buf.num_channels();
    for (uint32_t ch = 0; ch < channels; ++ch) {
        const std::vector<DigitalEdge> edges = buf.edges_for_channel(static_cast<uint8_t>(ch));
        const uint32_t blocks = static_cast<uint32_t>((edges.size() + BLOCK_EDGES - 1) / BLOCK_EDGES);

        uint8_t sec[SECTION_BYTES] = {};
        put<uint32_t>(sec + 0, SECTION_TAG);
        put<uint32_t>(sec + 4, ch);
        put<uint64_t>(sec + 8, edges.size());
        put<uint32_t>(sec + 16, blocks);
        put<uint32_t>(sec + 20, codec::crc32(sec, 20));
        out.write(sec, sizeof sec);

        // Encode the blocks on all cores, then write them in order
        std::vector<std::vector<uint8_t>> enc(blocks);
        std::vector<uint32_t> crcs(blocks), counts(blocks);
        parallel_for(blocks, [&](std::size_t b) {
            const std::size_t first = b * BLOCK_EDGES;
            const std::size_t n = std::min<std::size_t>(BLOCK_EDGES, edges.size() - first);
            codec::encode_block(edges.data() + first, n, enc[b]);
            crcs[b] = codec::crc32(enc[b].data(), enc[b].size());
            counts[b] = static_cast<uint32_t>(n);
        });
        for (uint32_t b = 0; b < blocks; ++b) {
            uint8_t bh[BLOCK_HDR];
            put<uint32_t>(bh + 0, counts[b]);
            put<uint32_t>(bh + 4, static_cast<uint32_t>(enc[b].size()));
            put<uint32_t>(bh + 8, crcs[b]);
            out.write(bh, sizeof bh);
            out.write(enc[b].data(), enc[b].size());
        }
        total_edges += edges.size();
        done += edges.size();
        if (progress && !progress(static_cast<double>(done) / static_cast<double>(grand_total)))
            throw Cancelled{};
    }

    put<char>(header + 0, MAGIC[0]); put<char>(header + 1, MAGIC[1]);
    put<char>(header + 2, MAGIC[2]); put<char>(header + 3, MAGIC[3]);
    put<uint32_t>(header + 4, 2);
    put<uint32_t>(header + 8, BLOCK_EDGES);
    put<uint32_t>(header + 12, channels);
    put<uint64_t>(header + 16, total_edges);
    put<uint64_t>(header + 24, bursts.size());
    put<double>(header + 32, buf.last_trigger_ns());
    put<uint32_t>(header + 60, codec::crc32(header, 60));
    out.patch(0, header, sizeof header);
    bytes = out.bytes();
    out.commit();
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

namespace {

/// Version 1 digital file: a u64 count, then 10-byte records. No checksums, no burst boundaries.
bool load_digital_v1(const std::filesystem::path& file, CaptureSession& session,
                     SessionSerializer::LoadReport& rep, const SessionSerializer::ProgressFn& progress) {
    rep.legacy_format = true;
    std::ifstream f(file, std::ios::binary);
    std::vector<char> rbuf(8u << 20);
    f.rdbuf()->pubsetbuf(rbuf.data(), static_cast<std::streamsize>(rbuf.size()));
    uint64_t count = 0;
    f.read(reinterpret_cast<char*>(&count), sizeof(count));
    if (!f) { rep.message = "The digital data file is empty or unreadable."; return false; }

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
        session.digital_buffer().push_batch(batch.data(), batch.size());
        remaining -= got;
        if (progress && !progress(1.0 - static_cast<double>(remaining) / static_cast<double>(count))) {
            rep.message = "Cancelled";
            return false;
        }
    }
    if (remaining > 0) {
        rep.message = "The digital data file is shorter than it should be; " + std::to_string(remaining) +
                      " edges are missing.";
        rep.damaged_blocks = 1;
    }
    return true;
}

struct BlockRef {
    const uint8_t* payload;
    uint32_t       n, len, crc;
    uint32_t       channel;
};

bool load_digital(const std::filesystem::path& file, CaptureSession& session,
                  SessionSerializer::LoadReport& rep, const SessionSerializer::ProgressFn& progress) {
    MappedFile mf(file);
    if (!mf.data() || mf.size() < 4) { rep.message = "The digital data file is empty or unreadable."; return false; }
    const uint8_t* d = mf.data();
    const std::size_t size = mf.size();

    if (std::memcmp(d, MAGIC, 4) != 0) {
        const bool ok = load_digital_v1(file, session, rep, progress);
        return ok;
    }

    auto truncated = [&](const std::string& what) {
        rep.message = "The digital data file is incomplete (" + what + "). The recording may have been interrupted.";
        ++rep.damaged_blocks;
    };

    // ---- header -------------------------------------------------------------------------------
    if (size < HEADER_BYTES) { rep.message = "The digital data file is truncated."; return false; }
    if (get<uint32_t>(d + 60) != codec::crc32(d, 60)) { rep.message = "The digital data header is damaged."; return false; }
    if (get<uint32_t>(d + 4) != 2) { rep.message = "Unsupported digital data version."; return false; }
    const uint32_t channels = get<uint32_t>(d + 12);
    const uint64_t burst_count = get<uint64_t>(d + 24);
    const double   last_trigger = get<double>(d + 32);
    if (channels == 0 || channels > 16) { rep.message = "The digital data header is damaged."; return false; }

    std::size_t pos = HEADER_BYTES;
    const std::size_t burst_bytes = static_cast<std::size_t>(burst_count) * sizeof(double);
    if (burst_count > size || pos + burst_bytes + 4 > size) { rep.message = "The digital data file is truncated."; return false; }
    std::vector<double> bursts(static_cast<std::size_t>(burst_count));
    std::memcpy(bursts.data(), d + pos, burst_bytes);
    pos += burst_bytes;
    if (get<uint32_t>(d + pos) != codec::crc32(bursts.data(), burst_bytes)) {
        bursts.clear();                                     // burst boundaries are an aid; keep going without them
        ++rep.damaged_blocks;
        rep.message = "The burst boundaries were damaged; decoding may be less accurate.";
    }
    pos += 4;

    // ---- index every block (cheap: headers only) -----------------------------------------------
    std::vector<BlockRef> refs;
    constexpr std::size_t UNSET = static_cast<std::size_t>(-1);
    std::vector<std::size_t> first_ref(channels + 1, UNSET);
    bool cut = false;
    for (uint32_t ch = 0; ch < channels && !cut; ++ch) {
        first_ref[ch] = refs.size();
        if (pos + SECTION_BYTES > size) { truncated("missing channel " + std::to_string(ch)); cut = true; break; }
        const uint8_t* s = d + pos;
        if (get<uint32_t>(s) != SECTION_TAG || get<uint32_t>(s + 20) != codec::crc32(s, 20) ||
            get<uint32_t>(s + 4) != ch) {
            truncated("damaged section for channel " + std::to_string(ch)); cut = true; break;
        }
        const uint32_t blocks = get<uint32_t>(s + 16);
        pos += SECTION_BYTES;
        for (uint32_t b = 0; b < blocks; ++b) {
            if (pos + BLOCK_HDR > size) { truncated("channel " + std::to_string(ch)); cut = true; break; }
            const uint8_t* h = d + pos;
            BlockRef r{h + BLOCK_HDR, get<uint32_t>(h), get<uint32_t>(h + 4), get<uint32_t>(h + 8), ch};
            if (pos + BLOCK_HDR + r.len > size || r.n == 0 || r.n > (1u << 20)) {
                truncated("channel " + std::to_string(ch)); cut = true; break;
            }
            refs.push_back(r);
            pos += BLOCK_HDR + r.len;
        }
    }
    // channels that were never reached start (and end) where the list ends
    for (auto& f : first_ref) if (f == UNSET) f = refs.size();
    first_ref[channels] = refs.size();
    rep.total_blocks = refs.size();

    // ---- decode and verify every block on all cores ----------------------------------------
    std::vector<std::vector<DigitalEdge>> decoded(refs.size());
    std::vector<uint8_t> bad(refs.size(), 0);
    std::atomic<std::size_t> finished{0};
    std::atomic<bool> stop{false};
    std::thread coordinator([&] {
        parallel_for(refs.size(), [&](std::size_t i) {
            if (stop.load(std::memory_order_relaxed)) return;
            const BlockRef& r = refs[i];
            if (codec::crc32(r.payload, r.len) != r.crc ||
                !codec::decode_block(r.payload, r.len, r.n, static_cast<uint8_t>(r.channel), decoded[i])) {
                bad[i] = 1;
                decoded[i].clear();
            }
            finished.fetch_add(1, std::memory_order_relaxed);
        });
    });
    while (finished.load() < refs.size() && !stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        if (progress && !progress(0.9 * static_cast<double>(finished.load()) / static_cast<double>(std::max<std::size_t>(1, refs.size()))))
            stop.store(true);
    }
    coordinator.join();
    if (stop.load()) { rep.message = "Cancelled"; return false; }

    // ---- assemble each channel's history in parallel -------------------------------------------
    std::atomic<std::size_t> assembled{0};
    parallel_for(channels, [&](std::size_t ch) {
        std::deque<DigitalEdge> edges;
        for (std::size_t i = first_ref[ch]; i < first_ref[ch + 1]; ++i) {
            if (bad[i]) continue;
            edges.insert(edges.end(), decoded[i].begin(), decoded[i].end());
            std::vector<DigitalEdge>().swap(decoded[i]);        // give the memory back as we go
        }
        session.digital_buffer().assign_channel(static_cast<uint8_t>(ch), std::move(edges));
        assembled.fetch_add(1);
    });
    if (progress) progress(1.0);

    std::size_t damaged = 0;
    for (uint8_t b : bad) damaged += b;
    rep.damaged_blocks += damaged;
    if (!bursts.empty()) session.digital_buffer().set_burst_ends(std::move(bursts));
    if (last_trigger >= 0.0) session.digital_buffer().mark_trigger(last_trigger);
    if (damaged > 0) {
        rep.message = std::to_string(damaged) + " of " + std::to_string(refs.size()) +
                      " data blocks failed their checksum and were skipped. The waveform has gaps there.";
    }
    return true;
}

} // namespace

std::unique_ptr<CaptureSession> SessionSerializer::load(const std::filesystem::path& path,
                                                        LoadReport* report, ProgressFn progress) {
    LoadReport local;
    LoadReport* rep = report ? report : &local;
    *rep = LoadReport{};
    const auto t0 = Clock::now();
    auto fail = [&](const std::string& why) {
        rep->ok = false;
        rep->message = why;
        std::cerr << "[SessionSerializer] " << why << "\n";
    };
    if (!std::filesystem::exists(path)) {
        fail("The folder does not exist: " + path.string());
        return nullptr;
    }

    auto meta_path = path / "metadata.json";
    if (!std::filesystem::exists(meta_path)) {
        fail("This folder is not a session (metadata.json is missing). If a save was interrupted, the data may be incomplete.");
        return nullptr;
    }
    std::ifstream mf(meta_path);
    std::string json((std::istreambuf_iterator<char>(mf)),
                      std::istreambuf_iterator<char>());

    int version = static_cast<int>(json_number(json, "format_version"));
    if (version < 1 || version > FORMAT_VERSION) {
        fail("This session was written by a newer version of EmbeddedScope (format " + std::to_string(version) + ").");
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

    const auto dig_path = path / "digital_edges.bin";
    if (std::filesystem::exists(dig_path)) {
        if (!load_digital(dig_path, *session, *rep, progress)) return nullptr;
    }

    session->set_state(CaptureSession::State::Complete);
    rep->ok = true;
    rep->edges = session->digital_buffer().total_edges();
    rep->seconds = seconds_since(t0);
    if (rep->damaged_blocks == 0) rep->message = "Loaded";
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
