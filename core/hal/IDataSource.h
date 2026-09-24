#pragma once

#include "session/CaptureSession.h"
#include <functional>
#include <string>
#include <vector>

namespace escope {

/// Device information returned by enumeration.
struct DeviceInfo {
    std::string id;           ///< Unique identifier (USB serial, etc.)
    std::string display_name;
    std::string firmware_ver;
    bool        connected = false;
};

/// Status codes for data source operations.
enum class SourceStatus {
    OK,
    NotConnected,
    DeviceError,
    Timeout,
    BufferOverflow,
    Unsupported,
};

/// IDataSource is the hardware abstraction layer interface.
///
/// All acquisition sources — real USB hardware, simulator, file playback —
/// implement this interface. The GUI and acquisition engine never know
/// which concrete source they are talking to.
///
/// Thread model:
///   open() / close() / start() / stop() are called from the main thread.
///   The source pushes data into the CaptureSession on its own internal thread
///   and signals completion via the callbacks registered below.
class IDataSource {
public:
    virtual ~IDataSource() = default;

    // ── Device management ─────────────────────────────────────────────────────

    /// Enumerate available devices. Synchronous.
    virtual std::vector<DeviceInfo> enumerate() = 0;

    /// Open a specific device by id. Empty id = open first available.
    virtual SourceStatus open(const std::string& device_id = "") = 0;

    /// Close the device and release resources.
    virtual void close() = 0;

    virtual bool is_open() const = 0;

    // ── Acquisition control ───────────────────────────────────────────────────

    /// Apply trigger/channel settings and arm the device.
    virtual SourceStatus configure(const CaptureSession& session) = 0;

    /// Start acquisition. Data is pushed into @p session asynchronously.
    virtual SourceStatus start(CaptureSession& session) = 0;

    /// Stop acquisition immediately.
    virtual void stop() = 0;

    virtual bool is_running() const = 0;

    // ── Callbacks (set before start()) ────────────────────────────────────────

    /// Called when new samples are available (on acquisition thread).
    using DataCallback = std::function<void()>;
    virtual void set_data_callback(DataCallback cb) = 0;

    /// Called when an error occurs.
    using ErrorCallback = std::function<void(SourceStatus, const std::string& msg)>;
    virtual void set_error_callback(ErrorCallback cb) = 0;

    /// Called when a trigger fires.
    using TriggerCallback = std::function<void(const TriggerEvent&)>;
    virtual void set_trigger_callback(TriggerCallback cb) = 0;

    // ── Info ──────────────────────────────────────────────────────────────────
    virtual std::string name() const = 0;
};

} // namespace escope
