#include "hal/StmDataSource.h"
#include "session/CaptureSession.h"
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <dirent.h>
#include <cstring>
#include <cerrno>
#include <algorithm>
#include <iostream>

static constexpr uint8_t  PKT_MAGIC    = 0xE5;
static constexpr uint32_t HDR_SIZE     = 4;
static constexpr uint32_t EDGE_SIZE    = 5;
static constexpr uint32_t MAX_EDGES    = 50;

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
    buf.reserve(4096);
    uint8_t  tmp[512];
    uint32_t pkt_count  = 0;
    uint8_t  expect_seq = 0;

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

        while (buf.size() >= HDR_SIZE) {
            // Find magic byte
            auto it = std::find(buf.begin(), buf.end(), PKT_MAGIC);
            if (it == buf.end()) { buf.clear(); break; }
            if (it != buf.begin()) buf.erase(buf.begin(), it);
            if (buf.size() < HDR_SIZE) break;

            uint8_t  seq   = buf[1];
            uint32_t count = (uint32_t(buf[2]) << 8) | buf[3];

            if (count > MAX_EDGES) { buf.erase(buf.begin()); continue; }

            uint32_t pkt_len = HDR_SIZE + count * EDGE_SIZE;
            if (buf.size() < pkt_len) break;

            // Sequence gap check
            if (pkt_count > 0 && seq != expect_seq)
                std::cerr << "[StmDataSource] Gap: expected seq "
                          << (int)expect_seq << " got " << (int)seq << "\n";
            expect_seq = seq + 1;
            pkt_count++;

            // Decode edges
            for (uint32_t i = 0; i < count; i++) {
                const uint8_t* e = buf.data() + HDR_SIZE + i * EDGE_SIZE;
                uint32_t ts_ns = (uint32_t(e[0]) << 24)
                               | (uint32_t(e[1]) << 16)
                               | (uint32_t(e[2]) <<  8)
                               |  uint32_t(e[3]);
                uint8_t ch    = e[4] & 0x7F;
                bool    level = (e[4] >> 7) & 1;

                if (ch < CaptureSession::MAX_DIGITAL_CH)
                    session->digital_buffer().push_edge((double)ts_ns, ch, level);
            }

            buf.erase(buf.begin(), buf.begin() + pkt_len);
            if (data_cb_) data_cb_();
        }
    }
    std::cout << "[StmDataSource] Stopped. " << pkt_count << " packets.\n";
}

} // namespace escope
