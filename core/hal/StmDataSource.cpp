#include "hal/StmDataSource.h"
#include "hal/BurstFrame.h"
#include "session/CaptureSession.h"
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <dirent.h>
#include <cstring>
#include <cerrno>
#include <algorithm>
#include <cmath>
#include <iostream>

static constexpr uint8_t  PKT_MAGIC     = escope::BURST_MAGIC;
static constexpr uint32_t MAX_SAMPLES   = escope::BURST_MAX_SAMPLES;
static constexpr uint32_t NUM_CHANNELS  = 8;

namespace escope {

StmDataSource::StmDataSource(std::string port) : port_(std::move(port)) {}
StmDataSource::~StmDataSource() { stop(); close(); }

std::vector<DeviceInfo> StmDataSource::enumerate()
{
    std::vector<DeviceInfo> devs;
    DIR* dir = opendir("/dev");
    if (!dir) return devs;
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        std::string name = ent->d_name;
        if (name.substr(0,6) == "ttyACM") {
            DeviceInfo di;
            di.id           = "/dev/" + name;
            di.display_name = "EmbeddedScope (" + di.id + ")";
            di.firmware_ver = "1.0";
            di.connected    = true;
            devs.push_back(di);
        }
    }
    closedir(dir);
    return devs;
}

bool StmDataSource::open_port(const std::string& path)
{
    fd_ = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd_ < 0) return false;
    struct termios tty{};
    tcgetattr(fd_, &tty);
    cfmakeraw(&tty);
    cfsetispeed(&tty, B115200);
    cfsetospeed(&tty, B115200);
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 1;
    tcsetattr(fd_, TCSANOW, &tty);
    int flags = fcntl(fd_, F_GETFL, 0);
    fcntl(fd_, F_SETFL, flags & ~O_NONBLOCK);
    return true;
}

SourceStatus StmDataSource::configure(const CaptureSession& session)
{
    if (fd_ < 0) return SourceStatus::OK;     // not open yet; sent again on start

    const TriggerConfig& tc = session.trigger().config();
    uint8_t mode = 0, ch = 0;              // unless a digital edge trigger is set: D0 rising

    // Trigger position: how many of the burst's 7 segments (32 KB at 48 MS/s =
    // 682.67 us each) come before the trigger. The default config asks for 0, which
    // puts the trigger near the left edge so almost the whole 4.78 ms window is
    // after it and a whole event fits. Not exposed in the GUI on purpose.
    const uint8_t pre = uint8_t(std::clamp(std::lround(tc.pre_trigger_ns / 682666.7), 0L, 6L));

    const int src = int(tc.source) - int(TriggerSource::Digital0);
    if (src >= 0 && src < int(NUM_CHANNELS)) {
        bool known = true;
        switch (tc.condition) {
        case TriggerCondition::RisingEdge:  mode = 0; break;
        case TriggerCondition::FallingEdge: mode = 1; break;
        case TriggerCondition::EitherEdge:  mode = 2; break;
        default: known = false; break;
        }
        if (known) ch = uint8_t(src);
    }

    // Auto = capture anyway after 500 ms (units of 10 ms); Normal/Single = wait for a real trigger.
    const uint8_t auto_10ms = (tc.mode == TriggerMode::Auto) ? 50 : 0;
    uint8_t pkt[8] = {0xC7, 0x01, mode, ch, 0, pre, auto_10ms, 0};
    for (int i = 0; i < 7; i++) pkt[7] ^= pkt[i];
    send_command(pkt);

    uint8_t pulls[8] = {0xC7, 0x02, nopull_mask_, 0, 0, 0, 0, 0};
    for (int i = 0; i < 7; i++) pulls[7] ^= pulls[i];
    send_command(pulls);
    return SourceStatus::OK;
}

void StmDataSource::send_command(const uint8_t (&pkt)[8])
{
    if (fd_ >= 0) { ssize_t n = ::write(fd_, pkt, sizeof(pkt)); (void)n; }
}

void StmDataSource::set_input_nopull(uint8_t mask)
{
    nopull_mask_ = mask;
    uint8_t pulls[8] = {0xC7, 0x02, mask, 0, 0, 0, 0, 0};
    for (int i = 0; i < 7; i++) pulls[7] ^= pulls[i];
    send_command(pulls);
}

SourceStatus StmDataSource::open(const std::string& id)
{
    std::string path = !id.empty() ? id : !port_.empty() ? port_ : "";
    if (path.empty()) {
        auto devs = enumerate();
        if (devs.empty()) return SourceStatus::NotConnected;
        path = devs[0].id;
    }
    if (!open_port(path)) return SourceStatus::DeviceError;
    port_ = path;
    std::cout << "[StmDataSource] Opened " << path << "\n";
    return SourceStatus::OK;
}

void StmDataSource::close()
{
    stop();
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
}

SourceStatus StmDataSource::start(CaptureSession& session)
{
    if (fd_ < 0)  return SourceStatus::NotConnected;
    if (running_) return SourceStatus::OK;
    running_ = true;
    worker_  = std::thread(&StmDataSource::reader_loop, this, &session);
    return SourceStatus::OK;
}

void StmDataSource::stop()
{
    running_ = false;
    if (worker_.joinable()) worker_.join();
}

void StmDataSource::reader_loop(CaptureSession* session)
{
    std::vector<uint8_t> buf;
    buf.reserve(1 << 16);
    uint8_t  tmp[65536];
    uint32_t burst_count = 0;
    BurstClock burst_clock;

    /* Record start time in ns for wall-clock alignment */
    auto t_start = std::chrono::steady_clock::now();

    while (running_) {
        ssize_t n = ::read(fd_, tmp, sizeof(tmp));
        if (n <= 0) {
            if (n < 0 && errno != EAGAIN && errno != EINTR) {
                if (error_cb_) error_cb_(SourceStatus::DeviceError, strerror(errno));
                break;
            }
            continue;
        }
        buf.insert(buf.end(), tmp, tmp + n);

        while (buf.size() >= BURST_HDR_V1) {
            // Find magic byte
            auto it = std::find(buf.begin(), buf.end(), PKT_MAGIC);
            if (it == buf.end()) { buf.clear(); break; }
            if (it != buf.begin()) buf.erase(buf.begin(), it);

            BurstHeader hdr;
            const BurstParse pr = parse_burst_header(buf.data(), buf.size(), hdr);
            if (pr == BurstParse::NeedMore) break;
            if (pr == BurstParse::Bad) {
                buf.erase(buf.begin());   // false magic, resync
                continue;
            }
            const uint16_t flags = hdr.flags;
            const uint32_t rate  = hdr.rate;
            const uint32_t nsamp = hdr.nsamp;
            const uint32_t trig  = hdr.trigger_index;
            const uint32_t seq   = hdr.seq;
            const uint32_t HDR_SIZE = uint32_t(hdr.header_size);

            uint32_t frame_len = HDR_SIZE + nsamp;
            if (buf.size() < frame_len) break;   // wait for the rest of the burst

            // Raw sample data can legitimately contain the magic byte
            // (0xE7) anywhere -- with dense/high-frequency signals this
            // happens often. A false magic hit reads whatever bytes follow
            // it as a header; those 23 bytes are essentially random, but
            // any nsamp <= MAX_SAMPLES still "looks" like a valid frame and
            // gets accepted, committing the parser to wait for however many
            // bytes that bogus nsamp demands. Real bursts stream back to
            // back, so if a next frame is already buffered, its magic byte
            // must sit exactly at this frame's end; if it doesn't, this was
            // a false hit -- reject it and keep searching instead of
            // silently stalling the whole pipeline waiting on garbage.
            if (buf.size() > frame_len && buf[frame_len] != PKT_MAGIC) {
                buf.erase(buf.begin());
                continue;
            }

            burst_count++;
            if (flags & 0x1)
                std::cerr << "[StmDataSource] Burst seq=" << seq
                           << " auto-triggered, no edge seen\n";

            /* Place the burst on the timeline. With firmware version 2 this uses
             * the device clock carried in the header, so the gaps between bursts
             * are exact; arrival time is only used to anchor the first burst
             * (and for version 1 firmware, which has no device clock). Each
             * sample is offset from sample 0 by its index over the sample rate. */
            auto    now      = std::chrono::steady_clock::now();
            double  wall_ns  = std::chrono::duration<double, std::nano>(now - t_start).count();
            double  ns_per_sample = 1e9 / double(rate);
            double  burst_t0_ns   = burst_clock.place(hdr, wall_ns);

            // Decode into a local batch and push it in one call -- taking the
            // digital buffer's lock per individual transition (there can be
            // tens of thousands per burst) serializes against every GUI
            // repaint far more than necessary.
            //
            // A single burst can hold up to MAX_SAMPLES (1M) samples. This
            // used to cap recorded transitions at a low 20,000, decimated
            // evenly across the burst -- but that was only ever needed
            // because the GUI's side panels were doing full-history copies
            // every frame (now fixed: see ChannelPanel/MeasurementPanel and
            // DigitalBuffer::last_edges()/time_range_ns()). At a real
            // burst's actual size (e.g. ~230k samples for this firmware), a
            // dense signal like a 10 MHz clock can have a transition on
            // nearly every sample; decimating that down to 20,000 threw
            // away over 90% of the edges of an otherwise-clean periodic
            // signal, which doesn't just lose detail -- unevenly dropped
            // edges of a periodic signal alias into a waveform that no
            // longer looks like the real one. The cap here is now sized to
            // comfortably hold every transition of a full-density burst up
            // to MAX_SAMPLES, so real signals aren't distorted; it only
            // kicks in as a backstop for a burst denser than that.
            const uint32_t max_ch = std::min<uint32_t>(NUM_CHANNELS, CaptureSession::MAX_DIGITAL_CH);
            const uint8_t  ch_mask = uint8_t((1u << max_ch) - 1);
            static constexpr std::size_t MAX_EDGES_PER_BURST = MAX_SAMPLES;

            const uint8_t* samples = buf.data() + HDR_SIZE;

            std::size_t total_transitions = 0;
            {
                uint8_t p = samples[0];
                for (uint32_t i = 1; i < nsamp; i++) {
                    uint8_t d = (samples[i] ^ p) & ch_mask;
                    if (d) { total_transitions++; p = samples[i]; }
                }
            }
            std::size_t stride = (total_transitions > MAX_EDGES_PER_BURST)
                ? (total_transitions + MAX_EDGES_PER_BURST - 1) / MAX_EDGES_PER_BURST
                : 1;

            std::vector<DigitalEdge> batch;
            batch.reserve(std::min<std::size_t>(total_transitions * max_ch, nsamp * max_ch));

            uint8_t     prev = samples[0];
            std::size_t transition_idx = 0;
            for (uint32_t ch = 0; ch < max_ch; ch++)
                batch.push_back({burst_t0_ns, uint8_t(ch), bool((prev >> ch) & 1), true});

            for (uint32_t i = 1; i < nsamp; i++) {
                uint8_t cur  = samples[i];
                uint8_t diff = (cur ^ prev) & ch_mask;
                if (diff) {
                    if ((transition_idx++ % stride) == 0) {
                        double ts_ns = burst_t0_ns + double(i) * ns_per_sample;
                        for (uint32_t ch = 0; ch < max_ch; ch++) {
                            if (diff >> ch & 1)
                                batch.push_back({ts_ns, uint8_t(ch), bool((cur >> ch) & 1)});
                        }
                    }
                    prev = cur;
                }

                if (i == trig && trigger_cb_ && !(flags & 0x1)) {
                    TriggerEvent evt;
                    evt.timestamp_ns = burst_t0_ns + double(trig) * ns_per_sample;
                    evt.source       = TriggerSource::Digital0;
                    evt.condition    = TriggerCondition::RisingEdge;
                    trigger_cb_(evt);
                }
            }
            session->digital_buffer().push_batch(batch.data(), batch.size());

            buf.erase(buf.begin(), buf.begin() + frame_len);

            /* Bursts at 48 MS/s turn into far more edges per second than the
             * old sparse hardware edge-list ever did. Without eviction a
             * long-running continuous capture accumulates edges forever,
             * and every scan/redraw over that history keeps getting more
             * expensive until the GUI stops responding. Keep a generous
             * rolling window of retained history instead. */
            static constexpr double RETENTION_NS = 30.0 * 1e9; // 30 s
            session->digital_buffer().trim_before(burst_clock.last_end_ns() - RETENTION_NS);

            if (data_cb_) data_cb_();
        }
    }
    std::cout << "[StmDataSource] Stopped. " << burst_count << " bursts.\n";
}

} // namespace escope
